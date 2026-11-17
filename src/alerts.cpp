#include "alerts.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "net.h"
#include "settings.h"
#include "timekeeper.h"

namespace alerts {
namespace {

constexpr int MAX_ATTEMPTS = 6;
constexpr uint32_t RETRY_DELAY_MS = 20000;
constexpr char TELEGRAM_API[] = "https://api.telegram.org/bot";

struct Message {
  char text[200];
  char event[20];
  uint8_t mask;  // channels still to deliver to, one bit each
  bool isAnnouncement;
  bool isTest;
};

QueueHandle_t queue = nullptr;
TestResult test = {DISCORD, TestState::NONE, 0};

String jsonEscape(const char *text) {
  String out;
  for (const char *p = text; *p; p++) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') out += '\\';
    out += c < 0x20 ? ' ' : static_cast<char>(c);
  }
  return out;
}

// One POST. http:// goes out as it is; https:// is verified against `cas`.
int post(const String &url, const char *cas, const char *contentType, const String &body,
         const char *headerName = nullptr, const String &headerValue = "",
         const char *header2Name = nullptr, const String &header2Value = "") {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  auto send = [&](WiFiClient &client) {
    if (!http.begin(client, url)) return -1;
    http.addHeader("Content-Type", contentType);
    if (headerName) http.addHeader(headerName, headerValue);
    if (header2Name) http.addHeader(header2Name, header2Value);
    int code = http.POST(body);
    http.end();
    return code;
  };
  if (url.startsWith("https://")) {
    net::TlsLock one;
    WiFiClientSecure client;
    client.setCACert(cas);
    return send(client);
  }
  WiFiClient client;
  return send(client);
}

int deliver(Channel channel, const Message &msg) {
  const settings::Config &conf = settings::get();
  switch (channel) {
    case DISCORD:
      return post(conf.hook, DISCORD_ROOT_CAS, "application/json",
                  "{\"content\":\"" + jsonEscape(msg.text) + "\"}");
    case NTFY:
      if (conf.ntfyToken.length()) {
        return post(conf.ntfyUrl, LOOKUP_ROOT_CAS, "text/plain; charset=utf-8", msg.text, "Title",
                    "Uptime Sentinel", "Authorization", "Bearer " + conf.ntfyToken);
      }
      return post(conf.ntfyUrl, LOOKUP_ROOT_CAS, "text/plain; charset=utf-8", msg.text, "Title",
                  "Uptime Sentinel");
    case TELEGRAM:
      return post(String(TELEGRAM_API) + conf.tgToken + "/sendMessage", LOOKUP_ROOT_CAS,
                  "application/json",
                  "{\"chat_id\":\"" + jsonEscape(conf.tgChat.c_str()) + "\",\"text\":\"" +
                      jsonEscape(msg.text) + "\"}");
    case WEBHOOK: {
      String body = "{\"source\":\"uptime-sentinel\",\"version\":\"";
      body += cfg::VERSION;
      body += "\",\"event\":\"" + jsonEscape(msg.event) + "\",\"message\":\"" +
              jsonEscape(msg.text) + "\",\"time\":";
      body += timekeeper::valid() ? timekeeper::now() : 0;
      body += "}";
      return post(conf.webhook, LOOKUP_ROOT_CAS, "application/json", body);
    }
    default:
      return -1;
  }
}

void runTest(const Message &msg) {
  Channel channel = DISCORD;
  for (uint8_t c = 0; c < CHANNEL_COUNT; c++) {
    if (msg.mask & (1 << c)) channel = static_cast<Channel>(c);
  }
  int code = net::up() ? deliver(channel, msg) : -1;
  bool ok = code >= 200 && code < 300;
  Serial.printf("[alert] test to %s: %s (%d)\n", channelName(channel), ok ? "delivered" : "failed",
                code);
  TestResult result = {channel, ok ? TestState::DELIVERED : TestState::FAILED, code};
  test = result;
}

void task(void *) {
  Message msg;
  for (;;) {
    if (xQueueReceive(queue, &msg, portMAX_DELAY) != pdTRUE) continue;
    if (msg.isTest) {
      runTest(msg);
      continue;
    }
    for (int attempt = 1; attempt <= MAX_ATTEMPTS && msg.mask; attempt++) {
      while (!net::up()) vTaskDelay(pdMS_TO_TICKS(1000));
      for (uint8_t c = 0; c < CHANNEL_COUNT; c++) {
        if (!(msg.mask & (1 << c))) continue;
        Channel channel = static_cast<Channel>(c);
        int code = deliver(channel, msg);
        if (code >= 200 && code < 300) {
          msg.mask &= ~(1 << c);
          Serial.printf("[alert] delivered to %s (HTTP %d): %s\n", channelName(channel), code,
                        msg.text);
          if (msg.isAnnouncement) settings::setHookAnnounced();
        } else {
          Serial.printf("[alert] %s attempt %d/%d failed (%d)\n", channelName(channel), attempt,
                        MAX_ATTEMPTS, code);
        }
      }
      if (msg.mask) vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
  }
}

uint8_t allConfigured() {
  uint8_t mask = 0;
  for (uint8_t c = 0; c < CHANNEL_COUNT; c++) {
    if (configured(static_cast<Channel>(c))) mask |= 1 << c;
  }
  return mask;
}

bool enqueue(const char *event, const char *text, uint8_t mask, bool announcement, bool isTest) {
  if (!queue || !mask) return false;
  Message msg;
  strlcpy(msg.text, text, sizeof(msg.text));
  strlcpy(msg.event, event, sizeof(msg.event));
  msg.mask = mask;
  msg.isAnnouncement = announcement;
  msg.isTest = isTest;
  return xQueueSend(queue, &msg, 0) == pdTRUE;
}

}  // namespace

const char *channelName(Channel channel) {
  static const char *const NAMES[CHANNEL_COUNT] = {"Discord", "ntfy", "Telegram", "webhook"};
  return channel < CHANNEL_COUNT ? NAMES[channel] : "?";
}

const char *channelKey(Channel channel) {
  static const char *const KEYS[CHANNEL_COUNT] = {"discord", "ntfy", "telegram", "webhook"};
  return channel < CHANNEL_COUNT ? KEYS[channel] : "";
}

bool parseChannel(const String &key, Channel &out) {
  for (uint8_t c = 0; c < CHANNEL_COUNT; c++) {
    if (key == channelKey(static_cast<Channel>(c))) {
      out = static_cast<Channel>(c);
      return true;
    }
  }
  return false;
}

bool configured(Channel channel) {
  const settings::Config &conf = settings::get();
  switch (channel) {
    case DISCORD: return conf.hook.length() > 0;
    case NTFY: return conf.ntfyUrl.length() > 0;
    case TELEGRAM: return conf.tgToken.length() > 0 && conf.tgChat.length() > 0;
    case WEBHOOK: return conf.webhook.length() > 0;
    default: return false;
  }
}

void begin() {
  if (!allConfigured()) return;
  queue = xQueueCreate(6, sizeof(Message));
  xTaskCreate(task, "alerts", 12288, nullptr, 1, nullptr);
}

void announceOnline() {
  enqueue("online", "Uptime Sentinel online", allConfigured() & (1 << DISCORD), true, false);
}

void outageEnded(uint32_t durationSec, uint32_t endEpoch) {
  char text[96];
  char dur[16];
  timekeeper::formatDuration(durationSec, dur, sizeof(dur));
  if (endEpoch != 0) {
    char clock[8];
    timekeeper::formatClock(endEpoch, clock, sizeof(clock));
    snprintf(text, sizeof(text), "Internet was down %s (ISP side). Back at %s.", dur, clock);
  } else {
    snprintf(text, sizeof(text), "Internet was down %s (ISP side).", dur);
  }
  notify("outage_ended", text);
}

void notify(const char *event, const char *text) {
  enqueue(event, text, allConfigured(), false, false);
}

bool sendTest(Channel channel) {
  if (!configured(channel)) return false;
  TestResult sending = {channel, TestState::SENDING, 0};
  test = sending;
  if (enqueue("test", "Uptime Sentinel test message", 1 << channel, false, true)) return true;
  TestResult failed = {channel, TestState::FAILED, -1};
  test = failed;
  return false;
}

TestResult lastTest() {
  TestResult copy = test;
  return copy;
}

}  // namespace alerts
