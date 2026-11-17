#include "devices.h"

#include <LittleFS.h>
#include <esp_timer.h>
#include <lwip/etharp.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/sockets.h>

#include "alerts.h"
#include "net.h"
#include "oui_table.h"
#include "settings.h"
#include "storage.h"
#include "timekeeper.h"

namespace devices {
namespace {

constexpr char PATH[] = "/devices.bin";
constexpr char TMP_PATH[] = "/devices.tmp";
constexpr uint32_t MAGIC = 0x56445055;  // "UPDV"
constexpr size_t BATCH = 3;             // ARP requests sent together; the ARP table holds ten
constexpr uint32_t BATCH_WAIT_MS = 300;
constexpr uint8_t MISSES_FOR_OFFLINE = 2;
constexpr uint32_t SAVE_SEEN_EVERY_SEC = 1800;  // "last seen" alone is saved this often
constexpr uint32_t NAME_RETRY_SEC = 6UL * 3600;
constexpr int NAME_LOOKUPS_PER_SCAN = 4;
constexpr int ALERTS_PER_SCAN = 3;
constexpr uint16_t ECHO_ID = 0x5544;  // "UD"; the ping task uses other ids
constexpr uint8_t FLAG_TRUSTED = 0x01;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint8_t primed;  // a first full scan has been stored: later arrivals are new
  uint8_t reserved[3];
};

struct Record {  // kept on flash
  uint8_t mac[6];
  uint8_t flags;
  uint8_t reserved;
  uint32_t ip;
  uint32_t firstSeen;
  uint32_t lastSeen;
  char nick[NICK_MAX_LEN + 1];
  char host[HOST_MAX_LEN + 1];
  uint8_t pad[3];
};

struct Live {  // kept while running
  bool online = false;
  bool seenThisScan = false;
  bool isNew = false;
  uint8_t missed = 0;
  int16_t pingMs = -1;
  uint32_t nameTriedMono = 0;
  bool nameTried = false;
};

Record records[MAX_DEVICES];
Live live[MAX_DEVICES];
size_t recordCount = 0;
bool primed = false;
bool dirty = false;
uint32_t savedMono = 0;
SemaphoreHandle_t mutex = nullptr;
Status state = {};
volatile bool scanNow = false;
volatile bool saveSoon = false;

// ---- flash -----------------------------------------------------------------

bool load(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  Header h;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) && h.magic == MAGIC &&
            h.version == 1 && h.count <= MAX_DEVICES;
  if (ok) {
    size_t bytes = h.count * sizeof(Record);
    ok = f.read(reinterpret_cast<uint8_t *>(records), bytes) == bytes;
    if (ok) {
      recordCount = h.count;
      primed = h.primed != 0;
      for (size_t i = 0; i < recordCount; i++) {
        records[i].nick[NICK_MAX_LEN] = 0;
        records[i].host[HOST_MAX_LEN] = 0;
      }
    }
  }
  f.close();
  return ok;
}

