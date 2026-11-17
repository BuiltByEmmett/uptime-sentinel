#include "sites.h"

#include <LittleFS.h>
#include <math.h>

#include "alerts.h"
#include "config.h"
#include "fetch.h"
#include "monitor.h"
#include "net.h"
#include "storage.h"
#include "timekeeper.h"

namespace sites {
namespace {

constexpr char PATH[] = "/sites.bin";
constexpr char TMP_PATH[] = "/sites.tmp";
constexpr uint32_t MAGIC = 0x54535055;  // "UPST"
constexpr size_t N = settings::MAX_SITES;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
};

// What is kept on flash for each site.
struct Slot {
  uint32_t urlHash;        // the address these figures belong to
  uint32_t since;
  uint32_t downStart;      // 0 = not down
  uint32_t certExpiry;
  uint32_t certWarnedFor;  // the expiry time the warnings below were about
  uint8_t certWarnLevel;   // 0 none, 1 = two-week warning sent, 2 = three-day warning sent
  uint8_t downCount;
  uint8_t reserved[2];
  Down downs[LOG_LEN];     // oldest first
};

// What is only kept while running.
struct Live {
  State state = State::WAITING;
  uint8_t fails = 0;
  int16_t code = 0;
  int32_t ms = -1;
  char reason[28] = "";
  uint32_t checkedAt = 0;
  uint32_t checkedMono = 0;
  bool everChecked = false;
  uint32_t firstFailAt = 0;
};

struct Result {
  bool ok;
  int16_t code;
  int32_t ms;
  uint32_t certExpiry;
  char reason[28];
};

Slot slots[N];
Live live[N];
SemaphoreHandle_t mutex = nullptr;
volatile bool simCert = false;
uint32_t simCertExpiry = 0;

uint32_t hashOf(const String &text) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < text.length(); i++) h = (h ^ static_cast<uint8_t>(text[i])) * 16777619u;
  return h ? h : 1;
}

bool load(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  Header h;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) && h.magic == MAGIC &&
            h.version == 1 && h.count == N &&
            f.read(reinterpret_cast<uint8_t *>(slots), sizeof(slots)) == sizeof(slots);
  f.close();
  if (!ok) memset(slots, 0, sizeof(slots));
  return ok;
}

