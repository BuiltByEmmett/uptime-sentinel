#include "settings.h"

#include <Preferences.h>

namespace settings {
namespace {

constexpr char NS_CONFIG[] = "cfg";
constexpr char NS_STATE[] = "state";

Config config;
uint32_t sinceEpoch = 0;

void putBool(const char *key, bool value) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putBool(key, value);
  p.end();
}

}  // namespace

void begin() {
  Preferences p;
  p.begin(NS_CONFIG, false);
  config.ssid = p.getString("ssid", "");
  config.pass = p.getString("pass", "");
  config.hook = p.getString("hook", "");
  config.verified = p.getBool("verified", false);
  config.hookAnnounced = p.getBool("announced", false);
  p.end();

  p.begin(NS_STATE, false);
  sinceEpoch = p.getUInt("since", 0);
  p.end();
}

const Config &get() { return config; }

bool hasWifi() { return config.ssid.length() > 0; }

void saveSetup(const String &ssid, const String &pass, const String &hook) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putBool("verified", false);
  if (hook.length() > 0 && hook != config.hook) {
    p.putString("hook", hook);
    p.putBool("announced", false);
    config.hook = hook;
    config.hookAnnounced = false;
  }
  p.end();
  config.ssid = ssid;
  config.pass = pass;
  config.verified = false;
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
  config = Config();
}

uint32_t since() { return sinceEpoch; }

void setSince(uint32_t epoch) {
  sinceEpoch = epoch;
  Preferences p;
  p.begin(NS_STATE, false);
  p.putUInt("since", epoch);
  p.end();
}

}  // namespace settings
