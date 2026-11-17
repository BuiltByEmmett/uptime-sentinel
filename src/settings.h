// WiFi credentials and the Discord webhook URL, kept in NVS. Nothing here is
// ever compiled into the firmware or printed to serial.
#pragma once
#include <Arduino.h>

namespace settings {

struct Config {
  String ssid;
  String pass;
  String hook;
  bool verified = false;       // these credentials have joined the network at least once
  bool hookAnnounced = false;  // the "Uptime monitor online" message has been delivered
};

void begin();
const Config &get();
bool hasWifi();

// An empty hook keeps whatever webhook is already saved.
void saveSetup(const String &ssid, const String &pass, const String &hook);
void setVerified();
void setHookAnnounced();
void wipe();

// When the monitor first had a valid clock; the start of "Watching for".
uint32_t since();
void setSince(uint32_t epoch);

}  // namespace settings
