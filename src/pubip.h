// Public IP log: asks a plain-text HTTPS service what address the internet
// sees this network as, every ten minutes and right after an outage ends,
// and keeps the last 50 changes on flash.
//
// The address is private. It is kept on the device and shown on one
// dashboard page. It never goes into an alert, the serial log, a screenshot
// or the outage CSV.
#pragma once
#include <Arduino.h>

extern const char LOOKUP_ROOT_CAS[];  // src/ca_roots.cpp

namespace pubip {

constexpr size_t CAPACITY = 50;

struct Change {
  uint32_t time;  // unix time the new address was first seen
  uint32_t from;  // addresses in network byte order
  uint32_t to;
};

struct Snapshot {
  bool enabled;
  uint32_t current;     // 0 = not known yet
  uint32_t checkedAt;   // unix time of the last successful lookup, 0 = never
  char status[64];      // how the latest lookup went
  size_t count;
  Change changes[CAPACITY];  // newest first
};

void begin();        // call after storage::begin(); does nothing when switched off
void afterOutage();  // an outage just ended: look the address up again now
void snapshot(Snapshot &out);

// For testing from the serial console.
void simulateChange();  // the next lookup, whenever it comes, reports 203.0.113.7
void clear();           // forget the address and erase the change log

}  // namespace pubip
