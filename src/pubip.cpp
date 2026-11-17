#include "pubip.h"

#include <HTTPClient.h>
#include <IPAddress.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "net.h"
#include "settings.h"
#include "storage.h"
#include "timekeeper.h"

namespace pubip {
namespace {

constexpr char FILE_PATH[] = "/pubip.bin";
constexpr char TMP_PATH[] = "/pubip.tmp";
constexpr uint32_t FILE_MAGIC = 0x50495055;  // "UPIP"
constexpr size_t MAX_ANSWER = 63;            // an address is at most 15 characters

struct FileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t current;
};

// Guards everything below: the lookup task writes, the web server reads.
SemaphoreHandle_t lock = nullptr;
Change changes[CAPACITY];  // oldest first
size_t changeCount = 0;
uint32_t current = 0;
uint32_t checkedAt = 0;
char status[64] = "Not checked yet";

volatile bool lookupNow = false;       // an outage just ended
volatile bool outageSinceLookup = false;
volatile bool fakeNext = false;         // serial test: pretend the address changed

// Collects the start of a response body and drops the rest, so a service
// that answers with a whole web page cannot use up the memory.
class Answer : public Stream {
 public:
  size_t write(uint8_t c) override {
    if (len < MAX_ANSWER) text[len++] = c;
    return 1;
  }
  size_t write(const uint8_t *data, size_t size) override {
    for (size_t i = 0; i < size; i++) write(data[i]);
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  char text[MAX_ANSWER + 1] = {0};
  size_t len = 0;
};

bool loadFrom(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  FileHeader h;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            h.magic == FILE_MAGIC && h.version == 1 && h.count <= CAPACITY;
  if (ok) {
    size_t bytes = h.count * sizeof(Change);
    ok = f.read(reinterpret_cast<uint8_t *>(changes), bytes) == bytes;
    if (ok) {
      changeCount = h.count;
      current = h.current;
    }
  }
  f.close();
  return ok;
}

// Temp file then rename, so a power cut leaves the old file or the new one.
bool persist() {
  File f = LittleFS.open(TMP_PATH, "w");
  if (!f) return false;
  FileHeader h = {FILE_MAGIC, 1, static_cast<uint16_t>(changeCount), current};
  size_t bytes = changeCount * sizeof(Change);
  bool ok = f.write(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            f.write(reinterpret_cast<uint8_t *>(changes), bytes) == bytes;
  f.close();
  if (!ok) return false;
  if (!LittleFS.rename(TMP_PATH, FILE_PATH)) {
    LittleFS.remove(FILE_PATH);
    ok = LittleFS.rename(TMP_PATH, FILE_PATH);
  }
  return ok;
}

void setStatus(const char *text) {
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(status, text, sizeof(status));
  xSemaphoreGive(lock);
}

// One GET. Returns the address (network byte order) or 0, with the reason
// left in `why`.
uint32_t ask(const String &url, char *why, size_t whyLen) {
  Answer answer;
  int code;
  {
    net::TlsLock one;  // one TLS session at a time; each needs a lot of memory
    WiFiClientSecure client;
    client.setCACert(LOOKUP_ROOT_CAS);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setUserAgent("uptime-sentinel");
    if (!http.begin(client, url)) {
      strlcpy(why, "the URL is not usable", whyLen);
      return 0;
    }
    code = http.GET();
    if (code == HTTP_CODE_OK) http.writeToStream(&answer);
    http.end();
  }
  if (code < 0) {
    strlcpy(why, "could not connect, or its certificate was not accepted", whyLen);
    return 0;
  }
  if (code != HTTP_CODE_OK) {
    snprintf(why, whyLen, "it answered HTTP %d", code);
    return 0;
  }
  String text = answer.text;
  text.trim();
  IPAddress ip;
  if (text.length() < 7 || text.length() > 15 || !ip.fromString(text) ||
      static_cast<uint32_t>(ip) == 0) {
    strlcpy(why, "the answer was not an IPv4 address", whyLen);
    return 0;
  }
  return static_cast<uint32_t>(ip);
}

void record(uint32_t addr, int service) {
  bool changed = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  checkedAt = timekeeper::now();
  snprintf(status, sizeof(status), "OK, answered by lookup service %d", service);
  if (current == 0) {
    current = addr;  // the first address ever seen is not a change
    persist();
  } else if (addr != current) {
    if (changeCount == CAPACITY) {
      memmove(&changes[0], &changes[1], (CAPACITY - 1) * sizeof(Change));
      changeCount--;
    }
    changes[changeCount++] = {checkedAt, current, addr};
    current = addr;
    changed = true;
    persist();
  }
  xSemaphoreGive(lock);

  // The address itself stays out of the serial log.
  if (changed) Serial.println("[ip] public address changed; see the dashboard");
  if (changed && outageSinceLookup) storage::markNewAddress();
  outageSinceLookup = false;
}

void task(void *) {
  const settings::Config &conf = settings::get();
  uint32_t nextAt = 0;  // seconds since boot
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool due = lookupNow || timekeeper::mono() >= nextAt;
    // Certificates cannot be checked without the date, so wait for the clock.
    if (!due || !net::up() || !timekeeper::valid()) continue;
    lookupNow = false;

    char why[64] = "";
    char line[64];
    uint32_t addr = 0;
    int service = 0;
    for (int i = 0; i < 2 && addr == 0; i++) {
      if (conf.ipUrl[i].length() == 0) continue;
      addr = ask(conf.ipUrl[i], why, sizeof(why));
      service = i + 1;
    }
    if (addr != 0) {
      if (fakeNext) {
        fakeNext = false;
        addr = static_cast<uint32_t>(IPAddress(203, 0, 113, 7));  // a documentation address
      }
      record(addr, service);
      nextAt = timekeeper::mono() + cfg::PUBIP_INTERVAL_SEC;
    } else {
      snprintf(line, sizeof(line), "Failed: %s", why[0] ? why : "no lookup service is set");
      setStatus(line);
      Serial.printf("[ip] lookup failed: %s\n", why[0] ? why : "no lookup service is set");
      nextAt = timekeeper::mono() + cfg::PUBIP_RETRY_SEC;
    }
  }
}

}  // namespace

void begin() {
  lock = xSemaphoreCreateMutex();
  if (!loadFrom(FILE_PATH)) loadFrom(TMP_PATH);
  LittleFS.remove(TMP_PATH);
  if (!settings::get().ipEnabled) return;
  xTaskCreate(task, "pubip", 12288, nullptr, 1, nullptr);
}

void afterOutage() {
  outageSinceLookup = true;
  lookupNow = true;
}

void simulateChange() { fakeNext = true; }

void clear() {
  xSemaphoreTake(lock, portMAX_DELAY);
  changeCount = 0;
  current = 0;
  persist();
  xSemaphoreGive(lock);
  lookupNow = true;
}

void snapshot(Snapshot &out) {
  out.enabled = settings::get().ipEnabled;
  xSemaphoreTake(lock, portMAX_DELAY);
  out.current = current;
  out.checkedAt = checkedAt;
  strlcpy(out.status, status, sizeof(out.status));
  out.count = changeCount;
  for (size_t i = 0; i < changeCount; i++) out.changes[i] = changes[changeCount - 1 - i];
  xSemaphoreGive(lock);
}

}  // namespace pubip
