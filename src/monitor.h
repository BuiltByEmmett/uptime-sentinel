// Outage logic: turns ping rounds into ONLINE / ISP outage / WIFI outage and
// keeps one hour of latency history for the graph.
#pragma once
#include <Arduino.h>

#include "config.h"
#include "ping.h"

namespace monitor {

enum class RoundClass : uint8_t { OK, ISP, WIFI };

struct State {
  bool haveData = false;
  bool outage = false;          // an outage has been declared and is ongoing
  Cause cause = Cause::ISP;
  uint32_t outageStartMono = 0;
  int16_t rtt[ping::TARGETS] = {-1, -1, -1};
  int16_t internetMs = -1;      // last round's internet latency, -1 = failed
  RoundClass lastClass = RoundClass::OK;
};

struct Summary {
  uint16_t samples = 0;
  int16_t minMs = -1;
  int16_t avgMs = -1;
  int16_t maxMs = -1;
  uint8_t lossPct = 0;
};

struct Handlers {
  void (*onStart)(Cause cause, uint32_t startMono);
  // recovered = ended because the internet came back, as opposed to the
  // outage changing cause (for example ISP outage followed by WiFi dropping).
  void (*onEnd)(Cause cause, uint32_t startMono, uint32_t endMono, bool recovered);
};

constexpr uint16_t HIST_FAIL = 0xFFFF;

void begin(const Handlers &handlers);
void onRound(const ping::Round &round);
const State &state();

RoundClass classify(const ping::Round &round);
const char *className(RoundClass c);
const char *causeName(Cause c);

size_t historyCount();
uint16_t historyAt(size_t age);       // age 0 = newest; ms or HIST_FAIL
Summary summarize(size_t rounds);     // over the newest `rounds` rounds

}  // namespace monitor
