#include "settings.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace settings {

const Zone ZONES[] = {
    {"Hawaii (Honolulu)", "HST10"},
    {"Alaska (Anchorage)", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Pacific (Los Angeles)", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Arizona (Phoenix)", "MST7"},
    {"US Mountain (Denver)", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Central (Chicago)", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern (New York)", "EST5EDT,M3.2.0,M11.1.0"},
    {"Atlantic (Halifax)", "AST4ADT,M3.2.0,M11.1.0"},
    {"Mexico City", "CST6"},
    {"Brazil (Sao Paulo)", "<-03>3"},
    {"UTC", "UTC0"},
    {"UK and Ireland (London)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe (Paris, Berlin)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe (Athens, Helsinki)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"West Africa (Lagos)", "WAT-1"},
    {"South Africa (Johannesburg)", "SAST-2"},
    {"Turkey, Moscow, East Africa", "<+03>-3"},
    {"Gulf (Dubai)", "<+04>-4"},
    {"Pakistan (Karachi)", "PKT-5"},
    {"India (Kolkata)", "IST-5:30"},
    {"Bangladesh (Dhaka)", "<+06>-6"},
    {"Indochina (Bangkok, Jakarta)", "<+07>-7"},
    {"China, Singapore, Perth", "<+08>-8"},
    {"Japan and Korea (Tokyo, Seoul)", "JST-9"},
    {"Australia East (Sydney)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia (Brisbane)", "AEST-10"},
    {"New Zealand (Auckland)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
const size_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

namespace {

constexpr char NS_CONFIG[] = "cfg";
constexpr char NS_STATE[] = "state";

Config config;
uint32_t sinceEpoch = 0;
uint32_t targetIp[2] = {0, 0};
char targetName[2][10] = {"", ""};

constexpr size_t SALT_LEN = 16;
constexpr size_t HASH_LEN = 32;
constexpr uint32_t ADMIN_ROUNDS = 4000;  // well under a second on an ESP32
uint8_t adminSalt[SALT_LEN];
uint8_t adminHash[HASH_LEN];
bool adminSet = false;

bool deriveAdmin(const String &password, const uint8_t *salt, uint8_t *out) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
            mbedtls_pkcs5_pbkdf2_hmac(&ctx,
                                      reinterpret_cast<const unsigned char *>(password.c_str()),
                                      password.length(), salt, SALT_LEN, ADMIN_ROUNDS, HASH_LEN,
                                      out) == 0;
  mbedtls_md_free(&ctx);
  return ok;
}

void putBool(const char *key, bool value) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putBool(key, value);
  p.end();
}

void refreshTargets() {
  for (int i = 0; i < 2; i++) {
    IPAddress ip;
    if (!ip.fromString(config.target[i])) {
      config.target[i] = i == 0 ? DEFAULT_TARGET1 : DEFAULT_TARGET2;
      ip.fromString(config.target[i]);
    }
    targetIp[i] = static_cast<uint32_t>(ip);
    // The status strip has room for nine characters per column.
    if (config.target[i].length() <= 9) {
      strlcpy(targetName[i], config.target[i].c_str(), sizeof(targetName[i]));
    } else {
      snprintf(targetName[i], sizeof(targetName[i]), "TARGET %d", i + 1);
    }
  }
}

}  // namespace

void begin() {
  Preferences p;
  p.begin(NS_CONFIG, false);
  config.ssid = p.getString("ssid", "");
  config.pass = p.getString("pass", "");
  config.hook = p.getString("hook", "");
  config.ntfyUrl = p.getString("ntfyurl", "");
  config.ntfyToken = p.getString("ntfytok", "");
  config.tgToken = p.getString("tgtok", "");
  config.tgChat = p.getString("tgchat", "");
  config.webhook = p.getString("whurl", "");
  config.tz = p.getString("tz", DEFAULT_TZ);
  config.target[0] = p.getString("target1", DEFAULT_TARGET1);
  config.target[1] = p.getString("target2", DEFAULT_TARGET2);
  config.ipEnabled = p.getBool("ipon", true);
  config.ipUrl[0] = p.getString("ipurl1", DEFAULT_IP_URL1);
  config.ipUrl[1] = p.getString("ipurl2", DEFAULT_IP_URL2);
  for (size_t i = 0; i < MAX_SITES; i++) {
    char key[8];
    snprintf(key, sizeof(key), "s%un", static_cast<unsigned>(i));
    config.site[i].name = p.getString(key, "");
    snprintf(key, sizeof(key), "s%uu", static_cast<unsigned>(i));
    config.site[i].url = p.getString(key, "");
  }
  config.siteMetrics = p.getBool("sitemet", false);
  config.deviceScan = p.getBool("devscan", true);
  config.deviceAlert = p.getBool("devalert", false);
  config.verified = p.getBool("verified", false);
  config.hookAnnounced = p.getBool("announced", false);
  adminSet = p.isKey("asalt") && p.isKey("ahash") &&
             p.getBytes("asalt", adminSalt, SALT_LEN) == SALT_LEN &&
             p.getBytes("ahash", adminHash, HASH_LEN) == HASH_LEN;
  p.end();
  if (!validTz(config.tz)) config.tz = DEFAULT_TZ;
  refreshTargets();

  p.begin(NS_STATE, false);
  sinceEpoch = p.getUInt("since", 0);
  p.end();
}

const Config &get() { return config; }

bool hasWifi() { return config.ssid.length() > 0; }

void saveWifi(const String &ssid, const String &pass) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putBool("verified", false);
  p.end();
  config.ssid = ssid;
  config.pass = pass;
  config.verified = false;
}

