// Everything the user configures, kept in NVS: WiFi credentials, the Discord
// webhook URL, timezone and ping targets. Secrets are never compiled into
// the firmware, printed to serial or sent back to a browser.
#pragma once
#include <Arduino.h>

namespace settings {

struct Config {
  String ssid;
  String pass;
  String hook;
  String tz;                   // POSIX TZ string
  String target[2];            // the two internet ping targets, dotted IPv4
  bool verified = false;       // these credentials have joined the network at least once
  bool hookAnnounced = false;  // the "Uptime monitor online" message has been delivered
};

struct Zone {
  const char *label;
  const char *tz;
};
extern const Zone ZONES[];
extern const size_t ZONE_COUNT;

constexpr char DEFAULT_TZ[] = "EST5EDT,M3.2.0,M11.1.0";  // America/New_York
constexpr char DEFAULT_TARGET1[] = "1.1.1.1";
constexpr char DEFAULT_TARGET2[] = "8.8.8.8";

void begin();
const Config &get();
bool hasWifi();

void saveWifi(const String &ssid, const String &pass);
void saveHook(const String &hook);  // empty removes it
bool saveOptions(const String &tz, const String &target1, const String &target2);
void setVerified();
void setHookAnnounced();
void wipe();

bool validIp(const String &text);
bool validTz(const String &text);
bool validHook(const String &text);

uint32_t targetAddr(int index);      // network byte order, for the ping task
const char *targetLabel(int index);  // short name for the screen

// When the monitor first had a valid clock; the start of "Watching for".
uint32_t since();
void setSince(uint32_t epoch);

}  // namespace settings