bool persist() {
  if (!storage::available()) return false;
  File f = LittleFS.open(TMP_PATH, "w");
  if (!f) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  Header h = {MAGIC, 1, static_cast<uint16_t>(recordCount), static_cast<uint8_t>(primed), {0, 0, 0}};
  size_t bytes = recordCount * sizeof(Record);
  bool ok = f.write(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            f.write(reinterpret_cast<uint8_t *>(records), bytes) == bytes;
  dirty = false;
  xSemaphoreGive(mutex);
  f.close();
  savedMono = timekeeper::mono();
  if (!ok) return false;
  if (!LittleFS.rename(TMP_PATH, PATH)) {
    LittleFS.remove(PATH);
    ok = LittleFS.rename(TMP_PATH, PATH);
  }
  return ok;
}

// ---- ARP, run inside the network stack's own thread ------------------------

struct ArpCall {
  struct tcpip_api_call_data base;  // must be first
  enum { CLEAR, REQUEST, LOOKUP } what;
  uint32_t self;  // the board's address, to pick the interface
  size_t n;
  uint32_t ip[BATCH];
  uint8_t mac[BATCH][6];
  bool found[BATCH];
};

err_t arpInStack(struct tcpip_api_call_data *data) {
  ArpCall *call = reinterpret_cast<ArpCall *>(data);
  struct netif *nif = nullptr;
  for (struct netif *it = netif_list; it != nullptr; it = it->next) {
    if (netif_is_up(it) && ip4_addr_get_u32(netif_ip4_addr(it)) == call->self) nif = it;
  }
  if (!nif) return ERR_IF;
  if (call->what == ArpCall::CLEAR) {
    // Forget old answers, so that an entry found later is a fresh one.
    etharp_cleanup_netif(nif);
    return ERR_OK;
  }
  for (size_t i = 0; i < call->n; i++) {
    ip4_addr_t addr;
    addr.addr = call->ip[i];
    if (call->what == ArpCall::REQUEST) {
      etharp_request(nif, &addr);
    } else {
      struct eth_addr *eth = nullptr;
      const ip4_addr_t *found = nullptr;
      call->found[i] = etharp_find_addr(nif, &addr, &eth, &found) >= 0 && eth != nullptr;
      if (call->found[i]) memcpy(call->mac[i], eth->addr, 6);
    }
  }
  return ERR_OK;
}

bool arp(ArpCall &call) { return tcpip_api_call(arpInStack, &call.base) == ERR_OK; }

// ---- ping and host names ---------------------------------------------------

uint16_t checksum(const uint8_t *data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < len; i += 2) sum += (data[i] << 8) | data[i + 1];
  if (len & 1) sum += data[len - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return htons(static_cast<uint16_t>(~sum));
}

// Waits until `sock` has something to read. False when the time is up.
bool readable(int sock, int64_t deadlineUs) {
  int64_t left = deadlineUs - esp_timer_get_time();
  if (left <= 0) return false;
  fd_set set;
  FD_ZERO(&set);
  FD_SET(sock, &set);
  timeval tv;
  tv.tv_sec = left / 1000000;
  tv.tv_usec = left % 1000000;
  return lwip_select(sock + 1, &set, nullptr, nullptr, &tv) > 0;
}

sockaddr_in addressOf(uint32_t ip, uint16_t port) {
  sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = ip;
  return to;
}

int16_t pingOnce(uint32_t ip, uint32_t timeoutMs) {
  static uint16_t seq = 0;
  int sock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sock < 0) return -1;
  uint8_t pkt[32];
  memset(pkt, 0, sizeof(pkt));
  uint16_t mySeq = ++seq;
  pkt[0] = 8;  // echo request
  pkt[4] = ECHO_ID >> 8;
  pkt[5] = ECHO_ID & 0xFF;
  pkt[6] = mySeq >> 8;
  pkt[7] = mySeq & 0xFF;
  for (size_t i = 8; i < sizeof(pkt); i++) pkt[i] = 'a' + (i % 26);
  uint16_t sum = checksum(pkt, sizeof(pkt));
  memcpy(pkt + 2, &sum, 2);

  sockaddr_in to = addressOf(ip, 0);
  int64_t sentAt = esp_timer_get_time();
  int16_t result = -1;
  if (lwip_sendto(sock, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) ==
      static_cast<int>(sizeof(pkt))) {
    int64_t deadline = sentAt + timeoutMs * 1000LL;
    uint8_t buf[96];
    while (readable(sock, deadline)) {
      sockaddr_in from;
      socklen_t fromLen = sizeof(from);
      int n = lwip_recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from),
                            &fromLen);
      if (n < 20) continue;
      int ipHeader = (buf[0] & 0x0F) * 4;
      if (n < ipHeader + 8) continue;
      const uint8_t *icmp = buf + ipHeader;
      if (icmp[0] != 0 || from.sin_addr.s_addr != ip) continue;
      if (((icmp[4] << 8) | icmp[5]) != ECHO_ID || ((icmp[6] << 8) | icmp[7]) != mySeq) continue;
      int64_t ms = (esp_timer_get_time() - sentAt + 500) / 1000;
      result = static_cast<int16_t>(ms < 1 ? 1 : ms);
      break;
    }
  }
  lwip_close(sock);
  return result;
}

