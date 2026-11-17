// Everything the user configures, kept in NVS: WiFi credentials, the alert
// channels (Discord, ntfy, Telegram, generic webhook), timezone, ping targets, the public IP lookup and the optional
// admin password.
// Secrets are never compiled into the firmware, printed to serial or sent
// back to a browser. The admin password is kept only as a salted hash.
#pragma once
#include <Arduino.h>

namespace settings {

constexpr size_t MAX_SITES = 5;
constexpr size_t SITE_NAME_MAX_LEN = 24;
constexpr size_t SITE_URL_MAX_LEN = 120;

struct Site {
  String name;
  String url;  // http:// or https://; empty = slot not used
};

struct Config {
  String ssid;
  String pass;
  String hook;                 // Discord webhook URL
  String ntfyUrl;              // ntfy topic URL, e.g. https://ntfy.sh/my-topic
  String ntfyToken;            // ntfy access token, optional
  String tgToken;              // Telegram bot token
  String tgChat;               // Telegram chat ID
  String webhook;              // generic webhook URL, receives a JSON POST
  String tz;                   // POSIX TZ string
  String target[2];            // the two internet ping targets, dotted IPv4
  bool ipEnabled = true;       // look up and log the public IP address
  String ipUrl[2];             // plain-text HTTPS lookup services; the second is the fallback
  Site site[MAX_SITES];        // websites to check
  bool siteMetrics = false;    // include the websites in /metrics
  bool deviceScan = true;      // look for other devices on the local network
  bool deviceAlert = false;    // send an alert when a device never seen before joins
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
constexpr char DEFAULT_IP_URL1[] = "https://api.ipify.org";
constexpr char DEFAULT_IP_URL2[] = "https://icanhazip.com";
constexpr size_t IP_URL_MAX_LEN = 120;

void begin();
const Config &get();
bool hasWifi();

void saveWifi(const String &ssid, const String &pass);
void saveHook(const String &hook);  // empty removes it
// The other alert channels. An empty URL (or bot token) removes the channel.
void saveNtfy(const String &url, const String &token);
void saveTelegram(const String &token, const String &chat);
void saveWebhook(const String &url);
bool saveOptions(const String &tz, const String &target1, const String &target2);
void saveSites(const Site sites[MAX_SITES], bool inMetrics);
void saveDevices(bool scan, bool alertOnNew);
bool saveIpLookup(bool enabled, const String &url1, const String &url2);
void setVerified();
void setHookAnnounced();
void wipe();

bool validIp(const String &text);
bool validTz(const String &text);
bool validHook(const String &text);
bool validLookupUrl(const String &text);  // https://, no spaces; empty is not valid
bool validAlertUrl(const String &text);   // http:// or https://, no spaces; empty is valid
bool validSiteUrl(const String &text);     // as an alert URL, 120 at most, not empty
bool validSiteName(const String &text);    // 1 to 24 plain characters, nothing HTML could misread
bool validToken(const String &text);      // printable, no spaces, 120 at most; empty is valid
bool validTelegramToken(const String &text);  // digits, a colon, then letters, digits, _ or -
bool validTelegramChat(const String &text);   // a number, or @channelname

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
