// Dashboard on the local network: http://uptime.local (or the IP address).
// Live status, the full outage log, a CSV download and a settings page.
// Reachable only from the same network; it never sends saved secrets back.
#pragma once
#include <Arduino.h>

namespace web {

void begin();  // call after WiFi has been started
void loop();

}  // namespace web