// Reads a DNS name at `pos`, following compression pointers. Returns false
// on anything malformed.
bool readName(const uint8_t *msg, int len, int pos, char *out, size_t outLen) {
  size_t o = 0;
  for (int jumps = 0; jumps < 12;) {
    if (pos >= len) return false;
    uint8_t n = msg[pos];
    if (n == 0) {
      out[o] = 0;
      return o > 0;
    }
    if ((n & 0xC0) == 0xC0) {
      if (pos + 1 >= len) return false;
      pos = ((n & 0x3F) << 8) | msg[pos + 1];
      jumps++;
      continue;
    }
    if (n > 63 || pos + 1 + n > len) return false;
    if (o && o < outLen - 1) out[o++] = '.';
    for (int i = 0; i < n; i++) {
      char c = static_cast<char>(msg[pos + 1 + i]);
      // Only what a host name can hold; anything else becomes a dash.
      bool plain = isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
      if (o < outLen - 1) out[o++] = plain ? c : '-';
    }
    pos += 1 + n;
  }
  return false;
}

// Reverse lookup: asks `server` what name `ip` has. With `mdns` the question
// goes to the device itself on the mDNS port, which Apple, Linux and many
// smart-home devices answer. Empty `out` when nothing useful came back.
void reverseLookup(uint32_t server, bool mdns, uint32_t ip, char *out, size_t outLen) {
  static uint16_t nextId = 0x6000;
  out[0] = 0;
  uint8_t pkt[64];
  size_t len = 0;
  uint16_t id = ++nextId;
  pkt[len++] = id >> 8;
  pkt[len++] = id & 0xFF;
  pkt[len++] = mdns ? 0x00 : 0x01;  // recursion desired from a real server
  pkt[len++] = 0x00;
  pkt[len++] = 0x00;
  pkt[len++] = 0x01;
  for (int i = 0; i < 6; i++) pkt[len++] = 0x00;
  const uint8_t *b = reinterpret_cast<const uint8_t *>(&ip);  // a.b.c.d in memory order
  for (int i = 3; i >= 0; i--) {
    char label[4];
    int n = snprintf(label, sizeof(label), "%u", b[i]);
    pkt[len++] = n;
    memcpy(pkt + len, label, n);
    len += n;
  }
  static const char TAIL[] = "\x07in-addr\x04" "arpa";
  memcpy(pkt + len, TAIL, sizeof(TAIL));  // includes the closing zero
  len += sizeof(TAIL);
  pkt[len++] = 0x00;
  pkt[len++] = 0x0C;                 // type PTR
  pkt[len++] = mdns ? 0x80 : 0x00;   // mDNS: please answer straight to us
  pkt[len++] = 0x01;                 // class IN

  int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return;
  sockaddr_in to = addressOf(server, mdns ? 5353 : 53);
  if (lwip_sendto(sock, pkt, len, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) ==
      static_cast<int>(len)) {
    int64_t deadline = esp_timer_get_time() + (mdns ? 400 : 600) * 1000LL;
    uint8_t buf[300];
    while (readable(sock, deadline)) {
      sockaddr_in from;
      socklen_t fromLen = sizeof(from);
      int n = lwip_recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from),
                            &fromLen);
      if (n < 12 || from.sin_addr.s_addr != server || !(buf[2] & 0x80)) continue;
      if (!mdns && (buf[0] != (id >> 8) || buf[1] != (id & 0xFF))) continue;
      int questions = (buf[4] << 8) | buf[5];
      int answers = (buf[6] << 8) | buf[7];
      int pos = 12;
      auto skipName = [&]() {
        while (pos < n) {
          uint8_t c = buf[pos];
          if (c == 0) {
            pos++;
            return;
          }
          if ((c & 0xC0) == 0xC0) {
            pos += 2;
            return;
          }
          pos += 1 + c;
        }
      };
      for (int q = 0; q < questions && pos < n; q++) {
        skipName();
        pos += 4;
      }
      for (int a = 0; a < answers && pos + 10 <= n; a++) {
        skipName();
        if (pos + 10 > n) break;
        int type = (buf[pos] << 8) | buf[pos + 1];
        int rdLen = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;
        if (type == 12 && pos + rdLen <= n) {
          char name[80];
          if (readName(buf, n, pos, name, sizeof(name))) {
            // "kitchen-speaker.local" and "laptop.lan" read better without the tail.
            static const char *const TAILS[] = {".local", ".lan", ".home", ".localdomain",
                                                ".home.arpa", ".internal"};
            for (const char *tail : TAILS) {
              size_t nameLen = strlen(name), tailLen = strlen(tail);
              if (nameLen > tailLen && strcasecmp(name + nameLen - tailLen, tail) == 0) {
                name[nameLen - tailLen] = 0;
              }
            }
            strlcpy(out, name, outLen);
          }
          break;
        }
        pos += rdLen;
      }
      break;
    }
  }
  lwip_close(sock);
}

