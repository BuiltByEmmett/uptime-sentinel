// First-boot setup: an open access point with a captive portal page for
// picking the WiFi network, entering its password and an optional Discord
// webhook URL. Saves to NVS and reboots.
#pragma once
#include <Arduino.h>

namespace portal {

// Never returns. `notice` is shown on the page and screen (for example when
// the saved password did not work). `idle` is called on every loop pass and returns true when it drew over the screen.
void run(const char *notice, bool (*idle)());

}  // namespace portal
