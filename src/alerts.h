// Alert messages. Four optional channels, each configured in Settings:
// a Discord webhook, an ntfy topic, a Telegram bot and a generic webhook
// that receives a JSON POST. Every alert goes to every configured channel
// with the same text, from a background task with retries. With nothing
// configured every call is a silent no-op.
//
// Saved URLs and tokens are never printed to serial or sent to a browser.
#pragma once
#include <Arduino.h>

extern const char DISCORD_ROOT_CAS[];  // src/ca_roots.cpp
extern const char LOOKUP_ROOT_CAS[];

namespace alerts {

enum Channel : uint8_t { DISCORD = 0, NTFY = 1, TELEGRAM = 2, WEBHOOK = 3, CHANNEL_COUNT = 4 };

const char *channelName(Channel channel);  // "Discord"
const char *channelKey(Channel channel);   // "discord", used in forms
bool parseChannel(const String &key, Channel &out);
bool configured(Channel channel);

void begin();
void announceOnline();                                      // "Uptime Sentinel online", Discord only
void outageEnded(uint32_t durationSec, uint32_t endEpoch);  // endEpoch 0 = clock unknown

// Any other alert. `event` is a short machine-readable name that the generic
// webhook passes on, for example "site_down".
void notify(const char *event, const char *text);

// "Send test message": one attempt on one channel, no retries.
enum class TestState : uint8_t { NONE, SENDING, DELIVERED, FAILED };
struct TestResult {
  Channel channel;
  TestState state;
  int code;  // HTTP status, or a negative number when there was no answer
};
bool sendTest(Channel channel);
TestResult lastTest();

}  // namespace alerts