// ---- the table -------------------------------------------------------------

int findMac(const uint8_t mac[6]) {
  for (size_t i = 0; i < recordCount; i++) {
    if (memcmp(records[i].mac, mac, 6) == 0) return static_cast<int>(i);
  }
  return -1;
}

void removeAt(size_t index) {
  memmove(&records[index], &records[index + 1], (recordCount - index - 1) * sizeof(Record));
  memmove(&live[index], &live[index + 1], (recordCount - index - 1) * sizeof(Live));
  recordCount--;
}

// Makes room in a full table: the device not seen for longest, among those
// the user has not named or marked. False if every one is named or marked.
bool evictOne() {
  int pick = -1;
  for (size_t i = 0; i < recordCount; i++) {
    if (live[i].online || live[i].seenThisScan || records[i].nick[0] ||
        (records[i].flags & FLAG_TRUSTED)) {
      continue;
    }
    if (pick < 0 || records[i].lastSeen < records[pick].lastSeen) pick = static_cast<int>(i);
  }
  if (pick < 0) return false;
  removeAt(pick);
  return true;
}

// A device answered. Call with the mutex held.
void seen(uint32_t ip, const uint8_t mac[6], uint32_t now) {
  int i = findMac(mac);
  if (i < 0) {
    if (recordCount == MAX_DEVICES && !evictOne()) return;
    i = static_cast<int>(recordCount++);
    memset(&records[i], 0, sizeof(Record));
    memcpy(records[i].mac, mac, 6);
    records[i].firstSeen = now;
    live[i] = Live();
    live[i].isNew = primed;
    dirty = true;
  }
  Record &r = records[i];
  if (r.ip != ip) {
    r.ip = ip;
    dirty = true;
  }
  if (now != 0) {
    if (r.firstSeen == 0) r.firstSeen = now;
    r.lastSeen = now;
  }
  live[i].seenThisScan = true;
  live[i].online = true;
  live[i].missed = 0;
}

