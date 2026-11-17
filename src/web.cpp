#include "web.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <math.h>

#include "config.h"
#include "html.h"
#include "logview.h"
#include "monitor.h"
#include "settings.h"
#include "stats.h"
#include "timekeeper.h"

namespace web {
namespace {

WebServer server(80);
bool mdnsStarted = false;
bool restartPending = false;
uint32_t restartAt = 0;

// One admin session at a time, held in RAM: a restart logs everyone out.
constexpr uint32_t SESSION_IDLE_MS = 30UL * 60UL * 1000UL;
constexpr char COOKIE_NAME[] = "sid";
String sessionToken;
uint32_t sessionSeenAt = 0;
uint8_t loginFails = 0;
uint32_t loginLockedUntil = 0;

const char DASHBOARD[] PROGMEM = R"HTML(
<div id="state" class="big m">...</div><div id="sub" class="m" style="font-size:13px">&nbsp;</div>
<div class="grid">
<div class="cell">LATENCY<b id="lat">--</b></div>
<div class="cell">LOSS 5 MIN<b id="loss">--</b></div>
<div class="cell">JITTER<b id="jit">--</b></div>
</div>
<canvas id="g" width="480" height="90"></canvas>
<div class="grid" id="tg"></div>
<h2>UPTIME (ISP OUTAGES ONLY)</h2>
<div class="grid">
<div class="cell">24 HOURS<b id="p24">--</b></div>
<div class="cell">7 DAYS<b id="p7">--</b></div>
<div class="cell">WATCHING<b id="watch">--</b></div>
<div class="cell">ISP OUTAGES<b id="isp">--</b></div>
<div class="cell">LONGEST<b id="long">--</b></div>
<div class="cell">DNS<b id="dns">--</b></div>
</div>
<h2>OUTAGE LOG</h2>
<table><thead><tr><th>Started</th><th>Lasted</th><th>Cause</th></tr></thead>
<tbody id="log"></tbody></table>
<p><a href="/log.csv">Download log (CSV)</a> &middot; <a href="/settings">Settings</a></p>
<p id="foot"></p>
<script>
const $=i=>document.getElementById(i);
const cls={ISP:'o',SLOW:'y',DNS:'y',WIFI:'m',POWER:'m'};
const txt=(el,v)=>{el.textContent=v};
const ms=v=>v<0?'--':v+' ms';
function draw(h){const c=$('g'),x=c.getContext('2d'),W=c.width,H=c.height;x.clearRect(0,0,W,H);
let m=20;h.forEach(v=>{if(v>m)m=v});const n=150,bw=W/n;
h.forEach((v,i)=>{const px=(n-h.length+i)*bw;
if(v<0){x.fillStyle='#FF8A3D';x.fillRect(px,0,bw-1,H)}
else{const y=Math.max(1,v/m*H);x.fillStyle='#3DDC84';x.fillRect(px,H-y,bw-1,y)}})}
async function status(){try{const s=await(await fetch('/api/status')).json();
const st=$('state');txt(st,s.state);
st.className='big '+(s.state=='ONLINE'?'g':s.state=='OFFLINE'?'o':s.state=='...'?'m':'y');
txt($('sub'),s.detail||' ');
txt($('lat'),ms(s.latency));txt($('loss'),s.loss+'%');txt($('jit'),ms(s.jitter));
txt($('p24'),s.pct24);txt($('p7'),s.pct7);txt($('watch'),s.watching);
txt($('isp'),s.isp);txt($('long'),s.longest);txt($('dns'),s.dns);
const tg=$('tg');tg.textContent='';
s.targets.forEach(t=>{const d=document.createElement('div');d.className='cell';
d.textContent=t.name;const b=document.createElement('b');b.textContent=ms(t.ms);
if(t.ms<0)b.className='o';d.appendChild(b);tg.appendChild(d)});
draw(s.history);
txt($('foot'),'v'+s.version+' · '+s.time+' · WiFi '+s.rssi+' dBm');
}catch(e){txt($('sub'),'Cannot reach the monitor')}}
async function log(){try{const r=await(await fetch('/api/log')).json();
const b=$('log');b.textContent='';
if(!r.length){const td=b.insertRow().insertCell();td.colSpan=3;td.className='m';
td.textContent='No outages logged yet'}
r.forEach(e=>{const tr=b.insertRow();txt(tr.insertCell(),e.start);
const d=tr.insertCell();txt(d,e.duration+(e.active?' (ongoing)':''));
const c=tr.insertCell();txt(c,e.cause);c.className=cls[e.cause]||'m';
if(e.active)d.className=cls[e.cause]||'m'})}catch(e){}}
status();log();setInterval(status,2000);setInterval(log,10000);
</script>)HTML";

// Only answer requests addressed to this device by its own name or IP, and
// only accept form posts that came from our own pages. Stops other websites
// from reaching the monitor through the visitor's browser.
bool allowed(bool isPost) {
  String host = server.hostHeader();
  host.toLowerCase();
  String mdns = String(cfg::MDNS_NAME) + ".local";
  bool hostOk = host == WiFi.localIP().toString() || host == mdns;
  bool originOk = true;
  if (isPost) {
    String origin = server.header("Origin");
    String referer = server.header("Referer");
    if (origin.length()) originOk = origin == "http://" + host;
    else if (referer.length()) originOk = referer.startsWith("http://" + host + "/");
  }
  if (hostOk && originOk) return true;
  server.send(403, "text/plain", "Forbidden");
  return false;
}

// Compares all of both strings, so timing does not reveal how much matched.
bool sameText(const String &a, const String &b) {
  uint8_t diff = a.length() == b.length() ? 0 : 1;
  for (size_t i = 0; i < a.length() && i < b.length(); i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

String cookie(const char *name) {
  String all = server.header("Cookie");
  String key = String(name) + "=";
  int from = 0;
  while (from < static_cast<int>(all.length())) {
    int end = all.indexOf(';', from);
    if (end < 0) end = all.length();
    String part = all.substring(from, end);
    part.trim();
    if (part.startsWith(key)) return part.substring(key.length());
    from = end + 1;
  }
  return "";
}

bool sessionValid() {
  if (sessionToken.length() == 0) return false;
  if (millis() - sessionSeenAt > SESSION_IDLE_MS) {
    sessionToken = "";
    return false;
  }
  if (!sameText(cookie(COOKIE_NAME), sessionToken)) return false;
  sessionSeenAt = millis();
  return true;
}

// The pages a login may lead back to. Anything else falls back to Settings.
const char *safeNext(const String &next) { return next == "/update" ? "/update" : "/settings"; }

void redirect(const String &where) {
  server.sendHeader("Location", where, true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(303, "text/plain", "");
}

// True when the request may use an admin page. With no admin password set
// the pages are open, as they were before passwords existed. Otherwise the
// visitor is sent to the login page and this returns false.
bool requireAdmin(const char *page) {
  if (!settings::hasAdmin() || sessionValid()) return true;
  redirect(String("/login?next=") + page);
  return false;
}

String pct(float value) {
  if (isnan(value)) return "--";
  if (value >= 99.995f) return "100%";
  char text[12];
  snprintf(text, sizeof(text), "%.2f%%", value);
  return text;
}

String duration(uint32_t seconds) {
  char text[16];
  timekeeper::formatDuration(seconds, text, sizeof(text));
  return text;
}

void handleRoot() {
  if (!allowed(false)) return;
  String page = html::pageStart("Uptime Sentinel");
  page += FPSTR(DASHBOARD);
  page += FPSTR(html::PAGE_END);
  server.send(200, "text/html", page);
}

void handleStatus() {
  if (!allowed(false)) return;
  const monitor::State &m = monitor::state();
  monitor::Summary sum = monitor::summarize(cfg::ROUNDS_5MIN);
  stats::Stats s = stats::compute();

  String detail;
  Cause cause;
  uint32_t startMono;
  if (monitor::activeEvent(cause, startMono)) {
    String lasted = duration(timekeeper::mono() - startMono);
    if (cause == Cause::ISP) detail = "ISP outage for " + lasted;
    else if (cause == Cause::WIFI) detail = "No reply from the router for " + lasted;
    else if (cause == Cause::DNS) detail = "DNS lookups failing for " + lasted;
    else detail = "Slow or losing packets for " + lasted;
  }

  char clock[16] = "clock not set";
  if (timekeeper::valid()) timekeeper::formatStamp(timekeeper::now(), clock, sizeof(clock));

  String json;
  json.reserve(1400);
  json += "{\"state\":\"";
  json += monitor::stateName();
  json += "\",\"detail\":\"" + detail + "\"";
  json += ",\"latency\":" + String(m.haveData ? m.internetMs : -1);
  json += ",\"loss\":" + String(sum.lossPct);
  json += ",\"jitter\":" + String(sum.jitterMs);
  json += ",\"baseline\":" + String(m.baselineMs);
  json += ",\"dns\":\"";
  json += m.dnsOk < 0 ? "--" : (m.dnsOk ? "OK" : "FAILING");
  json += "\",\"targets\":[";
  const String names[3] = {"ROUTER", settings::get().target[0], settings::get().target[1]};
  for (int i = 0; i < 3; i++) {
    if (i) json += ",";
    json += "{\"name\":\"" + names[i] + "\",\"ms\":" + String(m.rtt[i]) + "}";
  }
  json += "],\"pct24\":\"" + pct(s.pct24h) + "\",\"pct7\":\"" + pct(s.pct7d) + "\"";
  json += ",\"isp\":" + String(s.ispOutages);
  json += ",\"longest\":\"" + (s.longestSec ? duration(s.longestSec) : String("--")) + "\"";
  json += ",\"watching\":\"" + duration(s.watchingSec) + "\"";
  json += ",\"time\":\"" + String(clock) + "\"";
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"version\":\"";
  json += cfg::VERSION;
  json += "\",\"history\":[";
  size_t n = monitor::historyCount() < cfg::ROUNDS_5MIN ? monitor::historyCount()
                                                         : cfg::ROUNDS_5MIN;
  for (size_t i = 0; i < n; i++) {
    uint16_t v = monitor::historyAt(n - 1 - i);
    if (i) json += ",";
    json += v == monitor::HIST_FAIL ? String(-1) : String(v);
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleLog() {
  if (!allowed(false)) return;
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk = "[";
  size_t total = logview::count();
  for (size_t i = 0; i < total; i++) {
    logview::Row r = logview::get(i);
    char stamp[16];
    if (r.active && r.start == 0) strlcpy(stamp, "now", sizeof(stamp));
    else timekeeper::formatStamp(r.start, stamp, sizeof(stamp));
    if (i) chunk += ",";
    chunk += "{\"start\":\"";
    chunk += stamp;
    chunk += "\",\"duration\":\"" + duration(r.duration) + "\",\"cause\":\"";
    chunk += monitor::causeName(r.cause);
    chunk += r.active ? "\",\"active\":true}" : "\",\"active\":false}";
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += "]";
  server.sendContent(chunk);
  server.sendContent("");
}

void handleCsv() {
  if (!allowed(false)) return;
  server.sendHeader("Content-Disposition", "attachment; filename=\"uptime-sentinel-log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  String chunk = "started_local,started_unix,duration_seconds,cause,ongoing\r\n";
  size_t total = logview::count();
  for (size_t i = 0; i < total; i++) {
    logview::Row r = logview::get(i);
    char stamp[24] = "";
    if (r.start != 0) {
      time_t t = r.start;
      struct tm tm;
      localtime_r(&t, &tm);
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    }
    chunk += stamp;
    chunk += "," + String(r.start) + "," + String(r.duration) + ",";
    chunk += monitor::causeName(r.cause);
    chunk += r.active ? ",yes\r\n" : ",no\r\n";
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  if (chunk.length()) server.sendContent(chunk);
  server.sendContent("");
}

void sendLogin(const String &next, const String &notice) {
  String page = html::pageStart("Uptime Sentinel login");
  page += F("<p><a href=\"/\">&larr; Back to the dashboard</a></p>");
  if (notice.length()) page += "<p class=\"warn\">" + html::escape(notice) + "</p>";
  page += F("<form method=\"post\" action=\"/login\"><input type=\"hidden\" name=\"next\" "
            "value=\"");
  page += safeNext(next);
  page += F("\"><label for=\"password\">Admin password</label>"
            "<input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\" "
            "autocomplete=\"current-password\" autofocus>"
            "<button type=\"submit\">Log in</button></form>"
            "<p>Forgotten it? Hold the wipe gesture on the monitor for 5 seconds. That "
            "erases the password along with the other settings, and keeps the outage "
            "log.</p>");
  page += FPSTR(html::PAGE_END);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

void handleLoginGet() {
  if (!allowed(false)) return;
  String next = safeNext(server.arg("next"));
  if (!settings::hasAdmin() || sessionValid()) redirect(next);
  else sendLogin(next, "");
}

void handleLoginPost() {
  if (!allowed(true)) return;
  String next = safeNext(server.arg("next"));
  if (!settings::hasAdmin()) {
    redirect(next);
    return;
  }
  int32_t wait = static_cast<int32_t>(loginLockedUntil - millis());
  if (loginFails && wait > 0) {
    int seconds = wait / 1000 + 1;
    sendLogin(next, "Too many wrong passwords. Wait " + String(seconds) +
                        (seconds == 1 ? " second" : " seconds") + " and try again.");
    return;
  }
  if (!settings::checkAdminPassword(server.arg("password"))) {
    // Each wrong guess doubles the wait before the next one, up to a minute.
    if (loginFails < 6) loginFails++;
    loginLockedUntil = millis() + (1000UL << loginFails);
    Serial.println("[web] wrong admin password");
    sendLogin(next, "Wrong password.");
    return;
  }
  loginFails = 0;
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  char hex[33];
  for (size_t i = 0; i < sizeof(raw); i++) snprintf(hex + i * 2, 3, "%02x", raw[i]);
  sessionToken = hex;
  sessionSeenAt = millis();
  server.sendHeader("Set-Cookie", String(COOKIE_NAME) + "=" + sessionToken +
                                      "; Path=/; HttpOnly; SameSite=Strict");
  Serial.println("[web] admin logged in");
  redirect(next);
}

void handleLogout() {
  if (!allowed(false)) return;
  if (sessionValid()) sessionToken = "";
  server.sendHeader("Set-Cookie", String(COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly");
  redirect("/");
}

void sendSettings(const String &notice) {
  const settings::Config &conf = settings::get();
  String page = html::pageStart("Uptime Sentinel settings");
  page += F("<p><a href=\"/\">&larr; Back to the dashboard</a></p>");
  if (notice.length()) page += "<p class=\"warn\">" + html::escape(notice) + "</p>";
  page += F("<form method=\"post\" action=\"/settings\"><h2>DISCORD</h2>"
            "<label for=\"hook\">Webhook URL</label>"
            "<input id=\"hook\" name=\"hook\" type=\"url\" maxlength=\"240\" "
            "placeholder=\"https://discord.com/api/webhooks/...\">");
  if (conf.hook.length()) {
    page += F("<p>A webhook is saved (it is not shown here). Leave the box empty to keep "
              "it, or paste a new one to replace it.</p><label class=\"check\">"
              "<input type=\"checkbox\" name=\"clearhook\" value=\"1\"> "
              "Remove the saved webhook</label>");
  } else {
    page += F("<p>No webhook saved. Paste one to get a message when an ISP outage ends.</p>");
  }
  page += F("<h2>TIME AND TARGETS</h2>");
  page += html::optionFields();
  page += F("<h2>ADMIN PASSWORD</h2>");
  page += html::adminFields();
  page += F("<button type=\"submit\">Save and restart</button></form><h2>WIFI</h2><p>Connected to <b>");
  page += html::escape(conf.ssid);
  page += F("</b>. To move the monitor to a different network, hold its wipe gesture "
            "for 5 seconds and run the setup again.</p>");
  if (settings::hasAdmin()) page += F("<p><a href=\"/logout\">Log out</a></p>");
  page += FPSTR(html::PAGE_END);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

void handleSettingsGet() {
  if (!allowed(false) || !requireAdmin("/settings")) return;
  sendSettings("");
}

void handleSettingsPost() {
  if (!allowed(true) || !requireAdmin("/settings")) return;
  String hook = server.arg("hook");
  hook.trim();
  String tz, target1, target2, admin, error;
  bool adminChange = false;
  if (!settings::validHook(hook)) {
    error = "The webhook URL must start with https://";
  } else if (html::readOptions(server, tz, target1, target2, error)) {
    html::readAdmin(server, admin, adminChange, error);
  }
  if (error.length()) {
    sendSettings(error);
    return;
  }

  settings::saveOptions(tz, target1, target2);
  if (server.arg("clearhook") == "1" && hook.length() == 0) settings::saveHook("");
  else if (hook.length()) settings::saveHook(hook);
  if (adminChange) settings::setAdminPassword(admin);
  Serial.printf("[web] settings saved, webhook %s, admin password %s; restarting\n",
                settings::get().hook.length() ? "set" : "not set",
                settings::hasAdmin() ? "set" : "not set");

  String page = html::pageStart("Uptime Sentinel settings");
  page += F("<p>Saved. The monitor is restarting; the dashboard will be back in about "
            "15 seconds.</p><p><a href=\"/\">Back to the dashboard</a></p>");
  page += FPSTR(html::PAGE_END);
  server.send(200, "text/html", page);
  restartPending = true;
  restartAt = millis() + 1500;
}

}  // namespace

void begin() {
  static const char *HEADERS[] = {"Origin", "Referer", "Cookie"};
  server.collectHeaders(HEADERS, 3);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/log.csv", HTTP_GET, handleCsv);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/login", HTTP_GET, handleLoginGet);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  // mDNS needs an IP address, so start it on the first pass with WiFi up.
  if (!mdnsStarted && WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(cfg::MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.printf("[web] dashboard at http://%s.local or http://%s\n", cfg::MDNS_NAME,
                    WiFi.localIP().toString().c_str());
    }
  }
  server.handleClient();
  if (restartPending && static_cast<int32_t>(millis() - restartAt) >= 0) {
    delay(100);
    ESP.restart();
  }
}

}  // namespace web
