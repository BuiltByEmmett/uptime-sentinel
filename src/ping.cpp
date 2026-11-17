#include "ping.h"

#include <IPAddress.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "config.h"
#include "net.h"
#include "settings.h"

namespace ping {
namespace {

constexpr uint8_t ICMP_ECHO_REQUEST = 8;
constexpr uint8_t ICMP_ECHO_REPLY = 0;
constexpr uint16_t ECHO_ID = 0x554D;  // "UM"

struct __attribute__((packed)) EchoPacket {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t id;
  uint16_t seq;
  uint8_t payload[24];
};

QueueHandle_t results = nullptr;
volatile Sim simMode = Sim::NONE;
uint16_t seqBase = 0;

// RFC 1071 internet checksum.
uint16_t checksum(const uint8_t *data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < len; i += 2) sum += (data[i] << 8) | data[i + 1];
  if (len & 1) sum += data[len - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return htons(static_cast<uint16_t>(~sum));
}

int16_t elapsedMs(int64_t sinceUs) {
  int64_t ms = (esp_timer_get_time() - sinceUs + 500) / 1000;
  if (ms < 1) ms = 1;
  if (ms > 30000) ms = 30000;
  return static_cast<int16_t>(ms);
}

// Sends one echo request to every address (0 = skip) and collects replies
// until all have answered or the timeout passes.
void pingAll(const uint32_t addrs[TARGETS], int16_t rtt[TARGETS]) {
  for (int i = 0; i < TARGETS; i++) rtt[i] = -1;

  int sock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sock < 0) return;

  seqBase += TARGETS;
  int64_t sentAt[TARGETS] = {0};
  bool waiting[TARGETS] = {false};
  int pending = 0;

  for (int i = 0; i < TARGETS; i++) {
    if (addrs[i] == 0) continue;
    EchoPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = ICMP_ECHO_REQUEST;
    pkt.id = htons(ECHO_ID);
    pkt.seq = htons(static_cast<uint16_t>(seqBase + i));
    for (size_t b = 0; b < sizeof(pkt.payload); b++) pkt.payload[b] = 'a' + (b % 26);
    pkt.checksum = checksum(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));

    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = addrs[i];

    sentAt[i] = esp_timer_get_time();
    if (lwip_sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr *>(&to),
                    sizeof(to)) == static_cast<int>(sizeof(pkt))) {
      waiting[i] = true;
      pending++;
    }
  }

  const int64_t deadline = esp_timer_get_time() + cfg::PING_TIMEOUT_MS * 1000LL;
  uint8_t buf[128];
  while (pending > 0) {
    int64_t left = deadline - esp_timer_get_time();
    if (left <= 0) break;

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sock, &readable);
    timeval tv;
    tv.tv_sec = left / 1000000;
    tv.tv_usec = left % 1000000;
    if (lwip_select(sock + 1, &readable, nullptr, nullptr, &tv) <= 0) break;

    sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    int n = lwip_recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from),
                          &fromLen);
    if (n < 20) continue;

    // Raw sockets hand back the IP header too; skip it.
    int ipHeader = (buf[0] & 0x0F) * 4;
    if (n < ipHeader + 8) continue;
    const uint8_t *icmp = buf + ipHeader;
    if (icmp[0] != ICMP_ECHO_REPLY) continue;
    uint16_t id = (icmp[4] << 8) | icmp[5];
    uint16_t seq = (icmp[6] << 8) | icmp[7];
    if (id != ECHO_ID) continue;

    int idx = static_cast<uint16_t>(seq - seqBase);
    if (idx < 0 || idx >= TARGETS || !waiting[idx]) continue;
    if (from.sin_addr.s_addr != addrs[idx]) continue;

    rtt[idx] = elapsedMs(sentAt[idx]);
    waiting[idx] = false;
    pending--;
  }
  lwip_close(sock);
}