void scan() {
  uint32_t self = static_cast<uint32_t>(net::localIP());
  uint32_t mask = ntohl(static_cast<uint32_t>(net::subnetMask()));
  if (self == 0) return;
  if (mask == 0) mask = 0xFFFFFF00;
  uint32_t scanMask = mask | 0xFFFFFF00;  // never more than a /24
  uint32_t host = ntohl(self);
  uint32_t first = (host & scanMask) + 1;
  uint32_t last = (host | ~scanMask) - 1;
  uint32_t gateway = static_cast<uint32_t>(net::gatewayIP());
  uint32_t now = timekeeper::valid() ? timekeeper::now() : 0;

  xSemaphoreTake(mutex, portMAX_DELAY);
  state.scanning = true;
  state.capped = scanMask != mask;
  for (size_t i = 0; i < recordCount; i++) live[i].seenThisScan = false;
  xSemaphoreGive(mutex);

  ArpCall call;
  memset(&call, 0, sizeof(call));
  call.self = self;
  call.what = ArpCall::CLEAR;
  if (!arp(call)) {
    state.scanning = false;
    return;
  }

  size_t others = 0;
  bool complete = true;
  for (uint32_t addr = first; addr <= last;) {
    call.n = 0;
    while (call.n < BATCH && addr <= last) {
      if (addr != host) call.ip[call.n++] = htonl(addr);
      addr++;
    }
    if (call.n == 0) break;
    call.what = ArpCall::REQUEST;
    if (!arp(call) || static_cast<uint32_t>(net::localIP()) != self) {
      complete = false;  // the link went away or changed under us
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(BATCH_WAIT_MS));
    call.what = ArpCall::LOOKUP;
    if (!arp(call)) {
      complete = false;
      break;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (size_t i = 0; i < call.n; i++) {
      if (!call.found[i]) continue;
      seen(call.ip[i], call.mac[i], now);
      if (call.ip[i] != gateway) others++;
    }
    xSemaphoreGive(mutex);
  }

  // Pings and host names, outside the lock: they wait on the network.
  int lookups = 0, alertsSent = 0, newCount = 0;
  uint32_t dns = static_cast<uint32_t>(net::dnsIP());
  for (size_t i = 0; i < recordCount; i++) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool here = i < recordCount && live[i].seenThisScan;
    uint32_t ip = here ? records[i].ip : 0;
    uint8_t mac[6];
    if (here) memcpy(mac, records[i].mac, 6);
    bool wantName = here && records[i].host[0] == 0 &&
                    (live[i].isNew || (lookups < NAME_LOOKUPS_PER_SCAN &&
                                       (!live[i].nameTried || timekeeper::mono() - live[i].nameTriedMono >=
                                                                  NAME_RETRY_SEC)));
    bool isNew = here && live[i].isNew;
    xSemaphoreGive(mutex);
    if (!here) continue;

    int16_t ms = pingOnce(ip, 300);
    if (ms < 0) ms = pingOnce(ip, 300);
    char name[HOST_MAX_LEN + 1] = "";
    if (wantName) {
      lookups++;
      if (dns != 0) reverseLookup(dns, false, ip, name, sizeof(name));
      if (name[0] == 0) reverseLookup(ip, true, ip, name, sizeof(name));
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    int at = findMac(mac);  // the table may have shifted meanwhile
    if (at >= 0) {
      live[at].pingMs = ms;
      if (wantName) {
        live[at].nameTried = true;
        live[at].nameTriedMono = timekeeper::mono();
        if (name[0]) {
          strlcpy(records[at].host, name, sizeof(records[at].host));
          dirty = true;
        }
      }
      live[at].isNew = false;
    }
    xSemaphoreGive(mutex);

    if (isNew) {
      newCount++;
      Serial.printf("[devices] new device: %s%s%s\n", vendorOf(mac), name[0] ? ", " : "", name);
      if (settings::get().deviceAlert && alertsSent < ALERTS_PER_SCAN) {
        alertsSent++;
        char text[160];
        snprintf(text, sizeof(text),
                 "New device on the network: %s%s%s. See the Devices page on the dashboard.",
                 vendorOf(mac), name[0] ? ", " : "", name);
        alerts::notify("new_device", text);
      }
    }
  }
  if (settings::get().deviceAlert && newCount > ALERTS_PER_SCAN) {
    char text[96];
    snprintf(text, sizeof(text), "%d more new devices joined the network. See the Devices page.",
             newCount - ALERTS_PER_SCAN);
    alerts::notify("new_device", text);
  }

  xSemaphoreTake(mutex, portMAX_DELAY);
  uint16_t online = 0;
  for (size_t i = 0; i < recordCount; i++) {
    if (!live[i].seenThisScan && live[i].online && complete &&
        ++live[i].missed >= MISSES_FOR_OFFLINE) {
      live[i].online = false;
      live[i].pingMs = -1;
    }
    if (live[i].online) online++;
  }
  if (complete && !primed) {
    primed = true;
    dirty = true;
  }
  state.scanning = false;
  if (complete) {
    state.scanned = true;
    state.lastScan = now;
    state.onlyGateway = others == 0;
  }
  state.known = recordCount;
  state.online = online;
  bool save = dirty || timekeeper::mono() - savedMono >= SAVE_SEEN_EVERY_SEC;
  xSemaphoreGive(mutex);

  Serial.printf("[devices] scan %s: %u online of %u known\n", complete ? "done" : "interrupted",
                online, static_cast<unsigned>(recordCount));
  if (save && !persist()) Serial.println("[devices] SAVE FAILED");
}

void task(void *) {
  uint32_t nextScan = FIRST_SCAN_SEC;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t mono = timekeeper::mono();
    if (saveSoon) {
      saveSoon = false;
      if (!persist()) Serial.println("[devices] SAVE FAILED");
    }
    if (!scanNow && mono < nextScan) continue;
    if (!net::up()) continue;
    scanNow = false;
    scan();
    nextScan = timekeeper::mono() + SCAN_INTERVAL_SEC;
  }
}

}  // namespace

