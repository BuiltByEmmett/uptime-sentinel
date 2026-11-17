#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "display.h"
#include "html.h"
#include "settings.h"

namespace portal {
namespace {

WebServer server(80);
DNSServer dns;
String networkOptions;
String notice;
bool restartPending = false;
uint32_t restartAt = 0;

// Strongest first, each network name listed once.
void scanNetworks() {
  networkOptions = "";
  int found = WiFi.scanNetworks();
  for (int i = 0; i < found; i++) {
    String name = WiFi.SSID(i);
    if (name.length() == 0) continue;
    bool duplicate = false;
    for (int j = 0; j < i; j++) {
      if (WiFi.SSID(j) == name) duplicate = true;
    }
    if (duplicate) continue;
    String safe = html::escape(name);
    networkOptions += "<option value=\"" + safe + "\">" + safe + " (" + WiFi.RSSI(i) +
                      " dBm)</option>";
  }
  WiFi.scanDelete();
}

void sendForm() {
  String page = html::pageStart("Uptime Sentinel setup");
  page += F("<p>Connect the monitor to your 2.4 GHz WiFi.</p>");
  if (notice.length()) page += "<p class=\"warn\">" + html::escape(notice) + "</p>";
  page += F("<form method=\"post\" action=\"/save\">"
            "<label for=\"ssid\">WiFi network</label><select id=\"ssid\" name=\"ssid\">");
  page += networkOptions;
  page += F("<option value=\"\">Other (type below)</option></select>"
            "<label for=\"manual\">Network name, if not listed</label>"
            "<input type=\"text\" id=\"manual\" name=\"manual\" maxlength=\"32\" "
            "autocapitalize=\"none\">"
            "<label for=\"pass\">WiFi password</label>"
            "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"64\">"
            "<label for=\"hook\">Discord webhook URL (optional)</label>"
            "<input id=\"hook\" name=\"hook\" type=\"url\" maxlength=\"240\" "
            "placeholder=\"https://discord.com/api/webhooks/...\">");
  if (settings::get().hook.length()) {
    page += F("<p>A webhook is already saved. Leave this empty to keep it.</p>");
  }
  page += html::optionFields();
  page += F("<h2>ADMIN PASSWORD</h2>");
  page += html::adminFields();
  page += F("<button type=\"submit\">Save and connect</button></form>"
            "<p><a href=\"/rescan\">Scan for networks again</a></p>");
  page += FPSTR(html::PAGE_END);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

void redirectHome() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

void handleSave() {
  String ssid = server.arg("manual");
  ssid.trim();
  if (ssid.length() == 0) ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String hook = server.arg("hook");
  hook.trim();

  String tz, target1, target2, admin, error;
  bool adminChange = false;
  if (ssid.length() == 0 || ssid.length() > 32) {
    error = "Pick a network or type its name.";
  } else if (!settings::validHook(hook)) {
    error = "The webhook URL must start with https://";
  } else if (html::readOptions(server, tz, target1, target2, error)) {
    html::readAdmin(server, admin, adminChange, error);
  }
  if (error.length()) {
    notice = error;
    redirectHome();
    return;
  }

  settings::saveWifi(ssid, pass);
  if (hook.length()) settings::saveHook(hook);
  settings::saveOptions(tz, target1, target2);
  if (adminChange) settings::setAdminPassword(admin);
  Serial.printf("[setup] saved network \"%s\", webhook %s, admin password %s\n", ssid.c_str(),
                settings::get().hook.length() ? "set" : "not set",
                settings::hasAdmin() ? "set" : "not set");

  String page = html::pageStart("Uptime Sentinel setup");
  page += "<p>Saved. The monitor is restarting and will join <b>" + html::escape(ssid) +
          "</b>.</p><p>If the password is wrong, this setup network comes back after "
          "about 30 seconds.</p><p>Once it is running, open <b>http://";
  page += cfg::MDNS_NAME;
  page += ".local</b> from a device on your WiFi to see the dashboard.</p>";
  page += FPSTR(html::PAGE_END);
  server.send(200, "text/html", page);
  restartPending = true;
  restartAt = millis() + 1500;
}

}  // namespace

void run(const char *initialNotice, bool (*idle)()) {
  notice = initialNotice;

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(cfg::AP_NAME);
  delay(200);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[setup] access point \"%s\" up at %s\n", cfg::AP_NAME, ip.toString().c_str());

  display::message("SETUP", "Join this WiFi network:", cfg::AP_NAME, "Scanning...",
                   initialNotice);
  scanNetworks();
  String hint = "then open " + ip.toString();
  display::message("SETUP", "Join this WiFi network:", cfg::AP_NAME, hint.c_str(),
                   initialNotice);

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", ip);  // every hostname resolves to us, which triggers the captive page

  server.on("/", HTTP_GET, sendForm);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", HTTP_GET, []() {
    scanNetworks();
    redirectHome();
  });
  server.onNotFound(redirectHome);
  server.begin();

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    if (idle && idle()) {
      display::message("SETUP", "Join this WiFi network:", cfg::AP_NAME, hint.c_str(),
                       initialNotice);
    }
    if (restartPending && static_cast<int32_t>(millis() - restartAt) >= 0) {
      Serial.println("[setup] restarting");
      delay(100);
      ESP.restart();
    }
    delay(2);
  }
}

}  // namespace portal
