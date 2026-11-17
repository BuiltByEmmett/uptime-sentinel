// Outage log on LittleFS: the last 200 outages, written so that a power cut
// at any moment leaves either the old log or the new one, never a torn file.
#pragma once
#include <Arduino.h>

#include "config.h"

namespace storage {

struct OutageRecord {
  uint32_t start;     // unix time the outage began
  uint32_t duration;  // seconds
  uint8_t cause;      // Cause
  uint8_t reserved[3];
};

void begin();
void loop();  // flushes outages that ended before the clock was set

void outageStarted(Cause cause, uint32_t startMono);
void outageEnded(Cause cause, uint32_t startMono, uint32_t endMono);

size_t count();
const OutageRecord &get(size_t index);  // 0 = newest
void clear();

}  // namespace storage