bool persist() {
  if (!storage::available()) return false;
  File f = LittleFS.open(TMP_PATH, "w");
  if (!f) return false;
  Header h = {MAGIC, 1, static_cast<uint16_t>(N)};
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = f.write(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            f.write(reinterpret_cast<uint8_t *>(slots), sizeof(slots)) == sizeof(slots);
  xSemaphoreGive(mutex);
  f.close();
  if (!ok) return false;
  if (!LittleFS.rename(TMP_PATH, PATH)) {
    LittleFS.remove(PATH);
    ok = LittleFS.rename(TMP_PATH, PATH);
  }
  return ok;
}

// A check passes when the server answers with anything below 500.
Result check(const String &address) {
  fetch::Reply reply = fetch::request(address, "GET", "", nullptr, TIMEOUT_MS);
  Result r = {false, reply.code, reply.ms, reply.certExpiry, ""};
  if (reply.error != fetch::Error::NONE) {
    strlcpy(r.reason, reply.detail, sizeof(r.reason));
  } else if (reply.code >= 500) {
    snprintf(r.reason, sizeof(r.reason), "HTTP %d", reply.code);
  } else {
    r.ok = true;
  }
  return r;
}

void say(const char *event, const char *text) {
  Serial.printf("[site] %s\n", text);
  alerts::notify(event, text);
}

// Takes one result into the site's state. Returns true when something that
// is kept on flash changed.
bool apply(size_t i, const Result &r, const String &name) {
  uint32_t now = timekeeper::now();
  bool dirty = false;
  char text[160];
  xSemaphoreTake(mutex, portMAX_DELAY);
  Slot &s = slots[i];
  Live &l = live[i];
  l.code = r.code;
  l.ms = r.ms;
  strlcpy(l.reason, r.reason, sizeof(l.reason));
  l.checkedAt = now;
  if (s.since == 0) {
    s.since = now;
    dirty = true;
  }

  const char *event = nullptr;
  if (r.ok) {
    l.fails = 0;
    if (l.state == State::DOWN && s.downStart != 0) {
      uint32_t lasted = now > s.downStart ? now - s.downStart : 0;
      if (s.downCount == LOG_LEN) {
        memmove(&s.downs[0], &s.downs[1], (LOG_LEN - 1) * sizeof(Down));
        s.downCount--;
      }
      s.downs[s.downCount++] = {s.downStart, lasted};
      s.downStart = 0;
      dirty = true;
      char dur[16];
      timekeeper::formatDuration(lasted, dur, sizeof(dur));
      snprintf(text, sizeof(text), "Website %s is back after %s.", name.c_str(), dur);
      event = "site_up";
    }
    l.state = State::UP;
  } else {
    if (l.fails == 0) l.firstFailAt = now;
    if (l.fails < 250) l.fails++;
    if (l.state != State::DOWN && l.fails >= FAILS_FOR_DOWN) {
      l.state = State::DOWN;
      s.downStart = l.firstFailAt;
      dirty = true;
      snprintf(text, sizeof(text), "Website %s is down (%s).", name.c_str(), r.reason);
      event = "site_down";
    }
  }

  // Certificate expiry: one warning at two weeks, one more at three days.
  char certText[160] = "";
  if (r.certExpiry != 0) {
    if (s.certExpiry != r.certExpiry) {
      s.certExpiry = r.certExpiry;
      dirty = true;
    }
    if (s.certWarnedFor != r.certExpiry) {
      s.certWarnedFor = r.certExpiry;
      s.certWarnLevel = 0;
    }
    int32_t left = static_cast<int32_t>(r.certExpiry - now);
    int days = left > 0 ? left / 86400 : 0;
    uint8_t level = days <= CERT_URGENT_DAYS ? 2 : days <= CERT_WARN_DAYS ? 1 : 0;
    if (level > s.certWarnLevel) {
      s.certWarnLevel = level;
      dirty = true;
      time_t t = r.certExpiry;
      struct tm tm;
      localtime_r(&t, &tm);
      char date[12];
      strftime(date, sizeof(date), "%Y-%m-%d", &tm);
      if (left <= 0) {
        snprintf(certText, sizeof(certText), "The certificate for %s has expired (%s).",
                 name.c_str(), date);
      } else {
        snprintf(certText, sizeof(certText), "The certificate for %s expires in %d day%s (%s).",
                 name.c_str(), days, days == 1 ? "" : "s", date);
      }
    }
  }
  xSemaphoreGive(mutex);

  if (event) say(event, text);
  if (certText[0]) say("cert_expiring", certText);
  return dirty;
}

bool internetUsable() {
  const monitor::State &m = monitor::state();
  if (!m.haveData || m.outage || m.lastClass != monitor::RoundClass::OK) return false;
  return !(m.degraded && m.degradedCause == Cause::DNS);
}

void task(void *) {
  const settings::Config &conf = settings::get();
  uint32_t resumeAt = RESUME_DELAY_SEC / 2;  // let the link settle after power-on
  uint32_t lastCheck = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    uint32_t mono = timekeeper::mono();
    // Certificates cannot be judged without the date.
    if (!timekeeper::valid() || !net::up()) continue;
    if (!internetUsable()) {
      resumeAt = mono + RESUME_DELAY_SEC;
      continue;
    }
    if (mono < resumeAt || (lastCheck != 0 && mono - lastCheck < GAP_SEC)) continue;

    // The site that has waited longest, if it has waited long enough.
    int pick = -1;
    for (size_t i = 0; i < N; i++) {
      if (conf.site[i].url.length() == 0) continue;
      if (live[i].everChecked && mono - live[i].checkedMono < MIN_INTERVAL_SEC) continue;
      if (pick < 0 || !live[i].everChecked ||
          (live[pick].everChecked && live[i].checkedMono < live[pick].checkedMono)) {
        pick = static_cast<int>(i);
        if (!live[i].everChecked) break;
      }
    }
    if (pick < 0) continue;

    Result r = check(conf.site[pick].url);
    lastCheck = timekeeper::mono();
    live[pick].everChecked = true;
    live[pick].checkedMono = lastCheck;
    // A failure while the internet itself was going away says nothing about
    // the site: drop it.
    if (!r.ok && !internetUsable()) {
      resumeAt = lastCheck + RESUME_DELAY_SEC;
      continue;
    }
    if (simCert && pick == 0 && r.certExpiry != 0) {
      // One fixed date, so that it warns once, as a real certificate would.
      if (simCertExpiry == 0) simCertExpiry = timekeeper::now() + 10UL * 86400 + 3600;
      r.certExpiry = simCertExpiry;
    }
    Serial.printf("[site] %s: %s, HTTP %d, %ld ms%s%s\n", conf.site[pick].name.c_str(),
                  r.ok ? "ok" : "FAILED", r.code, static_cast<long>(r.ms), r.reason[0] ? ", " : "",
                  r.reason);
    if (apply(pick, r, conf.site[pick].name) && !persist()) Serial.println("[site] SAVE FAILED");
  }
}

