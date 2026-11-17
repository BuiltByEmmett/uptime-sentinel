// Discord webhook messages. Plain HTTPS POST from a background task with
// retries; every call is a silent no-op when no webhook URL is saved.
#pragma once
#include <Arduino.h>

extern const char DISCORD_ROOT_CAS[];  // src/ca_roots.cpp

namespace alerts {

void begin(const String &webhookUrl);
void announceOnline();                                   // "Uptime monitor online"
void outageEnded(uint32_t durationSec, uint32_t endEpoch);  // endEpoch 0 = clock unknown

}  // namespace alerts
