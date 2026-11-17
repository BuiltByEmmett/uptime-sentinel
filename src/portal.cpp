#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "display.h"
#include "settings.h"

namespace portal {
namespace {

WebServer server(80);
DNSServer dns;
String networkOptions;
String notice;
bool restartPending = false;
uint32_t restartAt = 0;

const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Uptime Monitor setup</title><style>
body{background:#0B0E11;color:#E8EDF2;font-family:ui-monospace,Menlo,Consolas,monospace;
margin:0 auto;padding:24px;max-width:420px}
h1{font-size:18px;color:#3DDC84;margin:0 0 4px}
p{color:#9AA4AF;font-size:13px;line-height:1.5}
label{display:block;color:#9AA4AF;font-size:13px;margin:18px 0 6px}
select,input{width:100%;box-sizing:border-box;padding:12px;background:#1C232B;color:#E8EDF2;
border:1px solid #2A333D;border-radius:6px;font:inherit;font-size:16px}
button{margin-top:24px;width:100%;padding:14px;background:#3DDC84;color:#0B0E11;border:0;
border-radius:6px;font:inherit;font-size:16px;font-weight:bold}
.warn{color:#FF8A3D}a{color:#9AA4AF}
</style></head><body><h1>Uptime Monitor</h1>)HTML";

const char PAGE_TAIL[] PROGMEM = "</body></html>";

String escapeHtml(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

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
    String safe = escapeHtml(name);
    networkOptions += "<option value=\"" + safe + "\">" + safe + " (" + WiFi.RSSI(i) +
                      " dBm)</option>";
  }
  WiFi.scanDelete();
}

void sendForm() {
  String html = FPSTR(PAGE_HEAD);
  html += F("<p>Connect the monitor to your 2.4 GHz WiFi.</p>");
  if (notice.length()) html += "<p class=\"warn\">" + escapeHtml(notice) + "</p>";
  html += F("<form method=\"post\" action=\"/save\">"
            "<label for=\"ssid\">WiFi network</label><select id=\"ssid\" name=\"ssid\">");
  html += networkOptions;
  html += F("<option value=\"\">Other (type below)</option></select>"
            "<label for=\"manual\">Network name, if not listed</label>"
            "<input id=\"manual\" name=\"manual\" maxlength=\"32\" autocapitalize=\"none\">"
            "<label for=\"pass\">WiFi password</label>"
            "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"64\">"
            "<label for=\"hook\">Discord webhook URL (optional)</label>"
            "<input id=\"hook\" name=\"hook\" type=\"url\" maxlength=\"240\" "
            "placeholder=\"https://discord.com/api/webhooks/...\">");
  if (settings::get().hook.length()) {
    html += F("<p>A webhook is already saved. Leave this empty to keep it.</p>");
  }
  html += F("<button type=\"submit\">Save and connect</button></form>"
            "<p><a href=\"/rescan\">Scan for networks again</a></p>");
  html += FPSTR(PAGE_TAIL);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
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

  if (ssid.length() == 0 || ssid.length() > 32) {
    notice = "Pick a network or type its name.";
    redirectHome();
    return;
  }
  if (hook.length() && !hook.startsWith("https://")) {
    notice = "The webhook URL must start with https://";
    redirectHome();
    return;
  }

  settings::saveSetup(ssid, pass, hook);
  Serial.printf("[setup] saved network \"%s\", webhook %s\n", ssid.c_str(),
                settings::get().hook.length() ? "set" : "not set");

  String html = FPSTR(PAGE_HEAD);
  html += "<p>Saved. The monitor is restarting and will join <b>" + escapeHtml(ssid) +
          "</b>.</p><p>If the password is wrong, this setup network comes back after "
          "about 30 seconds.</p>";
  html += FPSTR(PAGE_TAIL);
  server.send(200, "text/html", html);
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
