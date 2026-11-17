#include "ping.h"

#include <WiFi.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "config.h"

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

void task(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    Round round;
    round.monoSec = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    round.linkUp = WiFi.status() == WL_CONNECTED;

    uint32_t addrs[TARGETS] = {0, 0, 0};
    if (round.linkUp) {
      addrs[ROUTER] = static_cast<uint32_t>(WiFi.gatewayIP());
      addrs[CLOUDFLARE] = static_cast<uint32_t>(IPAddress(1, 1, 1, 1));
      addrs[GOOGLE] = static_cast<uint32_t>(IPAddress(8, 8, 8, 8));
    }
    pingAll(addrs, round.rtt);

    Sim sim = simMode;
    if (sim == Sim::ISP || sim == Sim::WIFI) {
      round.rtt[CLOUDFLARE] = -1;
      round.rtt[GOOGLE] = -1;
    }
    if (sim == Sim::WIFI) round.rtt[ROUTER] = -1;

    xQueueSend(results, &round, 0);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(cfg::PING_INTERVAL_MS));
  }
}

}  // namespace

void begin() {
  results = xQueueCreate(8, sizeof(Round));
  xTaskCreate(task, "ping", 4096, nullptr, 2, nullptr);
}

bool nextRound(Round &out) { return results && xQueueReceive(results, &out, 0) == pdTRUE; }

void setSim(Sim mode) { simMode = mode; }

}  // namespace ping