void saveHook(const String &hook) {
  if (hook == config.hook) return;
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("hook", hook);
  p.putBool("announced", false);
  p.end();
  config.hook = hook;
  config.hookAnnounced = false;
}

void saveNtfy(const String &url, const String &token) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("ntfyurl", url);
  p.putString("ntfytok", url.length() ? token : String(""));
  p.end();
  config.ntfyUrl = url;
  config.ntfyToken = url.length() ? token : String("");
}

void saveTelegram(const String &token, const String &chat) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("tgtok", token);
  p.putString("tgchat", token.length() ? chat : String(""));
  p.end();
  config.tgToken = token;
  config.tgChat = token.length() ? chat : String("");
}

void saveWebhook(const String &url) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("whurl", url);
  p.end();
  config.webhook = url;
}

bool saveOptions(const String &tz, const String &target1, const String &target2) {
  if (!validTz(tz) || !validIp(target1) || !validIp(target2)) return false;
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("tz", tz);
  p.putString("target1", target1);
  p.putString("target2", target2);
  p.end();
  config.tz = tz;
  config.target[0] = target1;
  config.target[1] = target2;
  refreshTargets();
  return true;
}

void saveSites(const Site sites[MAX_SITES], bool inMetrics) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  for (size_t i = 0; i < MAX_SITES; i++) {
    char key[8];
    snprintf(key, sizeof(key), "s%un", static_cast<unsigned>(i));
    if (sites[i].name != config.site[i].name) p.putString(key, sites[i].name);
    snprintf(key, sizeof(key), "s%uu", static_cast<unsigned>(i));
    if (sites[i].url != config.site[i].url) p.putString(key, sites[i].url);
    config.site[i] = sites[i];
  }
  p.putBool("sitemet", inMetrics);
  p.end();
  config.siteMetrics = inMetrics;
}

void saveDevices(bool scan, bool alertOnNew) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putBool("devscan", scan);
  p.putBool("devalert", alertOnNew);
  p.end();
  config.deviceScan = scan;
  config.deviceAlert = alertOnNew;
}

bool saveIpLookup(bool enabled, const String &url1, const String &url2) {
  if (!validLookupUrl(url1) || !validLookupUrl(url2)) return false;
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putBool("ipon", enabled);
  p.putString("ipurl1", url1);
  p.putString("ipurl2", url2);
  p.end();
  config.ipEnabled = enabled;
  config.ipUrl[0] = url1;
  config.ipUrl[1] = url2;
  return true;
}

void setVerified() {
  config.verified = true;
  putBool("verified", true);
}

void setHookAnnounced() {
  config.hookAnnounced = true;
  putBool("announced", true);
}

void wipe() {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.clear();
  p.end();
  adminSet = false;
  config = Config();
  config.tz = DEFAULT_TZ;
  config.target[0] = DEFAULT_TARGET1;
  config.target[1] = DEFAULT_TARGET2;
  config.ipUrl[0] = DEFAULT_IP_URL1;
  config.ipUrl[1] = DEFAULT_IP_URL2;
  refreshTargets();
}

bool validIp(const String &text) {
  IPAddress ip;
  return text.length() >= 7 && text.length() <= 15 && ip.fromString(text) &&
         static_cast<uint32_t>(ip) != 0;
}

// POSIX TZ strings are short and use a small alphabet; refusing anything
// else keeps junk out of the C library's parser.
bool validTz(const String &text) {
  if (text.length() < 3 || text.length() > 48) return false;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    bool ok = isalnum(static_cast<unsigned char>(c)) || c == ',' || c == '.' || c == '/' ||
              c == ':' || c == '+' || c == '-' || c == '<' || c == '>';
    if (!ok) return false;
  }
  return true;
}