// Asks the network's own DNS server (normally the router, which forwards to
// the ISP) for an A record, bypassing the ESP32's resolver cache. True if a
// usable answer came back in time.
bool dnsLookup(uint32_t server, const char *host) {
  static uint16_t nextId = 0x4000;
  uint8_t pkt[96];
  size_t len = 0;
  uint16_t id = ++nextId;

  pkt[len++] = id >> 8;
  pkt[len++] = id & 0xFF;
  pkt[len++] = 0x01;  // recursion desired
  pkt[len++] = 0x00;
  pkt[len++] = 0x00;
  pkt[len++] = 0x01;  // one question
  for (int i = 0; i < 6; i++) pkt[len++] = 0x00;
  // "www.example.com" -> 3www7example3com0
  const char *label = host;
  while (*label) {
    const char *dot = strchr(label, '.');
    size_t n = dot ? static_cast<size_t>(dot - label) : strlen(label);
    if (n == 0 || n > 63 || len + n + 6 > sizeof(pkt)) return false;
    pkt[len++] = n;
    memcpy(pkt + len, label, n);
    len += n;
    label += n + (dot ? 1 : 0);
  }
  pkt[len++] = 0x00;
  pkt[len++] = 0x00;
  pkt[len++] = 0x01;  // type A
  pkt[len++] = 0x00;
  pkt[len++] = 0x01;  // class IN

  int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return false;
  sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  to.sin_addr.s_addr = server;

  bool answered = false;
  if (lwip_sendto(sock, pkt, len, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) ==
      static_cast<int>(len)) {
    const int64_t deadline = esp_timer_get_time() + cfg::DNS_TIMEOUT_MS * 1000LL;
    uint8_t buf[256];
    for (;;) {
      int64_t left = deadline - esp_timer_get_time();
      if (left <= 0) break;
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(sock, &readable);
      timeval tv;
      tv.tv_sec = left / 1000000;
      tv.tv_usec = left % 1000000;
      if (lwip_select(sock + 1, &readable, nullptr, nullptr, &tv) <= 0) break;
      int n = lwip_recv(sock, buf, sizeof(buf), 0);
      if (n < 12 || buf[0] != (id >> 8) || buf[1] != (id & 0xFF)) continue;
      bool isResponse = buf[2] & 0x80;
      uint8_t rcode = buf[3] & 0x0F;
      uint16_t answers = (buf[6] << 8) | buf[7];
      answered = isResponse && rcode == 0 && answers > 0;
      break;
    }
  }
  lwip_close(sock);
  return answered;
}

void task(void *) {
  // Well-known names with short lifetimes, so a cached answer in the router
  // cannot hide a dead upstream resolver for long.
  static const char *const DNS_NAMES[] = {"www.google.com", "www.cloudflare.com",
                                          "www.amazon.com", "www.apple.com"};
  uint32_t roundNo = 0;
  size_t dnsName = 0;
  bool dnsFailing = false;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    Round round;
    round.monoSec = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    round.linkUp = net::up();
    round.wired = net::wired();
    round.dns = -1;

    uint32_t addrs[TARGETS] = {0, 0, 0};
    if (round.linkUp) {
      addrs[ROUTER] = static_cast<uint32_t>(net::gatewayIP());
      addrs[TARGET1] = settings::targetAddr(0);
      addrs[TARGET2] = settings::targetAddr(1);
    }
    pingAll(addrs, round.rtt);

    Sim sim = simMode;
    if (sim == Sim::ISP || sim == Sim::WIFI) {
      round.rtt[TARGET1] = -1;
      round.rtt[TARGET2] = -1;
    }
    if (sim == Sim::WIFI) round.rtt[ROUTER] = -1;
    if (sim == Sim::SLOW) {
      for (int t = TARGET1; t <= TARGET2; t++) {
        if (round.rtt[t] >= 0) round.rtt[t] += 300;
      }
    }

    // DNS only means something when the internet itself is reachable.
    bool internet = round.rtt[TARGET1] >= 0 || round.rtt[TARGET2] >= 0;
    bool due = dnsFailing || roundNo % cfg::DNS_EVERY_ROUNDS == 0 || sim == Sim::DNS;
    uint32_t server = static_cast<uint32_t>(net::dnsIP());
    if (internet && due && server != 0) {
      bool ok = sim == Sim::DNS ? false : dnsLookup(server, DNS_NAMES[dnsName]);
      dnsName = (dnsName + 1) % (sizeof(DNS_NAMES) / sizeof(DNS_NAMES[0]));
      dnsFailing = !ok;
      round.dns = ok ? 1 : 0;
    }
    roundNo++;

    xQueueSend(results, &round, 0);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(cfg::PING_INTERVAL_MS));
  }
}

}  // namespace

void begin() {
  results = xQueueCreate(8, sizeof(Round));
  xTaskCreate(task, "ping", 6144, nullptr, 2, nullptr);
}

bool nextRound(Round &out) { return results && xQueueReceive(results, &out, 0) == pdTRUE; }

void setSim(Sim mode) { simMode = mode; }

}  // namespace ping
