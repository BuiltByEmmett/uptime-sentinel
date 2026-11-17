// Everything the user configures, kept in NVS: WiFi credentials, the Discord
// webhook URL, timezone, ping targets and the optional admin password.
// Secrets are never compiled into the firmware, printed to serial or sent
// back to a browser. The admin password is kept only as a salted hash.
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
  bool hookAnnounced = false;  // the "Uptime Sentinel online" message has been delivered
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

// Optional admin password for the dashboard's Settings and firmware update
// pages. Only a random salt and a PBKDF2-HMAC-SHA256 hash are stored.
constexpr size_t ADMIN_MIN_LEN = 8;
constexpr size_t ADMIN_MAX_LEN = 64;
bool hasAdmin();
bool validAdminPassword(const String &text);
bool setAdminPassword(const String &password);  // empty removes it
bool checkAdminPassword(const String &password);

uint32_t targetAddr(int index);      // network byte order, for the ping task
const char *targetLabel(int index);  // short name for the screen

// When the monitor first had a valid clock; the start of "Watching for".
uint32_t since();
void setSince(uint32_t epoch);

}  // namespace settings
