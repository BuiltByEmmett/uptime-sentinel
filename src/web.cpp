#include "web.h"

#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <math.h>

#include "alerts.h"
#include "config.h"
#include "daily.h"
#include "devices.h"
#include "display.h"
#include "html.h"
#include "logview.h"
#include "metrics.h"
#include "monitor.h"
#include "net.h"
#include "pubip.h"
#include "report.h"
#include "screens.h"
#include "settings.h"
#include "sites.h"
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

// A firmware upload in progress. The image is written to the spare app slot
// as it arrives; the slot only becomes the one to boot from after every
// check has passed, so a refused or interrupted upload changes nothing.
constexpr size_t IMAGE_HEAD_LEN = 36;         // image header, first segment header, magic word
constexpr size_t IMAGE_CHIP_OFFSET = 12;      // esp_image_header_t::chip_id
constexpr size_t IMAGE_DESC_OFFSET = 32;      // esp_app_desc_t::magic_word
constexpr size_t MARKER_LEN = sizeof(cfg::BOARD_MARKER) - 1;
struct Upload {
  bool started = false;
  bool permitted = false;
  bool failed = false;
  bool markerFound = false;
  String error;
  uint8_t head[IMAGE_HEAD_LEN];
  size_t headLen = 0;
  uint8_t tail[MARKER_LEN];  // end of the previous chunk, for a marker split across two
  size_t tailLen = 0;
  size_t received = 0;
  size_t shownKb = 0;
};
Upload up;

