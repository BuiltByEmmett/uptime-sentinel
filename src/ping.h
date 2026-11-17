// One round every two seconds, on its own task so a slow reply never stalls
// the display or buttons: ICMP echo to the gateway, the two internet targets
// and (when it can be found and answers) the first ISP hop, plus a DNS
// lookup now and then.
#pragma once
#include <Arduino.h>

namespace ping {

constexpr int TARGETS = 4;
enum Target { ROUTER = 0, TARGET1 = 1, TARGET2 = 2, HOP = 3 };

// The first hop beyond the gateway, found by sending an echo request that
// expires there (TTL 2) and reading who reports it. Only a hop that has a
// public address and answers pings is used.
enum class HopState : uint8_t {
  SEARCHING,  // not looked up yet (no internet since boot)
  ACTIVE,     // found, answers pings: pinged every round
  NO_REPLY,   // found, but it ignores pings
  PRIVATE,    // found, but in a private range: probably a second router (double NAT)
  NONE,       // nothing reported the expired packet
};
struct HopInfo {
  HopState state;
  uint32_t addr;  // network byte order, 0 = none
};

struct Round {
  int16_t rtt[TARGETS];  // milliseconds, -1 = no reply
  uint32_t monoSec;      // seconds since boot when the round started
  bool linkUp;           // the network link was up when the round started
  bool wired;            // that link is (or was last) a wired port, not WiFi
  int8_t dns;            // 1 = lookup answered, 0 = failed, -1 = not checked this round
  bool hopActive;        // the first ISP hop was pinged this round (rtt[HOP] means something)
};

// Forces results, for testing classification without unplugging anything.
//   ISP       the internet targets stop replying, the first ISP hop still does
//   ISPLOCAL  the internet targets and the first ISP hop stop replying
enum class Sim : uint8_t { NONE, ISP, ISPLOCAL, WIFI, SLOW, DNS };

void begin();
bool nextRound(Round &out);  // non-blocking; true if a finished round was returned
void setSim(Sim mode);
HopInfo hopInfo();

}  // namespace ping
