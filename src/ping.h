// One round every two seconds, on its own task so a slow reply never stalls
// the display or buttons: ICMP echo to the gateway and the two internet
// targets, plus a DNS lookup now and then.
#pragma once
#include <Arduino.h>

namespace ping {

constexpr int TARGETS = 3;
enum Target { ROUTER = 0, TARGET1 = 1, TARGET2 = 2 };

struct Round {
  int16_t rtt[TARGETS];  // milliseconds, -1 = no reply
  uint32_t monoSec;      // seconds since boot when the round started
  bool linkUp;           // the network link was up when the round started
  bool wired;            // that link is (or was last) a wired port, not WiFi
  int8_t dns;            // 1 = lookup answered, 0 = failed, -1 = not checked this round
};

// Forces results, for testing classification without unplugging anything.
enum class Sim : uint8_t { NONE, ISP, WIFI, SLOW, DNS };

void begin();
bool nextRound(Round &out);  // non-blocking; true if a finished round was returned
void setSim(Sim mode);

}  // namespace ping
