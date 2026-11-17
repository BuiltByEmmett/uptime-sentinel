// The outage log as shown to people: the saved entries, newest first, with
// whatever is happening right now on top and still counting.
#pragma once
#include <Arduino.h>

#include "config.h"

namespace logview {

struct Row {
  uint32_t start;     // unix time, 0 = not known yet
  uint32_t duration;  // seconds (so far, for an active row)
  Cause cause;
  bool active;
  bool newAddress;  // the public IP address was different after this outage
};

size_t count();
Row get(size_t index);  // 0 = newest

}  // namespace logview