const char *vendorOf(const uint8_t mac[6]) {
  // Phones and laptops often make up a private address for each network;
  // those belong to no maker.
  if (mac[0] & 0x02) return "Unknown (private address)";
  uint32_t prefix = (mac[0] << 16) | (mac[1] << 8) | mac[2];
  size_t lo = 0, hi = OUI_COUNT;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const uint8_t *e = OUI_TABLE + mid * 4;
    uint32_t value = (e[0] << 16) | (e[1] << 8) | e[2];
    if (value == prefix) return OUI_VENDORS[e[3]];
    if (value < prefix) lo = mid + 1;
    else hi = mid;
  }
  return "Unknown";
}

void begin() {
  mutex = xSemaphoreCreateMutex();
  state.enabled = settings::get().deviceScan;
  if (!state.enabled) return;
  if (storage::available()) {
    if (!load(PATH)) load(TMP_PATH);
    LittleFS.remove(TMP_PATH);
  }
  state.known = recordCount;
  Serial.printf("[devices] %u known devices loaded\n", static_cast<unsigned>(recordCount));
  // Below the main loop's priority, like the website checks.
  xTaskCreate(task, "devices", 6144, nullptr, 0, nullptr);
}

Status status() {
  if (!mutex) return state;
  xSemaphoreTake(mutex, portMAX_DELAY);
  Status copy = state;
  xSemaphoreGive(mutex);
  return copy;
}

size_t count() { return recordCount; }

bool get(size_t index, View &out) {
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = index < recordCount;
  if (ok) {
    const Record &r = records[index];
    memcpy(out.mac, r.mac, 6);
    out.ip = r.ip;
    out.vendor = vendorOf(r.mac);
    strlcpy(out.host, r.host, sizeof(out.host));
    strlcpy(out.nick, r.nick, sizeof(out.nick));
    out.trusted = r.flags & FLAG_TRUSTED;
    out.online = live[index].online;
    out.gateway = r.ip == static_cast<uint32_t>(net::gatewayIP());
    out.pingMs = live[index].pingMs;
    out.firstSeen = r.firstSeen;
    out.lastSeen = r.lastSeen;
  }
  xSemaphoreGive(mutex);
  return ok;
}

bool validNick(const String &text) {
  if (text.length() > NICK_MAX_LEN) return false;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c < ' ' || c > '~' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' ||
        c == '\\') {
      return false;
    }
  }
  return true;
}

bool parseMac(const String &hex, uint8_t mac[6]) {
  if (hex.length() != 12) return false;
  for (int i = 0; i < 12; i++) {
    if (!isxdigit(static_cast<unsigned char>(hex[i]))) return false;
  }
  for (int i = 0; i < 6; i++) mac[i] = strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  return true;
}

bool update(const uint8_t mac[6], const String &nick, bool trusted) {
  if (!mutex || !validNick(nick)) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  int i = findMac(mac);
  if (i >= 0) {
    strlcpy(records[i].nick, nick.c_str(), sizeof(records[i].nick));
    if (trusted) records[i].flags |= FLAG_TRUSTED;
    else records[i].flags &= ~FLAG_TRUSTED;
  }
  xSemaphoreGive(mutex);
  // The scan task is the only one that writes the file.
  if (i >= 0) saveSoon = true;
  return i >= 0;
}

void simulateNew() {
  if (!mutex) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  uint32_t gateway = static_cast<uint32_t>(net::gatewayIP());
  for (size_t i = recordCount; i-- > 0;) {
    if (live[i].online && records[i].ip != gateway) {
      removeAt(i);
      break;
    }
  }
  xSemaphoreGive(mutex);
  scanNow = true;
}

}  // namespace devices
