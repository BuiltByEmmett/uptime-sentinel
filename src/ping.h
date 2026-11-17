// ICMP echo to the gateway, 1.1.1.1 and 8.8.8.8, once per round, on its own
// task so a slow reply never stalls the display or buttons.
#pragma once
#include <Arduino.h>

namespace ping {

constexpr int TARGETS = 3;
enum Target { ROUTER = 0, CLOUDFLARE = 1, GOOGLE = 2 };

struct Round {
  int16_t rtt[TARGETS];  // milliseconds, -1 = no reply
  uint32_t monoSec;      // seconds since boot when the round started
  bool linkUp;           // WiFi association state when the round started
};

// Forces results, for testing classification without unplugging anything.
enum class Sim : uint8_t { NONE, ISP, WIFI };

void begin();
bool nextRound(Round &out);  // non-blocking; true if a finished round was returned
void setSim(Sim mode);

}  // namespace ping