const char DASHBOARD[] PROGMEM = R"HTML(
<div id="state" class="big m">...</div><div id="sub" class="m" style="font-size:13px">&nbsp;</div>
<div class="grid">
<div class="cell">LATENCY<b id="lat">--</b></div>
<div class="cell">LOSS 5 MIN<b id="loss">--</b></div>
<div class="cell">JITTER<b id="jit">--</b></div>
</div>
<canvas id="g" width="480" height="90"></canvas>
<div class="grid" id="tg"></div>
<p id="hop"></p>
<h2>UPTIME (ISP OUTAGES ONLY)</h2>
<div class="grid">
<div class="cell">24 HOURS<b id="p24">--</b></div>
<div class="cell">7 DAYS<b id="p7">--</b></div>
<div class="cell">WATCHING<b id="watch">--</b></div>
<div class="cell">ISP OUTAGES<b id="isp">--</b></div>
<div class="cell">LONGEST<b id="long">--</b></div>
<div class="cell">DNS<b id="dns">--</b></div>
</div>
<h2>LAST 90 DAYS (ISP DOWNTIME PER DAY)</h2>
<style>
#cal{display:grid;grid-template-rows:repeat(7,auto);grid-auto-flow:column;grid-auto-columns:1fr;gap:3px;margin-top:10px}
.d{aspect-ratio:1;padding:0;margin:0;width:auto;border:1px solid #2A333D;border-radius:3px;background:#12171C;position:relative;display:block}
.d::after{content:'';position:absolute;inset:0;margin:auto;border-radius:2px}
.l1::after{width:25%;height:25%;background:#FFD23D}
.l2::after{width:50%;height:50%;background:#FFB03D}
.l3::after{width:75%;height:75%;background:#FF8A3D}
.l4::after{width:100%;height:100%;background:#FF5A3D}
.ln{border-style:dashed;opacity:.45}
.d:focus{outline:2px solid #E8EDF2}
.k{display:inline-block;width:13px;height:13px;vertical-align:-2px;margin:0 4px 0 10px}
</style>
<div id="cal"></div>
<p id="calt" aria-live="polite">Each column is a week, oldest on the left. Today is the last square. Point at or tap a day.</p>
<p style="margin-top:-6px"><span class="d k" style="margin-left:0"></span>none<span class="d l1 k"></span>under 1m<span class="d l2 k"></span>under 10m<span class="d l3 k"></span>under 1h<span class="d l4 k"></span>1h or more<span class="d ln k"></span>not watching</p>
<h2>OUTAGE LOG</h2>
<table><thead><tr><th>Started</th><th>Lasted</th><th>Cause</th></tr></thead>
<tbody id="log"></tbody></table>
<p><a href="/devices">Devices</a> &middot; <a href="/sites">Websites</a> &middot; <a href="/report">ISP report</a> &middot; <a href="/log.csv">Download log (CSV)</a> &middot; <a href="/ip">Public IP log</a>
&middot; <a href="/settings">Settings</a>
&middot; <a href="/update">Update firmware</a></p>
<p id="foot"></p>
<script>
const $=i=>document.getElementById(i);
const cls={ISP:'o','ISP LOCAL':'o','ISP UPSTREAM':'o',SLOW:'y',DNS:'y',WIFI:'m',LAN:'m',POWER:'m'};
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
txt($('hop'),s.hop);draw(s.history);
txt($('foot'),'v'+s.version+' · '+s.time+' · '+s.link);
}catch(e){txt($('sub'),'Cannot reach the monitor')}}
async function log(){try{const r=await(await fetch('/api/log')).json();
const b=$('log');b.textContent='';
if(!r.length){const td=b.insertRow().insertCell();td.colSpan=3;td.className='m';
td.textContent='No outages logged yet'}
r.forEach(e=>{const tr=b.insertRow();txt(tr.insertCell(),e.start);
const d=tr.insertCell();txt(d,e.duration+(e.active?' (ongoing)':''));
const c=tr.insertCell();txt(c,e.cause+(e.newip?' \u00b7 new IP':''));c.className=cls[e.cause]||'m';
if(e.active)d.className=cls[e.cause]||'m'})}catch(e){}}
const dur=s=>s<60?s+'s':s<3600?Math.floor(s/60)+'m '+s%60+'s':Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';
async function cal(){try{const r=await(await fetch('/api/days')).json();
const c=$('cal');c.textContent='';
if(!r.today){txt($('calt'),'The calendar starts once the clock has been set.');return}
const m={};r.days.forEach(d=>m[d[0]]=d);
const first=r.today-89,off=new Date(first*864e5).getUTCDay();
for(let i=0;i<off;i++)c.appendChild(document.createElement('i'));
for(let d=first;d<=r.today;d++){const e=m[d],b=document.createElement('button');b.type='button';
const date=new Date(d*864e5).toLocaleDateString(undefined,{timeZone:'UTC',weekday:'short',month:'short',day:'numeric'});
let lv,t;
if(!r.since||d<r.since){lv='n';t=date+': not watching yet'}
else if(!e){lv=0;t=date+': no ISP downtime'}
else{lv=e[1]<60?1:e[1]<600?2:e[1]<3600?3:4;
t=date+': down '+dur(e[1])+(e[2]?' in '+e[2]+' outage'+(e[2]==1?'':'s'):', carried over from the day before')}
b.className='d l'+lv;b.title=t;b.setAttribute('aria-label',t);
b.onclick=b.onfocus=b.onmouseenter=()=>txt($('calt'),t);c.appendChild(b)}
}catch(e){}}
status();log();cal();setInterval(status,2000);setInterval(log,10000);setInterval(cal,60000);
</script>)HTML";

// Only answer requests addressed to this device by its own name or IP, and
// only accept form posts that came from our own pages. Stops other websites
// from reaching the monitor through the visitor's browser.
bool requestOk(bool isPost) {
  String host = server.hostHeader();
  host.toLowerCase();
  // Scrapers such as Prometheus name the port even when it is the usual one.
  if (host.endsWith(":80")) host.remove(host.length() - 3);
  String mdns = String(cfg::MDNS_NAME) + ".local";
  bool hostOk = host == net::localIP().toString() || host == mdns;
  bool originOk = true;
  if (isPost) {
    String origin = server.header("Origin");
    String referer = server.header("Referer");
    if (origin.length()) originOk = origin == "http://" + host;
    else if (referer.length()) originOk = referer.startsWith("http://" + host + "/");
  }
  return hostOk && originOk;
}

bool allowed(bool isPost) {
  if (requestOk(isPost)) return true;
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
const char *safeNext(const String &next) {
  return next == "/update" ? "/update" : next == "/devices" ? "/devices" : "/settings";
}

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

// Which of the first-hop cases applies, for the dashboard.
String hopText() {
  ping::HopInfo hop = ping::hopInfo();
  String addr = IPAddress(hop.addr).toString();
  switch (hop.state) {
    case ping::HopState::ACTIVE:
      return "First ISP hop " + addr + " answers pings: ISP outages are logged as ISP LOCAL "
             "(that hop is down too) or ISP UPSTREAM (it still answers).";
    case ping::HopState::NO_REPLY:
      return "First ISP hop " + addr + " does not answer pings, so ISP outages are not split "
             "into local and upstream.";
    case ping::HopState::PRIVATE:
      return "The hop after the router, " + addr + ", has a private address (probably a "
             "second router), so ISP outages are not split into local and upstream.";
    case ping::HopState::NONE:
      return "No first ISP hop could be found, so ISP outages are not split into local and "
             "upstream.";
    default:
      return "Looking for the first ISP hop...";
  }
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
    else if (cause == Cause::ISP_LOCAL) detail = "ISP outage (local: first hop down) for " + lasted;
    else if (cause == Cause::ISP_UPSTREAM) detail = "ISP outage (upstream: first hop up) for " + lasted;
    else if (cause == Cause::WIFI || cause == Cause::LAN) detail = "No reply from the router for " + lasted;
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
  if (m.hopActive) json += ",{\"name\":\"ISP HOP\",\"ms\":" + String(m.rtt[ping::HOP]) + "}";
  json += "],\"hop\":\"" + hopText();
  json += "\",\"pct24\":\"" + pct(s.pct24h) + "\",\"pct7\":\"" + pct(s.pct7d) + "\"";
  json += ",\"isp\":" + String(s.ispOutages);
  json += ",\"longest\":\"" + (s.longestSec ? duration(s.longestSec) : String("--")) + "\"";
  json += ",\"watching\":\"" + duration(s.watchingSec) + "\"";
  json += ",\"time\":\"" + String(clock) + "\"";
  json += ",\"link\":\"" + net::linkText() + "\"";
  json += ",\"version\":\"";
  json += cfg::VERSION;
  json += "\",\"board\":\"";
  json += cfg::BOARD;
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
    chunk += r.active ? "\",\"active\":true" : "\",\"active\":false";
    chunk += r.newAddress ? ",\"newip\":true}" : "}";
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += "]";
  server.sendContent(chunk);
  server.sendContent("");
}

// The uptime calendar: [day, seconds down, outages] for each day that had
// ISP downtime. Days are counted from 1970-01-01 in the monitor's timezone.
void handleDays() {
  if (!allowed(false)) return;
  static daily::Entry days[daily::DAYS];
  bool clock = timekeeper::valid();
  size_t n = clock ? daily::collect(days, daily::DAYS) : 0;
  String json = "{\"today\":";
  json += clock ? daily::dayOf(timekeeper::now()) : 0;
  json += ",\"since\":";
  json += clock && settings::since() ? daily::dayOf(settings::since()) : 0;
  json += ",\"days\":[";
  for (size_t i = 0; i < n; i++) {
    if (i) json += ",";
    json += "[" + String(days[i].day) + "," + String(days[i].downSec) + "," +
            String(days[i].outages) + "]";
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCsv() {
  if (!allowed(false)) return;
  server.sendHeader("Content-Disposition", "attachment; filename=\"uptime-sentinel-log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  String chunk = "started_local,started_unix,duration_seconds,cause,ongoing,new_public_ip\r\n";
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
    chunk += r.active ? ",yes" : ",no";
    // Only whether the address changed; the address itself is never exported.
    chunk += r.newAddress ? ",yes\r\n" : ",no\r\n";
    if (chunk.length() > 1000) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  if (chunk.length()) server.sendContent(chunk);
  server.sendContent("");
}

void handleMetrics() {
  if (!allowed(false)) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; version=0.0.4; charset=utf-8", metrics::render());
}

void handleReport() {
  if (!allowed(false)) return;
  report::send(server);
}

String macText(const uint8_t mac[6], bool colons) {
  char text[18];
  snprintf(text, sizeof(text), colons ? "%02X:%02X:%02X:%02X:%02X:%02X" : "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return text;
}

// The device list. Everything on this page stays on the monitor.
void handleDevices() {
  if (!allowed(false)) return;
  devices::Status st = devices::status();
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  String page = html::pageStart("Uptime Sentinel devices");
  page += F("<style>details{margin:6px 0 0}summary{color:#9AA4AF;font-size:13px;cursor:pointer}"
            ".dv{border-bottom:1px solid #1C232B;padding:12px 0}.dv b{font-size:15px}"
            ".dv p{margin:3px 0}.tag{border:1px solid #2A333D;border-radius:4px;padding:0 5px;"
            "font-size:11px;margin-left:6px}</style>"
            "<p><a href=\"/\">&larr; Back to the dashboard</a></p>");
  if (server.arg("saved") == "1") page += F("<p class=\"warn\">Saved.</p>");
  page += F("<h2>THIS MONITOR</h2><p>");
  int dbm;
  if (net::wifiSignal(dbm)) {
    const char *words = dbm >= -55 ? "strong" : dbm >= -67 ? "good" : dbm >= -75 ? "fair" : "weak";
    page += "WiFi signal at the monitor: <b>" + String(dbm) + " dBm</b> (" + words + ").";
  } else {
    page += F("The monitor is connected by cable, so it has no WiFi signal to show.");
  }
  page += F(" Signal strength is not shown for the devices below: only your router or access "
            "point knows how well it hears each of them, and the monitor has no way to "
            "measure it.</p><h2>DEVICES ON THIS NETWORK</h2>");
  if (!st.enabled) {
    page += F("<p>The device list is switched off. Turn it on in "
              "<a href=\"/settings\">Settings</a>.</p>");
    page += FPSTR(html::PAGE_END);
    server.send(200, "text/html", page);
    return;
  }
  if (!st.scanned) {
    page += F("<p>The first scan has not finished yet. It starts about 40 seconds after the "
              "monitor powers on and takes up to a minute and a half. Reload this page then.</p>");
  } else {
    char stamp[16] = "";
    if (st.lastScan) timekeeper::formatStamp(st.lastScan, stamp, sizeof(stamp));
    page += "<p>" + String(st.online) + " online, " + String(st.known) + " known. Last scan";
    page += st.lastScan ? String(" ") + stamp : String(" finished");
    page += st.scanning ? ", scanning again now." : ". The network is scanned every 5 minutes.";
    page += F("</p>");
    if (st.capped) {
      page += F("<p>This network is larger than 254 addresses. Only the 254 addresses around "
                "the monitor's own are scanned.</p>");
    }
    if (st.onlyGateway) {
      page += F("<p class=\"warn\">The last scan found nothing but the router. This network "
                "probably has client isolation switched on (common on guest networks and "
                "public WiFi), which stops devices from seeing each other. The monitor cannot "
                "list devices on such a network.</p>");
    }
  }
  server.send(200, "text/html", page);

  // Online first, then by address.
  static uint8_t order[devices::MAX_DEVICES];
  static devices::View v, other;
  size_t n = devices::count();
  if (n > devices::MAX_DEVICES) n = devices::MAX_DEVICES;
  for (size_t i = 0; i < n; i++) order[i] = i;
  for (size_t i = 1; i < n; i++) {
    uint8_t key = order[i];
    if (!devices::get(key, v)) continue;
    size_t j = i;
    while (j > 0 && devices::get(order[j - 1], other) &&
           (other.online < v.online ||
            (other.online == v.online && ntohl(other.ip) > ntohl(v.ip)))) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }
  for (size_t i = 0; i < n; i++) {
    if (!devices::get(order[i], v)) continue;
    String title = v.nick[0] ? html::escape(v.nick)
                   : v.host[0] ? html::escape(v.host)
                               : String("Unnamed device");
    page = "<div class=\"dv\"><b>" + title + "</b><span class=\"tag ";
    page += v.online ? "g\">&#9650; ONLINE" : "m\">&#9661; OFFLINE";
    page += "</span>";
    if (v.gateway) page += "<span class=\"tag\">ROUTER</span>";
    if (v.trusted) page += "<span class=\"tag\">&#10003; TRUSTED</span>";
    page += "<p>" + IPAddress(v.ip).toString() + " &middot; " + macText(v.mac, true) + "</p><p>Maker: " +
            html::escape(v.vendor);
    if (v.host[0]) page += " &middot; Host name: " + html::escape(v.host);
    page += "</p><p>Ping: ";
    page += !v.online ? String("--") : v.pingMs >= 0 ? String(v.pingMs) + " ms" : String("no reply to pings");
    char first[16], last[16];
    timekeeper::formatStamp(v.firstSeen, first, sizeof(first));
    timekeeper::formatStamp(v.lastSeen, last, sizeof(last));
    page += " &middot; First seen ";
    page += first;
    page += " &middot; Last seen ";
    page += last;
    page += "</p><details><summary>Name or trust this device</summary><form method=\"post\" "
            "action=\"/devices\"><input type=\"hidden\" name=\"mac\" value=\"" + macText(v.mac, false) +
            "\"><label>Nickname<input type=\"text\" name=\"nick\" maxlength=\"20\" value=\"" +
            html::escape(v.nick) + "\"></label><label class=\"check\"><input type=\"checkbox\" "
            "name=\"trusted\" value=\"1\"";
    if (v.trusted) page += " checked";
    page += "> Trusted</label><button type=\"submit\" class=\"sec\">Save</button></form></details></div>";
    server.sendContent(page);
  }
  page = F("<p>Devices are found by asking every address on the monitor's own network who is "
           "there (ARP). A device that is asleep may show as offline. Makers come from a "
           "built-in list of common brands; phones often use a private address that belongs "
           "to no maker. Up to 64 devices are remembered. This list never leaves the monitor.</p>");
  page += FPSTR(html::PAGE_END);
  server.sendContent(page);
  server.sendContent("");
}

void handleDevicePost() {
  if (!allowed(true) || !requireAdmin("/devices")) return;
  uint8_t mac[6];
  String nick = server.arg("nick");
  nick.trim();
  if (!devices::parseMac(server.arg("mac"), mac) || !devices::validNick(nick) ||
      !devices::update(mac, nick, server.arg("trusted") == "1")) {
    server.send(400, "text/plain",
                "That did not work. A nickname can be up to 20 letters, digits and simple "
                "punctuation.");
    return;
  }
  redirect("/devices?saved=1");
}

// The website checks: one summary row per site, then each site's own log.
void handleSites() {
  if (!allowed(false)) return;
  const settings::Config &conf = settings::get();
  uint32_t now = timekeeper::now();
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  String page = html::pageStart("Uptime Sentinel websites");
  page += F("<meta http-equiv=\"refresh\" content=\"30\">"
            "<p><a href=\"/\">&larr; Back to the dashboard</a></p><h2>WEBSITES</h2>");
  if (!sites::any()) {
    page += F("<p>No websites are being checked. Add up to five in "
              "<a href=\"/settings\">Settings</a>.</p>");
    page += FPSTR(html::PAGE_END);
    server.send(200, "text/html", page);
    return;
  }
  const monitor::State &m = monitor::state();
  if (m.outage) {
    page += F("<p class=\"warn\">The internet connection is down, so website checks are "
              "paused. The figures below are from before the outage.</p>");
  }
  server.send(200, "text/html", page);

  static sites::View v;
  for (size_t i = 0; i < settings::MAX_SITES; i++) {
    sites::snapshot(i, v);
    if (!v.used) continue;
    const char *stateText = v.state == sites::State::UP     ? "&#9650; UP"
                            : v.state == sites::State::DOWN ? "&#9660; DOWN"
                                                            : "&#8230; WAITING";
    const char *stateClass = v.state == sites::State::UP ? "g" : v.state == sites::State::DOWN ? "o" : "m";
    page = "<h3>" + html::escape(conf.site[i].name) + "</h3><p style=\"margin:2px 0 0;"
           "word-break:break-all\">" + html::escape(conf.site[i].url) + "</p>";
    page += "<div class=\"grid\"><div class=\"cell\">STATE<b class=\"";
    page += stateClass;
    page += "\" style=\"font-size:16px\">";
    page += stateText;
    page += "</b></div><div class=\"cell\">RESPONSE<b>";
    page += v.code != 0 && v.ms >= 0 ? String(v.ms) + " ms" : String("--");
    page += "</b></div><div class=\"cell\">STATUS<b>";
    page += v.code != 0 ? "HTTP " + String(v.code) : String("--");
    page += "</b></div><div class=\"cell\">24 HOURS<b>" + pct(v.pct24h);
    page += "</b></div><div class=\"cell\">7 DAYS<b>" + pct(v.pct7d);
    page += "</b></div><div class=\"cell\">CERTIFICATE<b";
    if (!v.https) {
      page += ">none</b>";
    } else if (v.certExpiry == 0) {
      page += ">--</b>";
    } else {
      int32_t left = static_cast<int32_t>(v.certExpiry - now);
      int days = left > 0 ? left / 86400 : 0;
      if (days <= sites::CERT_WARN_DAYS) page += " class=\"o\"";
      page += left > 0 ? ">" + String(days) + (days == 1 ? " day" : " days") : String(">EXPIRED");
      page += days <= sites::CERT_WARN_DAYS && left > 0 ? " !</b>" : "</b>";
    }
    page += "</div></div><p>";
    if (v.checkedAt == 0) {
      page += "Not checked yet.";
    } else {
      char stamp[16];
      timekeeper::formatStamp(v.checkedAt, stamp, sizeof(stamp));
      page += "Last checked ";
      page += stamp;
      if (v.reason[0]) {
        page += ": failed (" + html::escape(v.reason) + "), " + String(v.fails) +
                (v.fails == 1 ? " time" : " times") + " in a row";
      }
      page += ".";
      if (v.https && v.certExpiry != 0) {
        time_t t = v.certExpiry;
        struct tm tm;
        localtime_r(&t, &tm);
        char date[12];
        strftime(date, sizeof(date), "%Y-%m-%d", &tm);
        page += " Certificate valid until ";
        page += date;
        page += ".";
      }
    }
    page += "</p><table><thead><tr><th>Went down</th><th>Lasted</th></tr></thead><tbody>";
    if (v.downStart != 0) {
      char stamp[16];
      timekeeper::formatStamp(v.downStart, stamp, sizeof(stamp));
      page += "<tr><td>";
      page += stamp;
      page += "</td><td class=\"o\">" + duration(now > v.downStart ? now - v.downStart : 0) +
              " (still down)</td></tr>";
    }
    for (size_t k = 0; k < v.downCount; k++) {
      char stamp[16];
      timekeeper::formatStamp(v.downs[k].start, stamp, sizeof(stamp));
      page += "<tr><td>";
      page += stamp;
      page += "</td><td>" + duration(v.downs[k].duration) + "</td></tr>";
    }
    if (v.downStart == 0 && v.downCount == 0) {
      page += "<tr><td colspan=\"2\" class=\"m\">No down periods logged</td></tr>";
    }
    page += "</tbody></table>";
    server.sendContent(page);
  }
  page = F("<h2>HOW THIS WORKS</h2><p>Sites are checked one at a time, each no more than once "
           "a minute. A site counts as down after 3 failed checks in a row: no response, a "
           "timeout, a certificate that does not verify, or a 5xx status. Response time is "
           "the time to connect plus the time to answer. Checks pause while the internet itself is down, and "
           "website trouble never counts against the internet uptime figures. The last 20 "
           "down periods of each site are kept. This page reloads every 30 seconds.</p>");
  page += FPSTR(html::PAGE_END);
  server.sendContent(page);
  server.sendContent("");
}

// The public IP log. This page is the only place the address is shown.
void handleIp() {
  if (!allowed(false)) return;
  static pubip::Snapshot snap;  // too big for the stack; one request at a time
  pubip::snapshot(snap);
  String page = html::pageStart("Uptime Sentinel public IP log");
  page += F("<p><a href=\"/\">&larr; Back to the dashboard</a></p><h2>PUBLIC IP ADDRESS</h2>");
  if (!snap.enabled) {
    page += F("<p>The public IP lookup is switched off. Turn it on in "
              "<a href=\"/settings\">Settings</a>.</p>");
  } else {
    char stamp[16] = "never";
    if (snap.checkedAt) timekeeper::formatStamp(snap.checkedAt, stamp, sizeof(stamp));
    page += F("<div class=\"grid\"><div class=\"cell\" style=\"grid-column:span 2\">NOW<b>");
    page += snap.current ? IPAddress(snap.current).toString() : String("--");
    page += F("</b></div><div class=\"cell\">CHECKED<b style=\"font-size:14px\">");
    page += stamp;
    page += F("</b></div></div><p>Latest lookup: ");
    page += html::escape(snap.status);
    page += F(". The address is looked up every 10 minutes and after each outage. It is "
              "shown only on this page.</p>");
  }
  page += F("<h2>CHANGES</h2><table><thead><tr><th>Seen</th><th>Old address</th>"
            "<th>New address</th></tr></thead><tbody>");
  if (snap.count == 0) {
    page += F("<tr><td colspan=\"3\" class=\"m\">No changes logged yet</td></tr>");
  }
  for (size_t i = 0; i < snap.count; i++) {
    char stamp[16];
    timekeeper::formatStamp(snap.changes[i].time, stamp, sizeof(stamp));
    page += "<tr><td>";
    page += stamp;
    page += "</td><td>" + IPAddress(snap.changes[i].from).toString() + "</td><td>" +
            IPAddress(snap.changes[i].to).toString() + "</td></tr>";
  }
  page += F("</tbody></table><p>The last 50 changes are kept.</p>");
  page += FPSTR(html::PAGE_END);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

// The display as a picture. No password: it shows nothing the dashboard
// does not. Builds without a display have no such page.
#ifndef HEADLESS
void handleScreenshot() {
  if (!allowed(false)) return;
  if (!screens::capture(server.arg("screen").c_str())) {
    display::freeCapture();
    if (server.hasArg("screen") && server.arg("screen").length()) {
      server.send(400, "text/plain", "screen must be status, latency, outages, uptime or sites "
                                       "(sites needs a website in Settings)");
    } else {
      server.send(503, "text/plain", "Not enough memory for a screenshot right now");
    }
    return;
  }
  size_t length;
  const uint8_t *file = display::captureFile(length);
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/bmp", reinterpret_cast<const char *>(file), length);
  display::freeCapture();
}
#endif

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

bool canUpdate() { return esp_ota_get_next_update_partition(nullptr) != nullptr; }

void sendUpdate(const String &notice) {
  String page = html::pageStart("Uptime Sentinel firmware update");
  page += F("<p><a href=\"/\">&larr; Back to the dashboard</a></p>");
  if (notice.length()) page += "<p class=\"warn\">" + html::escape(notice) + "</p>";
  page += F("<h2>UPDATE FIRMWARE</h2><div class=\"grid\"><div class=\"cell\">VERSION<b>");
  page += cfg::VERSION;
  page += F("</b></div><div class=\"cell\" style=\"grid-column:span 2\">BOARD<b>");
  page += cfg::BOARD;
  page += F("</b></div></div><p>New versions are published on the <a href=\"");
  page += cfg::RELEASES_URL;
  page += F("\" target=\"_blank\" rel=\"noopener\">GitHub releases page</a>. Download the "
            "file named <b>uptime-sentinel-");
  page += cfg::BOARD;
  page += F("-update.bin</b> and choose it here.</p>");
  if (!canUpdate()) {
    page += F("<p class=\"warn\">This build has no spare firmware slot, so it cannot be "
              "updated from the dashboard. Flash it over USB instead.</p>");
  } else if (!settings::hasAdmin()) {
    page += F("<p class=\"warn\">Updating from the dashboard is switched off until an admin "
              "password is set.</p><p><a href=\"/settings\">Set one in Settings</a>, then "
              "come back here.</p>");
  } else {
    page += F("<form method=\"post\" action=\"/update\" enctype=\"multipart/form-data\">"
              "<label for=\"firmware\">Firmware file (.bin)</label>"
              "<input id=\"firmware\" name=\"firmware\" type=\"file\" accept=\".bin\" required>"
              "<button type=\"submit\" onclick=\"setTimeout(()=>{this.disabled=true;"
              "this.textContent='Uploading, please wait...'},0)\">Upload and install</button>"
              "</form><p>The upload takes up to a minute. The file is checked before it "
              "is used: a damaged file, or one built for a different board, is refused "
              "and the monitor keeps running what it has. Your settings and the outage "
              "log are kept. Do not unplug the monitor while it updates.</p>");
  }
  page += FPSTR(html::PAGE_END);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

void handleUpdateGet() {
  if (!allowed(false)) return;
  if (canUpdate() && !requireAdmin("/update")) return;
  sendUpdate("");
}

void failUpload(const String &why) {
  if (up.failed) return;
  up.failed = true;
  up.error = why;
  Update.abort();
}

// The first bytes say what kind of file this is: an ESP application image,
// for this chip, and an application rather than a bootloader or a full
// flash image.
void checkImageHead() {
  esp_image_header_t running;
  const esp_partition_t *part = esp_ota_get_running_partition();
  uint16_t chip;
  uint32_t descMagic;
  memcpy(&chip, up.head + IMAGE_CHIP_OFFSET, sizeof(chip));
  memcpy(&descMagic, up.head + IMAGE_DESC_OFFSET, sizeof(descMagic));
  if (up.head[0] != ESP_IMAGE_HEADER_MAGIC || descMagic != ESP_APP_DESC_MAGIC_WORD) {
    failUpload(String("That is not a firmware update file. Choose the file named "
                      "uptime-sentinel-") + cfg::BOARD + "-update.bin.");
  } else if (!part || esp_partition_read(part, 0, &running, sizeof(running)) != ESP_OK ||
             chip != static_cast<uint16_t>(running.chip_id)) {
    failUpload("That firmware is for a different kind of ESP32 chip.");
  }
}

void scanForMarker(const uint8_t *data, size_t len) {
  if (up.markerFound) return;
  uint8_t join[2 * MARKER_LEN];
  size_t take = len < MARKER_LEN - 1 ? len : MARKER_LEN - 1;
  memcpy(join, up.tail, up.tailLen);
  memcpy(join + up.tailLen, data, take);
  size_t joined = up.tailLen + take;
  if (memmem(join, joined, cfg::BOARD_MARKER, MARKER_LEN) ||
      memmem(data, len, cfg::BOARD_MARKER, MARKER_LEN)) {
    up.markerFound = true;
    return;
  }
  if (len >= MARKER_LEN - 1) {
    up.tailLen = MARKER_LEN - 1;
    memcpy(up.tail, data + len - up.tailLen, up.tailLen);
  } else {
    // A short chunk: `join` already holds the old tail followed by all of it.
    up.tailLen = joined < MARKER_LEN - 1 ? joined : MARKER_LEN - 1;
    memmove(up.tail, join + joined - up.tailLen, up.tailLen);
  }
}

// Called by the web server as each piece of the uploaded file arrives,
// before handleUpdatePost() runs.
void handleUpdateData() {
  HTTPUpload &part = server.upload();
  if (part.status == UPLOAD_FILE_START) {
    up = Upload();
    up.started = true;
    up.permitted = requestOk(true) && canUpdate() && settings::hasAdmin() && sessionValid();
    if (!up.permitted) return;
    Serial.println("[update] receiving firmware");
    display::message("UPDATE", "Receiving firmware...", "", "Do not unplug");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      failUpload(String("Could not start the update (") + Update.errorString() + ").");
    }
  } else if (part.status == UPLOAD_FILE_WRITE) {
    if (!up.permitted || up.failed) return;
    if (up.headLen < IMAGE_HEAD_LEN) {
      size_t take = IMAGE_HEAD_LEN - up.headLen;
      if (take > part.currentSize) take = part.currentSize;
      memcpy(up.head + up.headLen, part.buf, take);
      up.headLen += take;
      if (up.headLen == IMAGE_HEAD_LEN) checkImageHead();
      if (up.failed) return;
    }
    scanForMarker(part.buf, part.currentSize);
    if (Update.write(part.buf, part.currentSize) != part.currentSize) {
      failUpload(String("Could not store the firmware (") + Update.errorString() + ").");
      return;
    }
    up.received += part.currentSize;
    if (up.received / 1024 >= up.shownKb + 64) {
      up.shownKb = up.received / 1024;
      char line[24];
      snprintf(line, sizeof(line), "%u KB", static_cast<unsigned>(up.shownKb));
      display::message("UPDATE", "Receiving firmware...", line, "Do not unplug");
    }
  } else if (part.status == UPLOAD_FILE_ABORTED) {
    if (up.permitted) failUpload("The upload was interrupted.");
  }
}

void handleUpdatePost() {
  Upload done = up;
  up = Upload();
  bool refused = !requestOk(true) || !canUpdate() || !settings::hasAdmin() || !sessionValid();
  if (refused) {
    Update.abort();
    if (!allowed(true)) return;
    if (canUpdate() && !requireAdmin("/update")) return;
    sendUpdate("");
    return;
  }

  String error = done.error;
  if (!done.started || !done.permitted) {
    error = "No file arrived. Choose a file and try again.";
  } else if (!done.failed) {
    if (done.headLen < IMAGE_HEAD_LEN) {
      error = "No file was chosen, or it was empty.";
    } else if (!done.markerFound) {
      error = String("That firmware was not built for this board. This one needs "
                     "uptime-sentinel-") + cfg::BOARD + "-update.bin.";
    } else if (!Update.end(true)) {
      // end() checks the image's own checksum and SHA-256 before switching to it.
      error = String("The file is damaged or incomplete (") + Update.errorString() +
              "). Download it again.";
    }
  }
  if (error.length()) {
    Update.abort();
    Serial.printf("[update] refused: %s\n", error.c_str());
    screens::show();
    sendUpdate(error);
    return;
  }

  Serial.printf("[update] %u bytes installed; restarting\n", static_cast<unsigned>(done.received));
  display::message("UPDATE", "Firmware installed.", "Restarting...");
  String page = html::pageStart("Uptime Sentinel firmware update");
  page += F("<p>The new firmware is installed. The monitor is restarting; the dashboard will "
            "be back in about 20 seconds.</p><p><a href=\"/\">Back to the dashboard</a></p>");
  page += FPSTR(html::PAGE_END);
  server.send(200, "text/html", page);
  restartPending = true;
  restartAt = millis() + 1500;
}

// The end of one alert channel's section: whether something is saved, the
// box to remove it and the test button. Never the saved value itself.
String channelFooter(alerts::Channel channel, const char *clearName) {
  String out;
  if (!alerts::configured(channel)) {
    out += F("<p>Not set up.</p>");
    return out;
  }
  out += F("<p>Saved (not shown here).</p><label class=\"check\"><input type=\"checkbox\" "
           "name=\"");
  out += clearName;
  out += F("\" value=\"1\"> Remove it</label><button type=\"submit\" class=\"sec\" "
           "formaction=\"/alerts/test\" formnovalidate name=\"ch\" value=\"");
  out += alerts::channelKey(channel);
  out += F("\">Send test message to ");
  out += alerts::channelName(channel);
  out += F("</button>");
  return out;
}

void sendSettings(const String &notice, bool testNotice = false) {
  const settings::Config &conf = settings::get();
  String page = html::pageStart("Uptime Sentinel settings");
  page += F("<p><a href=\"/\">&larr; Back to the dashboard</a></p>");
  if (notice.length()) page += "<p class=\"warn\">" + html::escape(notice) + "</p>";
  page += F("<form method=\"post\" action=\"/settings\">");
  if (testNotice) {
    page += F("<p id=\"tr\" class=\"warn\" aria-live=\"polite\">Sending the test message...</p>"
              "<script>const tr=document.getElementById('tr');async function tp(){try{"
              "const r=await fetch('/api/alerttest');tr.textContent=await r.text();"
              "if(tr.textContent.indexOf('Sending')==0)setTimeout(tp,1500)}catch(e){"
              "setTimeout(tp,3000)}}tp()</script>");
  }
  page += F("<h2>ALERTS</h2><p>One message each time an ISP outage ends, sent to every channel "
            "set up here. All four are optional. Saved addresses and tokens are never shown "
            "again: leave a box empty to keep what is saved.</p>");

  page += F("<h3>Discord</h3><label for=\"hook\">Webhook URL</label>"
            "<input id=\"hook\" name=\"hook\" type=\"url\" maxlength=\"240\" "
            "autocomplete=\"off\" placeholder=\"https://discord.com/api/webhooks/...\">");
  page += channelFooter(alerts::DISCORD, "clearhook");

  page += F("<h3>ntfy</h3><label for=\"ntfyurl\">Topic URL</label>"
            "<input id=\"ntfyurl\" name=\"ntfyurl\" type=\"url\" maxlength=\"240\" "
            "autocomplete=\"off\" placeholder=\"https://ntfy.sh/your-topic-name\">"
            "<label for=\"ntfytok\">Access token, only if the topic needs one</label>"
            "<input id=\"ntfytok\" name=\"ntfytok\" type=\"password\" maxlength=\"120\" "
            "autocomplete=\"off\">");
  page += channelFooter(alerts::NTFY, "clearntfy");

  page += F("<h3>Telegram</h3><label for=\"tgtok\">Bot token (from @BotFather)</label>"
            "<input id=\"tgtok\" name=\"tgtok\" type=\"password\" maxlength=\"80\" "
            "autocomplete=\"off\"><label for=\"tgchat\">Chat ID</label>"
            "<input id=\"tgchat\" name=\"tgchat\" type=\"text\" maxlength=\"40\" "
            "autocomplete=\"off\">");
  page += channelFooter(alerts::TELEGRAM, "cleartg");

  page += F("<h3>Webhook (JSON POST)</h3><label for=\"whurl\">URL</label>"
            "<input id=\"whurl\" name=\"whurl\" type=\"url\" maxlength=\"240\" "
            "autocomplete=\"off\" placeholder=\"https://example.com/hook\">");
  page += channelFooter(alerts::WEBHOOK, "clearwh");
  page += F("<h2>WEBSITES</h2><p>Up to five websites to keep an eye on. Give each a short "
            "name and its address, starting with https:// or http://. Empty the address to "
            "remove one. Alerts go to the channels above when a site goes down, comes back, "
            "or its certificate is about to expire.</p>");
  for (size_t i = 0; i < settings::MAX_SITES; i++) {
    String n = String(i + 1);
    page += "<label for=\"sn" + n + "\">Website " + n + " name</label><input type=\"text\" id=\"sn" +
            n + "\" name=\"sn" + n + "\" maxlength=\"24\" value=\"" + html::escape(conf.site[i].name) +
            "\"><label for=\"su" + n + "\">Website " + n + " address</label><input type=\"url\" "
            "id=\"su" + n + "\" name=\"su" + n + "\" maxlength=\"120\" autocapitalize=\"none\" "
            "placeholder=\"https://example.com\" value=\"" + html::escape(conf.site[i].url) + "\">";
  }
  page += F("<label class=\"check\"><input type=\"checkbox\" name=\"sitemet\" value=\"1\"");
  if (conf.siteMetrics) page += F(" checked");
  page += F("> Include these websites in /metrics (which needs no password)</label>");
  page += F("<h2>DEVICES</h2><label class=\"check\"><input type=\"checkbox\" name=\"devscan\" "
            "value=\"1\"");
  if (conf.deviceScan) page += F(" checked");
  page += F("> List the devices on this network (a slow scan every 5 minutes)</label>"
            "<label class=\"check\"><input type=\"checkbox\" name=\"devalert\" value=\"1\"");
  if (conf.deviceAlert) page += F(" checked");
  page += F("> Send an alert when a device that has never been seen before joins</label>");
  page += F("<h2>TIME AND TARGETS</h2>");
  page += html::optionFields();
  page += F("<h2>PUBLIC IP LOG</h2><label class=\"check\"><input type=\"checkbox\" "
            "name=\"ipon\" value=\"1\"");
  if (conf.ipEnabled) page += F(" checked");
  page += F("> Look up and log the public IP address</label>"
            "<label for=\"ipurl1\">Lookup service (HTTPS, answers with the address as "
            "plain text)</label><input type=\"url\" id=\"ipurl1\" name=\"ipurl1\" "
            "maxlength=\"120\" value=\"");
  page += html::escape(conf.ipUrl[0]);
  page += F("\"><label for=\"ipurl2\">Second service, used when the first fails</label>"
            "<input type=\"url\" id=\"ipurl2\" name=\"ipurl2\" maxlength=\"120\" value=\"");
  page += html::escape(conf.ipUrl[1]);
  page += F("\"><p>Leave a box empty to go back to its default.</p>");
  page += F("<h2>ADMIN PASSWORD</h2>");
  page += html::adminFields();
  if (net::HAS_ETHERNET) {
    // A board that was set up over its cable never saw the setup portal, so
    // this is where it learns a WiFi network to fall back to.
    page += F("<h2>WIFI (USED WHEN THE CABLE IS UNPLUGGED)</h2>"
              "<label for=\"wssid\">Network name, or empty for none</label>"
              "<input type=\"text\" id=\"wssid\" name=\"wssid\" maxlength=\"32\" "
              "autocapitalize=\"none\" value=\"");
    page += html::escape(conf.ssid);
    page += F("\"><label for=\"wpass\">WiFi password</label>"
              "<input id=\"wpass\" name=\"wpass\" type=\"password\" maxlength=\"64\" "
              "autocomplete=\"off\">");
    if (conf.ssid.length()) {
      page += F("<p>The saved password is not shown. Leave the box empty to keep it.</p>");
    }
    page += F("<button type=\"submit\">Save and restart</button></form>");
  } else {
    page += F("<button type=\"submit\">Save and restart</button></form><h2>WIFI</h2>"
              "<p>Connected to <b>");
    page += html::escape(conf.ssid);
    page += F("</b>. To move the monitor to a different network, hold its wipe gesture "
              "for 5 seconds and run the setup again.</p>");
  }
  if (settings::hasAdmin()) page += F("<p><a href=\"/logout\">Log out</a></p>");
  page += FPSTR(html::PAGE_END);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

void handleSettingsGet() {
  if (!allowed(false) || !requireAdmin("/settings")) return;
  sendSettings("", server.arg("test") == "1");
}

// "Send test message" on the Settings page. The message goes out from the
// alert task; the page then asks /api/alerttest how it went.
void handleAlertTest() {
  if (!allowed(true) || !requireAdmin("/settings")) return;
  alerts::Channel channel;
  if (!alerts::parseChannel(server.arg("ch"), channel) || !alerts::configured(channel)) {
    sendSettings("Save that alert channel first, then send a test message.");
    return;
  }
  if (!alerts::sendTest(channel)) {
    sendSettings("The monitor is busy sending other alerts. Try again in a minute.");
    return;
  }
  redirect("/settings?test=1");
}

void handleAlertTestStatus() {
  if (!requestOk(false) || (settings::hasAdmin() && !sessionValid())) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  alerts::TestResult r = alerts::lastTest();
  String text;
  if (r.state == alerts::TestState::NONE) {
    text = "No test message has been sent.";
  } else if (r.state == alerts::TestState::SENDING) {
    text = String("Sending a test message to ") + alerts::channelName(r.channel) + "...";
  } else if (r.state == alerts::TestState::DELIVERED) {
    text = String("Test message delivered to ") + alerts::channelName(r.channel) + " (HTTP " +
           String(r.code) + ").";
  } else if (r.code > 0) {
    text = String("Test message to ") + alerts::channelName(r.channel) + " was refused (HTTP " +
           String(r.code) + "). Check the address or token.";
  } else {
    text = String("Test message to ") + alerts::channelName(r.channel) +
           " failed: no answer from the server. Check the address.";
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", text);
}

void handleSettingsPost() {
  if (!allowed(true) || !requireAdmin("/settings")) return;
  String hook = server.arg("hook");
  hook.trim();
  String tz, target1, target2, admin, error;
  bool adminChange = false;
  String ntfyUrl = server.arg("ntfyurl"), ntfyToken = server.arg("ntfytok");
  String tgToken = server.arg("tgtok"), tgChat = server.arg("tgchat");
  String webhook = server.arg("whurl");
  ntfyUrl.trim();
  ntfyToken.trim();
  tgToken.trim();
  tgChat.trim();
  webhook.trim();
  const settings::Config &saved = settings::get();
  if (!settings::validHook(hook)) {
    error = "The Discord webhook URL must start with https://";
  } else if (!settings::validAlertUrl(ntfyUrl)) {
    error = "The ntfy topic URL must start with https:// or http:// and have no spaces.";
  } else if (!settings::validToken(ntfyToken)) {
    error = "The ntfy access token has characters that cannot be used.";
  } else if (!settings::validTelegramToken(tgToken)) {
    error = "That does not look like a Telegram bot token (numbers, a colon, then letters).";
  } else if (!settings::validTelegramChat(tgChat)) {
    error = "The Telegram chat ID must be a number, or @ and a channel name.";
  } else if ((tgToken.length() || tgChat.length()) && server.arg("cleartg") != "1" &&
             (tgToken.length() ? tgToken : saved.tgToken).length() *
                     (tgChat.length() ? tgChat : saved.tgChat).length() == 0) {
    error = "Telegram needs both a bot token and a chat ID.";
  } else if (!settings::validAlertUrl(webhook)) {
    error = "The webhook URL must start with https:// or http:// and have no spaces.";
  } else if (html::readOptions(server, tz, target1, target2, error)) {
    html::readAdmin(server, admin, adminChange, error);
  }
  settings::Site newSites[settings::MAX_SITES];
  for (size_t i = 0; i < settings::MAX_SITES && error.length() == 0; i++) {
    settings::Site &site = newSites[i];
    site.name = server.arg("sn" + String(i + 1));
    site.url = server.arg("su" + String(i + 1));
    site.name.trim();
    site.url.trim();
    if (site.url.length() == 0) {
      site.name = "";
      continue;
    }
    if (!settings::validSiteUrl(site.url)) {
      error = "Website " + String(i + 1) + ": the address must start with https:// or http://, "
              "have no spaces and be 120 characters at most.";
      break;
    }
    if (site.name.length() == 0) {
      // No name given: use the host part of the address.
      int start = site.url.indexOf("//") + 2;
      int end = site.url.indexOf('/', start);
      site.name = site.url.substring(start, end < 0 ? site.url.length() : end);
      if (site.name.length() > settings::SITE_NAME_MAX_LEN) {
        site.name.remove(settings::SITE_NAME_MAX_LEN);
      }
    }
    if (!settings::validSiteName(site.name)) {
      error = "Website " + String(i + 1) + ": the name can be up to 24 letters, digits and "
              "simple punctuation.";
    }
  }
  String ipUrl1 = server.arg("ipurl1");
  String ipUrl2 = server.arg("ipurl2");
  ipUrl1.trim();
  ipUrl2.trim();
  if (ipUrl1.length() == 0) ipUrl1 = settings::DEFAULT_IP_URL1;
  if (ipUrl2.length() == 0) ipUrl2 = settings::DEFAULT_IP_URL2;
  if (error.length() == 0 &&
      (!settings::validLookupUrl(ipUrl1) || !settings::validLookupUrl(ipUrl2))) {
    error = "A public IP lookup service must be an https:// address without spaces.";
  }
  String wifiName = server.arg("wssid");
  wifiName.trim();
  if (net::HAS_ETHERNET && error.length() == 0 && wifiName.length() > 32) {
    error = "The WiFi network name is too long.";
  }
  if (error.length()) {
    sendSettings(error);
    return;
  }

  if (net::HAS_ETHERNET) {
    const settings::Config &old = settings::get();
    String wifiPass = server.arg("wpass");
    if (wifiPass.length() == 0 && wifiName == old.ssid) wifiPass = old.pass;
    if (wifiName != old.ssid || wifiPass != old.pass) settings::saveWifi(wifiName, wifiPass);
  }
  settings::saveOptions(tz, target1, target2);
  settings::saveIpLookup(server.arg("ipon") == "1", ipUrl1, ipUrl2);
  settings::saveSites(newSites, server.arg("sitemet") == "1");
  settings::saveDevices(server.arg("devscan") == "1", server.arg("devalert") == "1");
  if (server.arg("clearhook") == "1" && hook.length() == 0) settings::saveHook("");
  else if (hook.length()) settings::saveHook(hook);
  // Each box that was filled in replaces what was saved; empty ones keep it.
  if (server.arg("clearntfy") == "1" && ntfyUrl.length() == 0) {
    settings::saveNtfy("", "");
  } else if (ntfyUrl.length() || ntfyToken.length()) {
    settings::saveNtfy(ntfyUrl.length() ? ntfyUrl : saved.ntfyUrl,
                       ntfyToken.length() ? ntfyToken : saved.ntfyToken);
  }
  if (server.arg("cleartg") == "1" && tgToken.length() == 0) {
    settings::saveTelegram("", "");
  } else if (tgToken.length() || tgChat.length()) {
    settings::saveTelegram(tgToken.length() ? tgToken : saved.tgToken,
                           tgChat.length() ? tgChat : saved.tgChat);
  }
  if (server.arg("clearwh") == "1" && webhook.length() == 0) settings::saveWebhook("");
  else if (webhook.length()) settings::saveWebhook(webhook);
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
  server.on("/api/days", HTTP_GET, handleDays);
  server.on("/log.csv", HTTP_GET, handleCsv);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.on("/devices", HTTP_GET, handleDevices);
  server.on("/devices", HTTP_POST, handleDevicePost);
  server.on("/sites", HTTP_GET, handleSites);
  server.on("/report", HTTP_GET, handleReport);
  server.on("/ip", HTTP_GET, handleIp);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/alerts/test", HTTP_POST, handleAlertTest);
  server.on("/api/alerttest", HTTP_GET, handleAlertTestStatus);
  server.on("/login", HTTP_GET, handleLoginGet);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);
#ifndef HEADLESS
  server.on("/screenshot.bmp", HTTP_GET, handleScreenshot);
#endif
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateData);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  // mDNS needs an IP address, so start it on the first pass with the link up.
  if (!mdnsStarted && net::up()) {
    if (MDNS.begin(cfg::MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.printf("[web] dashboard at http://%s.local or http://%s\n", cfg::MDNS_NAME,
                    net::localIP().toString().c_str());
    }
  }
  server.handleClient();
  if (restartPending && static_cast<int32_t>(millis() - restartAt) >= 0) {
    delay(100);
    ESP.restart();
  }
}

}  // namespace web
