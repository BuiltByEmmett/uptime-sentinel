// Internet uptime monitor for the LilyGO / TENSTAR T-Display (ESP32).
// Wires the modules together; the logic lives in the files named after it.
#include <Arduino.h>
#include <WiFi.h>

#include "alerts.h"
#include "buttons.h"
#include "config.h"
#include "display.h"
#include "monitor.h"
#include "ping.h"
#include "portal.h"
#include "screens.h"
#include "settings.h"
#include "stats.h"
#include "storage.h"
#include "timekeeper.h"

namespace {

constexpr char VERSION[] = "1.0.0";
constexpr uint32_t REDRAW_MS = 250;
constexpr uint32_t RECONNECT_MS = 30000;

uint32_t ispDownCarry = 0;  // ISP downtime in the current run of back-to-back outages
const char *simName = "";

// Both buttons held: show a countdown, wipe settings at five seconds.
// Returns true when the countdown was abandoned and the screen needs redrawing.
bool serviceWipe() {
  static bool overlay = false;
  uint32_t held = buttons::bothHeldMs();
  if (held >= cfg::WIPE_HOLD_MS) {
    display::message("RESET", "Settings wiped.", "Restarting...");
    settings::wipe();
    WiFi.disconnect(true, true);
    Serial.println("[boot] settings wiped by button hold");
    delay(1200);
    ESP.restart();
  }
  if (held >= 600) {
    char line[28];
    snprintf(line, sizeof(line), "Keep holding: %us",
             static_cast<unsigned>((cfg::WIPE_HOLD_MS - held + 999) / 1000));
    display::message("RESET", "Wipe WiFi and webhook?", line, "Release to cancel");
    overlay = true;
    return false;
  }
  if (overlay) {
    overlay = false;
    return true;
  }
  return false;
}

bool portalIdle() {
  buttons::poll();
  return serviceWipe();
}

void onOutageStart(Cause cause, uint32_t startMono) { storage::outageStarted(cause, startMono); }

void onOutageEnd(Cause cause, uint32_t startMono, uint32_t endMono, bool recovered) {
  storage::outageEnded(cause, startMono, endMono);
  if (cause == Cause::ISP) ispDownCarry += endMono - startMono;
  if (recovered) {
    // One Discord message per ISP outage, sent when the internet is back.
    if (ispDownCarry > 0) {
      alerts::outageEnded(ispDownCarry, timekeeper::valid() ? timekeeper::now() : 0);
    }
    ispDownCarry = 0;
  }
}

void printRound(const ping::Round &r) {
  static const char *const NAMES[ping::TARGETS] = {"router", "1.1.1.1", "8.8.8.8"};
  Serial.print("[ping]");
  for (int i = 0; i < ping::TARGETS; i++) {
    if (r.rtt[i] >= 0) Serial.printf(" %s=%dms", NAMES[i], r.rtt[i]);
    else Serial.printf(" %s=--", NAMES[i]);
  }
  const monitor::State &m = monitor::state();
  Serial.printf(" -> %s%s | %s\n", monitor::className(m.lastClass), simName,
                m.outage ? (m.cause == Cause::ISP ? "OFFLINE (ISP)" : "OFFLINE (WIFI)")
                         : "ONLINE");
}

void serviceWifi() {
  static bool wasUp = false;
  static uint32_t lastKick = 0;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) {
    wasUp = up;
    if (up) {
      Serial.printf("[wifi] connected ip=%s gateway=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                    WiFi.RSSI());
    } else {
      Serial.println("[wifi] disconnected");
      lastKick = millis();
    }
  }
  if (!up && millis() - lastKick >= RECONNECT_MS) {
    lastKick = millis();
    WiFi.reconnect();
  }
}

void serviceClock() {
  static bool announced = false;
  if (announced || !timekeeper::valid()) return;
  announced = true;
  time_t t = timekeeper::now();
  struct tm tm;
  localtime_r(&t, &tm);
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z", &tm);
  Serial.printf("[time] synced: %s\n", text);
  // "Watching for" starts at the first boot that ever got the time.
  if (settings::since() == 0) settings::setSince(timekeeper::toEpoch(0));
}

void printLog() {
  Serial.printf("[log] %u outages, newest first\n", static_cast<unsigned>(storage::count()));
  for (size_t i = 0; i < storage::count(); i++) {
    const storage::OutageRecord &r = storage::get(i);
    char stamp[16], dur[16];
    timekeeper::formatStamp(r.start, stamp, sizeof(stamp));
    timekeeper::formatDuration(r.duration, dur, sizeof(dur));
    Serial.printf("  %s  %-8s %s\n", stamp, dur, r.cause == 0 ? "ISP" : "WIFI");
  }
}

void printStats() {
  stats::Stats s = stats::compute();
  char longest[16], watching[16];
  timekeeper::formatDuration(s.longestSec, longest, sizeof(longest));
  timekeeper::formatDuration(s.watchingSec, watching, sizeof(watching));
  Serial.printf("[stats] 24h=%.2f%% 7d=%.2f%% isp_outages=%u longest=%s watching=%s\n", s.pct24h,
                s.pct7d, s.ispOutages, longest, watching);
}

void runCommand(const String &cmd) {
  if (cmd == "sim isp") {
    ping::setSim(ping::Sim::ISP);
    simName = " (simulated)";
  } else if (cmd == "sim wifi") {
    ping::setSim(ping::Sim::WIFI);
    simName = " (simulated)";
  } else if (cmd == "sim off") {
    ping::setSim(ping::Sim::NONE);
    simName = "";
  } else if (cmd == "log") {
    printLog();
    return;
  } else if (cmd == "stats") {
    printStats();
    return;
  } else if (cmd == "clearlog") {
    storage::clear();
  } else {
    Serial.println("[cmd] commands: sim isp | sim wifi | sim off | log | stats | clearlog");
    return;
  }
  Serial.printf("[cmd] ok: %s\n", cmd.c_str());
}

void serviceSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) runCommand(line);
      line = "";
    } else if (line.length() < 32) {
      line += c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.printf("\n[boot] uptime-monitor %s\n", VERSION);

  buttons::begin();
  display::begin();
  display::message("UPTIME MONITOR", "Starting...");
  settings::begin();
  storage::begin();

  if (!settings::hasWifi()) {
    Serial.println("[boot] no WiFi saved, starting setup portal");
    portal::run("", portalIdle);
  }

  const settings::Config &conf = settings::get();
  Serial.printf("[boot] network \"%s\", webhook %s\n", conf.ssid.c_str(),
                conf.hook.length() ? "set" : "not set");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep would add tens of ms to every ping
  WiFi.setAutoReconnect(true);
  WiFi.begin(conf.ssid.c_str(), conf.pass.length() ? conf.pass.c_str() : nullptr);
  timekeeper::begin();

  if (!conf.verified) {
    // Fresh credentials: prove them once, otherwise go back to setup so a
    // typo in the password cannot strand the device.
    char name[29];
    strlcpy(name, conf.ssid.c_str(), sizeof(name));
    display::message("UPTIME MONITOR", "Joining WiFi:", name);
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < cfg::FIRST_JOIN_TIMEOUT_MS) {
      buttons::poll();
      serviceWipe();
      delay(20);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[boot] could not join, returning to setup portal");
      WiFi.disconnect(true);
      portal::run("Couldn't join WiFi. Retry.", portalIdle);
    }
    settings::setVerified();
  }

  alerts::begin(conf.hook);
  if (conf.hook.length() && !conf.hookAnnounced) alerts::announceOnline();

  monitor::Handlers handlers = {onOutageStart, onOutageEnd};
  monitor::begin(handlers);
  ping::begin();
  screens::show();
}

void loop() {
  buttons::Event ev = buttons::poll();
  if (serviceWipe()) screens::show();
  if (!display::messageShowing()) {
    if (ev == buttons::BUTTON1) screens::next();
    else if (ev == buttons::BUTTON2) screens::action();
  }

  bool fresh = false;
  ping::Round round;
  while (ping::nextRound(round)) {
    monitor::onRound(round);
    printRound(round);
    fresh = true;
  }

  storage::loop();
  serviceWifi();
  serviceClock();
  serviceSerial();

  static uint32_t lastDraw = 0;
  if (fresh || millis() - lastDraw >= REDRAW_MS) {
    lastDraw = millis();
    screens::update();
  }
  delay(5);
}
