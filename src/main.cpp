// Uptime Sentinel: internet uptime monitor for ESP32 boards (see platformio.ini for targets).
// Wires the modules together; the logic lives in the files named after it.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "alerts.h"
#include "buttons.h"
#include "config.h"
#include "display.h"
#include "monitor.h"
#include "net.h"
#include "ping.h"
#include "portal.h"
#include "screens.h"
#include "settings.h"
#include "stats.h"
#include "storage.h"
#include "timekeeper.h"
#include "web.h"

namespace {

constexpr uint32_t REDRAW_MS = 250;
constexpr uint32_t ETH_WAIT_MS = 10000;  // a cable gets this long to come up before setup starts

uint32_t ispDownCarry = 0;  // ISP downtime in the current run of back-to-back outages
const char *simName = "";

// Wipe gesture held (both buttons, or the only button): show a countdown and
// wipe settings at five seconds. Returns true when the countdown was
// abandoned and the screen needs redrawing.
bool serviceWipe() {
  static bool overlay = false;
  uint32_t held = buttons::wipeHeldMs();
  if (held >= cfg::WIPE_HOLD_MS) {
    display::message("RESET", "Settings wiped.", "Restarting...");
    settings::wipe();
    WiFi.disconnect(true, true);
    Serial.println("[boot] settings wiped by button hold");
    delay(1200);
    ESP.restart();
  }
  if (held >= buttons::wipeWarnMs()) {
    char line[28];
    snprintf(line, sizeof(line), "Keep holding: %us",
             static_cast<unsigned>((cfg::WIPE_HOLD_MS - held + 999) / 1000));
    display::message("RESET", "Wipe all settings?", line, "Release to cancel");
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
  // A cable plugged in while the setup network is up makes setup unnecessary.
  if (net::HAS_ETHERNET && net::ethernetReady()) {
    Serial.println("[boot] Ethernet is up, leaving setup");
    delay(100);
    ESP.restart();
  }
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
  const char *const NAMES[ping::TARGETS] = {"router", settings::get().target[0].c_str(),
                                            settings::get().target[1].c_str()};
  Serial.print("[ping]");
  for (int i = 0; i < ping::TARGETS; i++) {
    if (r.rtt[i] >= 0) Serial.printf(" %s=%dms", NAMES[i], r.rtt[i]);
    else Serial.printf(" %s=--", NAMES[i]);
  }
  if (r.dns >= 0) Serial.printf(" dns=%s", r.dns ? "ok" : "FAIL");
  const monitor::State &m = monitor::state();
  Cause cause;
  uint32_t startMono;
  bool active = monitor::activeEvent(cause, startMono);
  // The cause is only worth adding for an outage; SLOW and NO DNS say it already.
  bool outage = active && m.outage;
  Serial.printf(" -> %s%s | %s%s%s%s\n", monitor::className(m.lastClass), simName,
                monitor::stateName(), outage ? " (" : "", outage ? monitor::causeName(cause) : "",
                outage ? ")" : "");
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
    Serial.printf("  %s  %-8s %s\n", stamp, dur, monitor::causeName(static_cast<Cause>(r.cause)));
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
  } else if (cmd == "sim slow") {
    ping::setSim(ping::Sim::SLOW);
    simName = " (simulated)";
  } else if (cmd == "sim dns") {
    ping::setSim(ping::Sim::DNS);
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
    Serial.println("[cmd] commands: sim isp | sim wifi | sim slow | sim dns | sim off | log | stats | clearlog");
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
  const esp_partition_t *slot = esp_ota_get_running_partition();
  Serial.printf("\n[boot] uptime-sentinel %s (%s, slot %s)\n", cfg::VERSION, cfg::BOARD,
                slot ? slot->label : "?");

  buttons::begin();
  display::begin();
  display::message("UPTIME SENTINEL", "Starting...");
  settings::begin();
  storage::begin();

  // Boards with a wired port can run on the cable alone; everything else
  // needs WiFi details before it can do anything.
  net::beginEthernet();
  if (!settings::hasWifi() && !net::waitEthernet(ETH_WAIT_MS)) {
    Serial.println("[boot] no WiFi saved, starting setup portal");
    portal::run("", portalIdle);
  }

  const settings::Config &conf = settings::get();
  Serial.printf("[boot] network \"%s\", webhook %s\n", conf.ssid.c_str(),
                conf.hook.length() ? "set" : "not set");

  net::beginWifi(conf.ssid, conf.pass);
  timekeeper::begin();

  if (settings::hasWifi() && !conf.verified) {
    // Fresh credentials: prove them once, otherwise go back to setup so a
    // typo in the password cannot strand the device.
    char name[29];
    strlcpy(name, conf.ssid.c_str(), sizeof(name));
    display::message("UPTIME SENTINEL", "Joining WiFi:", name);
    uint32_t started = millis();
    while (!net::up() && millis() - started < cfg::FIRST_JOIN_TIMEOUT_MS) {
      buttons::poll();
      serviceWipe();
      delay(20);
    }
    if (!net::up()) {
      Serial.println("[boot] could not join, returning to setup portal");
      WiFi.disconnect(true);
      portal::run("Couldn't join WiFi. Retry.", portalIdle);
    }
    // On a cable the WiFi details stay unproven until they are first needed.
    if (net::wifiUp()) settings::setVerified();
  }

  alerts::begin(conf.hook);
  if (conf.hook.length() && !conf.hookAnnounced) alerts::announceOnline();

  monitor::Handlers handlers = {onOutageStart, onOutageEnd};
  monitor::begin(handlers);
  ping::begin();
  web::begin();
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
    // The link takes a few seconds to come up after power-on; do not count that as
    // an outage. If it still has not joined after 20 seconds, it is one.
    static bool everLinked = false;
    everLinked = everLinked || round.linkUp;
    if (!everLinked && round.monoSec < 20) continue;
    monitor::onRound(round);
    printRound(round);
    fresh = true;
  }

  storage::loop();
  net::loop();
  serviceClock();
  serviceSerial();
  web::loop();

  static uint32_t lastDraw = 0;
  if (fresh || millis() - lastDraw >= REDRAW_MS) {
    lastDraw = millis();
    screens::update();
  }
  delay(5);
}
