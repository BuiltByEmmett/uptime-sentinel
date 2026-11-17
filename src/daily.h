// Uptime calendar: one small summary per local calendar day (ISP downtime in
// seconds and how many ISP outages began that day), kept on flash for 90
// days. Only days that had downtime are stored; a day without an entry had
// none. The outage log holds 200 entries at most, so it cannot answer this
// on its own once a bad week has pushed older outages out.
#pragma once
#include <Arduino.h>

namespace daily {

constexpr size_t DAYS = 90;

struct Entry {
  uint16_t day;      // local calendar date as days since 1970-01-01
  uint16_t outages;  // ISP outages that began on this day
  uint32_t downSec;  // ISP downtime that fell on this day
};

void begin();  // call after storage::begin()
void loop();

// An ISP outage has been written to the log. Downtime that crosses midnight
// is split between the days. Safe to call before begin().
void add(uint32_t start, uint32_t duration);

// The stored days, oldest first, with the outage in progress (if any) added.
size_t collect(Entry *out, size_t max);

uint16_t dayOf(uint32_t epoch);
void formatDay(uint16_t day, char *out, size_t len);  // "2026-11-17"

// For testing from the serial console.
void simulate();  // adds a spread of made-up days
void clear();     // erases the calendar and rebuilds it from the outage log

}  // namespace daily
