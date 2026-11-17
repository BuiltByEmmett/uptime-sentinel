// Outage logic: turns rounds into ONLINE, an outage (ISP, WIFI or LAN) or a
// degraded state (SLOW or DNS), and keeps one hour of latency history.
#pragma once
#include <Arduino.h>

#include "config.h"
#include "ping.h"

namespace monitor {

enum class RoundClass : uint8_t { OK, ISP, WIFI, LAN };

struct State {
  bool haveData = false;

  // Outage: the internet is unreachable.
  bool outage = false;
  Cause cause = Cause::ISP;
  uint32_t outageStartMono = 0;

  // Degraded: reachable, but slow, lossy or without working DNS. Never set
  // while an outage is in progress.
  bool degraded = false;
  Cause degradedCause = Cause::SLOW;
  uint32_t degradedStartMono = 0;

  int16_t rtt[ping::TARGETS] = {-1, -1, -1};
  int16_t internetMs = -1;      // last round's internet latency, -1 = failed
  int8_t dnsOk = -1;            // result of the latest DNS lookup, -1 = none yet
  int16_t baselineMs = -1;      // this connection's normal latency, -1 = still learning
  RoundClass lastClass = RoundClass::OK;
};

struct Summary {
  uint16_t samples = 0;
  int16_t minMs = -1;
  int16_t avgMs = -1;
  int16_t maxMs = -1;
  int16_t jitterMs = -1;
  uint8_t lossPct = 0;
};

struct Handlers {
  void (*onStart)(Cause cause, uint32_t startMono);
  // recovered = the problem went away, as opposed to turning into a
  // different one (for example an ISP outage followed by WiFi dropping).
  void (*onEnd)(Cause cause, uint32_t startMono, uint32_t endMono, bool recovered);
};

constexpr uint16_t HIST_FAIL = 0xFFFF;

void begin(const Handlers &handlers);
void onRound(const ping::Round &round);
const State &state();

// The outage or degraded period in progress, if any.
bool activeEvent(Cause &cause, uint32_t &startMono);

RoundClass classify(const ping::Round &round);
const char *className(RoundClass c);
const char *causeName(Cause c);
const char *stateName();              // ONLINE, OFFLINE, SLOW, NO DNS or "..."

size_t historyCount();
uint16_t historyAt(size_t age);       // age 0 = newest; ms or HIST_FAIL
Summary summarize(size_t rounds);     // over the newest `rounds` rounds

}  // namespace monitor
