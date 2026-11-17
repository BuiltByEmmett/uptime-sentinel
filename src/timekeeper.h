// Wall-clock time from NTP (America/New_York) plus a monotonic clock that
// works from the moment of boot, with or without internet.
#pragma once
#include <Arduino.h>

namespace timekeeper {

void begin();                 // call once WiFi has been started
bool valid();                 // true once NTP has set the clock
uint32_t now();               // unix time (only meaningful when valid)
uint32_t mono();              // seconds since boot
uint32_t toEpoch(uint32_t monoSec);  // convert a seconds-since-boot stamp

void formatStamp(uint32_t epoch, char *out, size_t len);     // "11/16 14:02"
void formatClock(uint32_t epoch, char *out, size_t len);     // "14:03"
void formatDuration(uint32_t seconds, char *out, size_t len);  // "1m 12s"

}  // namespace timekeeper