bool validHook(const String &text) {
  return text.length() == 0 || (text.startsWith("https://") && text.length() <= 240);
}

bool validLookupUrl(const String &text) {
  if (!text.startsWith("https://") || text.length() < 12 || text.length() > IP_URL_MAX_LEN) {
    return false;
  }
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c <= ' ' || c > '~' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\') {
      return false;
    }
  }
  return true;
}

bool validAlertUrl(const String &text) {
  if (text.length() == 0) return true;
  if (!(text.startsWith("https://") || text.startsWith("http://")) || text.length() < 11 ||
      text.length() > 240) {
    return false;
  }
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c <= ' ' || c > '~' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\') {
      return false;
    }
  }
  return true;
}

bool validSiteUrl(const String &text) {
  if (text.length() == 0 || text.length() > SITE_URL_MAX_LEN || !validAlertUrl(text)) return false;
  int start = text.startsWith("https://") ? 8 : 7;
  char first = text[start];
  return isAlphaNumeric(first) || first == '[';
}

bool validSiteName(const String &text) {
  if (text.length() == 0 || text.length() > SITE_NAME_MAX_LEN) return false;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c < ' ' || c > '~' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' ||
        c == '\\') {
      return false;
    }
  }
  return true;
}

bool validToken(const String &text) {
  if (text.length() > 120) return false;
  for (size_t i = 0; i < text.length(); i++) {
    if (text[i] <= ' ' || text[i] > '~') return false;
  }
  return true;
}

// The token becomes part of a URL, so only the characters Telegram uses.
bool validTelegramToken(const String &text) {
  if (text.length() == 0) return true;
  int colon = text.indexOf(':');
  if (colon < 1 || text.length() > 80 || text.length() - colon < 10) return false;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (static_cast<int>(i) < colon ? !isDigit(c)
                                    : !(i == static_cast<size_t>(colon) || isAlphaNumeric(c) ||
                                        c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool validTelegramChat(const String &text) {
  if (text.length() == 0) return true;
  if (text.length() > 40) return false;
  if (text[0] == '@') {
    for (size_t i = 1; i < text.length(); i++) {
      if (!isAlphaNumeric(text[i]) && text[i] != '_') return false;
    }
    return text.length() > 1;
  }
  for (size_t i = text[0] == '-' ? 1 : 0; i < text.length(); i++) {
    if (!isDigit(text[i])) return false;
  }
  return isDigit(text[text.length() - 1]);
}

bool hasAdmin() { return adminSet; }

bool validAdminPassword(const String &text) {
  return text.length() >= ADMIN_MIN_LEN && text.length() <= ADMIN_MAX_LEN;
}

bool setAdminPassword(const String &password) {
  Preferences p;
  if (password.length() == 0) {
    p.begin(NS_CONFIG, false);
    p.remove("asalt");
    p.remove("ahash");
    p.end();
    adminSet = false;
    return true;
  }
  if (!validAdminPassword(password)) return false;
  uint8_t salt[SALT_LEN], hash[HASH_LEN];
  esp_fill_random(salt, SALT_LEN);
  if (!deriveAdmin(password, salt, hash)) return false;
  p.begin(NS_CONFIG, false);
  bool stored = p.putBytes("asalt", salt, SALT_LEN) == SALT_LEN &&
                p.putBytes("ahash", hash, HASH_LEN) == HASH_LEN;
  p.end();
  if (!stored) return false;
  memcpy(adminSalt, salt, SALT_LEN);
  memcpy(adminHash, hash, HASH_LEN);
  adminSet = true;
  return true;
}

bool checkAdminPassword(const String &password) {
  if (!adminSet || password.length() > ADMIN_MAX_LEN) return false;
  uint8_t hash[HASH_LEN];
  if (!deriveAdmin(password, adminSalt, hash)) return false;
  uint8_t diff = 0;  // compare every byte, so timing says nothing about where they differ
  for (size_t i = 0; i < HASH_LEN; i++) diff |= hash[i] ^ adminHash[i];
  return diff == 0;
}

uint32_t targetAddr(int index) { return targetIp[index]; }

const char *targetLabel(int index) { return targetName[index]; }

uint32_t since() { return sinceEpoch; }

void setSince(uint32_t epoch) {
  sinceEpoch = epoch;
  Preferences p;
  p.begin(NS_STATE, false);
  p.putUInt("since", epoch);
  p.end();
}

}  // namespace settings
