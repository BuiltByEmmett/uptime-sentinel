#include "alerts.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "settings.h"
#include "timekeeper.h"

namespace alerts {
namespace {

constexpr int MAX_ATTEMPTS = 6;
constexpr uint32_t RETRY_DELAY_MS = 20000;

struct Message {
  char text[160];
  bool isAnnouncement;
};

String url;
QueueHandle_t queue = nullptr;

String jsonBody(const char *text) {
  String body = "{\"content\":\"";
  for (const char *p = text; *p; p++) {
    if (*p == '"' || *p == '\\') body += '\\';
    body += *p;
  }
  body += "\"}";
  return body;
}

int post(const char *text) {
  WiFiClientSecure client;
  client.setCACert(DISCORD_ROOT_CAS);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonBody(text));
  http.end();
  return code;
}

void task(void *) {
  Message msg;
  for (;;) {
    if (xQueueReceive(queue, &msg, portMAX_DELAY) != pdTRUE) continue;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
      while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
      int code = post(msg.text);
      if (code >= 200 && code < 300) {
        Serial.printf("[alert] delivered (HTTP %d): %s\n", code, msg.text);
        if (msg.isAnnouncement) settings::setHookAnnounced();
        break;
      }
      Serial.printf("[alert] attempt %d/%d failed (%d)\n", attempt, MAX_ATTEMPTS, code);
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
  }
}

void enqueue(const Message &msg) {
  if (queue) xQueueSend(queue, &msg, 0);
}

}  // namespace

void begin(const String &webhookUrl) {
  if (webhookUrl.length() == 0) return;
  url = webhookUrl;
  queue = xQueueCreate(4, sizeof(Message));
  xTaskCreate(task, "alerts", 12288, nullptr, 1, nullptr);
}

void announceOnline() {
  Message msg;
  strlcpy(msg.text, "Uptime monitor online", sizeof(msg.text));
  msg.isAnnouncement = true;
  enqueue(msg);
}

void outageEnded(uint32_t durationSec, uint32_t endEpoch) {
  Message msg;
  msg.isAnnouncement = false;
  char dur[16];
  timekeeper::formatDuration(durationSec, dur, sizeof(dur));
  if (endEpoch != 0) {
    char clock[8];
    timekeeper::formatClock(endEpoch, clock, sizeof(clock));
    snprintf(msg.text, sizeof(msg.text), "Internet was down %s (ISP side). Back at %s.", dur,
             clock);
  } else {
    snprintf(msg.text, sizeof(msg.text), "Internet was down %s (ISP side).", dur);
  }
  enqueue(msg);
}

}  // namespace alerts
