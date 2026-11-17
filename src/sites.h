// Website checks: up to five sites from Settings, fetched one at a time on
// a slow rotation from a background task. A site is DOWN after three failed
// checks in a row (no response, timeout or a 5xx status). HTTPS sites are
// verified against the CA bundle and their certificate's expiry is read.
//
// Checks stop while the internet itself is down or DNS is failing, so an ISP
// outage is not reported as five dead websites. Nothing here touches the
// outage log or the internet uptime figures.
#pragma once
#include <Arduino.h>

#include "settings.h"

namespace sites {

constexpr size_t LOG_LEN = 20;                 // down periods kept per site
constexpr uint32_t MIN_INTERVAL_SEC = 60;      // per site, never more often
constexpr uint32_t GAP_SEC = 30;               // between any two checks
constexpr uint32_t RESUME_DELAY_SEC = 60;      // after the internet comes back
constexpr uint8_t FAILS_FOR_DOWN = 3;
constexpr int CERT_WARN_DAYS = 14;
constexpr int CERT_URGENT_DAYS = 3;
constexpr uint32_t TIMEOUT_MS = 8000;

enum class State : uint8_t { WAITING, UP, DOWN };

struct Down {
  uint32_t start;     // unix time of the first failed check
  uint32_t duration;  // seconds
};

struct View {
  bool used;
  State state;
  uint8_t fails;          // failed checks in a row
  int16_t code;           // HTTP status of the latest check, 0 = none
  int32_t ms;             // time to connect plus time to answer, -1 = none
  char reason[28];        // why the latest check failed, "" when it did not
  uint32_t checkedAt;     // unix time of the latest check, 0 = never
  uint32_t since;         // unix time of the first check of this address
  uint32_t downStart;     // when the current down period began, 0 = not down
  bool https;
  uint32_t certExpiry;    // unix time, 0 = not known
  float pct24h;           // NAN before the first check
  float pct7d;
  size_t downCount;
  Down downs[LOG_LEN];    // newest first
};

void begin();  // call after storage::begin(); does nothing with no sites saved
void snapshot(size_t index, View &out);
bool any();    // at least one site is configured

// For testing from the serial console: the next check of the first site
// reads its certificate as expiring in ten days.
void simulateCert();

}  // namespace sites
