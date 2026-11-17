// Uptime figures. Only ISP outages count; WIFI events are ignored here.
#pragma once
#include <Arduino.h>

namespace stats {

struct Stats {
  bool clockValid;
  float pct24h;         // NAN until the clock is set
  float pct7d;
  uint16_t ispOutages;  // ISP outages in the log
  uint32_t longestSec;  // longest ISP outage in the log, 0 = none
  uint32_t watchingSec;
};

Stats compute();

}  // namespace stats