uint32_t overlap(uint32_t start, uint32_t end, uint32_t from, uint32_t to) {
  uint32_t a = start > from ? start : from;
  uint32_t b = end < to ? end : to;
  return b > a ? b - a : 0;
}

float uptimePct(const Slot &s, uint32_t window, uint32_t now) {
  if (s.since == 0 || now <= s.since) return NAN;
  uint32_t watched = now - s.since;
  uint32_t span = watched < window ? watched : window;
  uint32_t from = now - span;
  uint32_t down = 0;
  for (size_t i = 0; i < s.downCount; i++) {
    down += overlap(s.downs[i].start, s.downs[i].start + s.downs[i].duration, from, now);
  }
  if (s.downStart != 0) down += overlap(s.downStart, now, from, now);
  if (down > span) down = span;
  return 100.0f * (1.0f - static_cast<float>(down) / static_cast<float>(span));
}

}  // namespace

bool any() {
  const settings::Config &conf = settings::get();
  for (size_t i = 0; i < N; i++) {
    if (conf.site[i].url.length()) return true;
  }
  return false;
}

void begin() {
  mutex = xSemaphoreCreateMutex();
  const settings::Config &conf = settings::get();
  if (storage::available()) {
    if (!load(PATH)) load(TMP_PATH);
    LittleFS.remove(TMP_PATH);
  }
  // Figures belong to an address: a slot whose address changed starts again.
  for (size_t i = 0; i < N; i++) {
    uint32_t hash = conf.site[i].url.length() ? hashOf(conf.site[i].url) : 0;
    if (slots[i].urlHash != hash || slots[i].downCount > LOG_LEN) {
      memset(&slots[i], 0, sizeof(Slot));
      slots[i].urlHash = hash;
    }
    if (slots[i].downStart != 0) {
      // It was down when the monitor last stopped: carry on from there.
      live[i].state = State::DOWN;
      live[i].fails = FAILS_FOR_DOWN;
    }
  }
  if (!any()) return;
  // Below the main loop's priority: a TLS handshake is seconds of arithmetic,
  // and the screen and dashboard should not wait for it.
  xTaskCreate(task, "sites", 12288, nullptr, 0, nullptr);
}

void snapshot(size_t index, View &out) {
  memset(&out, 0, sizeof(out));
  out.pct24h = out.pct7d = NAN;
  out.ms = -1;
  const settings::Config &conf = settings::get();
  if (index >= N || conf.site[index].url.length() == 0 || !mutex) return;
  uint32_t now = timekeeper::now();
  xSemaphoreTake(mutex, portMAX_DELAY);
  const Slot &s = slots[index];
  const Live &l = live[index];
  out.used = true;
  out.state = l.state;
  out.fails = l.fails;
  out.code = l.code;
  out.ms = l.ms;
  strlcpy(out.reason, l.reason, sizeof(out.reason));
  out.checkedAt = l.checkedAt;
  out.since = s.since;
  out.downStart = s.downStart;
  out.https = conf.site[index].url.startsWith("https://");
  out.certExpiry = s.certExpiry;
  if (timekeeper::valid()) {
    out.pct24h = uptimePct(s, 24UL * 3600, now);
    out.pct7d = uptimePct(s, 7UL * 24 * 3600, now);
  }
  out.downCount = s.downCount;
  for (size_t i = 0; i < s.downCount; i++) out.downs[i] = s.downs[s.downCount - 1 - i];
  xSemaphoreGive(mutex);
}

void simulateCert() { simCert = true; }

}  // namespace sites
