#include "report.h"

#include <time.h>

#include "config.h"
#include "daily.h"
#include "html.h"
#include "logview.h"
#include "monitor.h"
#include "settings.h"
#include "storage.h"
#include "timekeeper.h"

namespace report {
namespace {

constexpr int MAX_MONTHS_LISTED = 24;
constexpr size_t MAX_DAYS = 34;

const char HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:system-ui,"Segoe UI",Arial,sans-serif;color:#111;background:#fff;
max-width:760px;margin:0 auto;padding:24px;font-size:14px;line-height:1.45}
h1{font-size:22px;margin:0 0 2px}
h2{font-size:15px;margin:26px 0 8px;border-bottom:1px solid #888;padding-bottom:4px}
p{margin:6px 0}
table{width:100%;border-collapse:collapse}
td,th{padding:5px 6px;border-bottom:1px solid #ccc;text-align:left;vertical-align:top}
th{font-weight:600}
.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.sum td:first-child{width:45%;color:#333}
.sum td:last-child{font-weight:600}
.note{border:1px solid #888;padding:10px 12px;margin-top:18px}
.nav{margin-bottom:20px;padding-bottom:12px;border-bottom:1px solid #ccc}
.nav select,.nav button{font:inherit;padding:6px 10px;margin-right:6px}
.small{color:#444;font-size:12px}
@page{margin:18mm}
@media print{.nav{display:none}body{padding:0;max-width:none;font-size:10.5pt}
tr{break-inside:avoid}thead{display:table-header-group}h2{break-after:avoid}
.note{break-inside:avoid}}
</style>)HTML";

uint32_t monthStart(int year, int month) {  // month 13 rolls into the next year
  struct tm tm = {};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = 1;
  tm.tm_isdst = -1;
  return static_cast<uint32_t>(mktime(&tm));
}

String stamp(uint32_t epoch, const char *format = "%Y-%m-%d %H:%M:%S") {
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char text[40];
  strftime(text, sizeof(text), format, &tm);
  return text;
}

// Exact, unlike the rounded form used on the small screen.
String exact(uint32_t s) {
  char text[24];
  if (s < 60) {
    snprintf(text, sizeof(text), "%us", static_cast<unsigned>(s));
  } else if (s < 3600) {
    snprintf(text, sizeof(text), "%um %02us", static_cast<unsigned>(s / 60),
             static_cast<unsigned>(s % 60));
  } else {
    snprintf(text, sizeof(text), "%uh %02um %02us", static_cast<unsigned>(s / 3600),
             static_cast<unsigned>(s % 3600 / 60), static_cast<unsigned>(s % 60));
  }
  return text;
}

uint32_t overlap(uint32_t start, uint32_t end, uint32_t from, uint32_t to) {
  uint32_t a = start > from ? start : from;
  uint32_t b = end < to ? end : to;
  return b > a ? b - a : 0;
}

// Part of the month: it overlaps it, or (a zero-length entry) began in it.
bool inMonth(const logview::Row &r, uint32_t from, uint32_t to) {
  return overlap(r.start, r.start + r.duration, from, to) > 0 || (r.start >= from && r.start < to);
}

bool parseMonth(const String &text, int &year, int &month) {
  if (text.length() != 7 || text[4] != '-') return false;
  for (int i = 0; i < 7; i++) {
    if (i != 4 && !isDigit(text[i])) return false;
  }
  int y = text.substring(0, 4).toInt();
  int m = text.substring(5).toInt();
  if (y < 2024 || y > 2099 || m < 1 || m > 12) return false;
  year = y;
  month = m;
  return true;
}

String row(const char *label, const String &value) {
  return String("<tr><td>") + label + "</td><td>" + value + "</td></tr>";
}

}  // namespace

void send(WebServer &server) {
  server.sendHeader("Cache-Control", "no-store");
  String page = FPSTR(HEAD);
  if (!timekeeper::valid()) {
    page += F("<title>Internet outage report</title></head><body><p class=\"nav\">"
              "<a href=\"/\">&larr; Back to the dashboard</a></p><p>The monitor has not "
              "been able to set its clock yet, so it cannot make a report. Try again once "
              "it has reached the internet.</p></body></html>");
    server.send(200, "text/html", page);
    return;
  }

  uint32_t now = timekeeper::now();
  time_t t = now;
  struct tm nowTm;
  localtime_r(&t, &nowTm);
  int curYear = nowTm.tm_year + 1900, curMonth = nowTm.tm_mon + 1;
  int year = curYear, month = curMonth;
  parseMonth(server.arg("month"), year, month);

  uint32_t from = monthStart(year, month);
  uint32_t to = monthStart(year, month + 1);
  uint32_t since = settings::since();
  uint32_t watchFrom = since > from ? since : from;
  uint32_t watchTo = now < to ? now : to;
  uint32_t span = since != 0 && watchTo > watchFrom ? watchTo - watchFrom : 0;

  // First pass over the log: the summary.
  static daily::Entry days[MAX_DAYS];
  size_t dayCount = 0;
  uint32_t down = 0, longest = 0, longestStart = 0, powerOff = 0;
  unsigned outages = 0;
  size_t total = logview::count();
  for (size_t i = total; i-- > 0;) {
    logview::Row r = logview::get(i);
    if (r.start == 0 || !inMonth(r, from, to)) continue;
    uint32_t part = overlap(r.start, r.start + r.duration, from, to);
    if (r.cause == Cause::POWER) powerOff += part;
    if (!isIsp(r.cause)) continue;
    down += part;
    if (r.start >= from) outages++;
    if (r.duration > longest) {
      longest = r.duration;
      longestStart = r.start;
    }
    if (part) daily::split(days, dayCount, MAX_DAYS, r.start > from ? r.start : from, part);
  }
  const daily::Entry *worst = nullptr;
  for (size_t i = 0; i < dayCount; i++) {
    if (!worst || days[i].downSec > worst->downSec) worst = &days[i];
  }
  uint32_t measured = span > powerOff ? span - powerOff : 0;
  if (down > measured) down = measured;

  String title = "Internet outage report, " + stamp(from, "%B %Y");
  page += "<title>" + title + "</title></head><body>";

  page += F("<form class=\"nav\" method=\"get\" action=\"/report\"><a href=\"/\">&larr; "
            "Dashboard</a> &nbsp; <label for=\"month\">Month</label> "
            "<select id=\"month\" name=\"month\">");
  int listYear = curYear, listMonth = curMonth;
  bool listed = false;
  for (int i = 0; i < MAX_MONTHS_LISTED; i++) {
    // Stop at the month the monitor was first set up.
    if (i > 0 && (since == 0 || monthStart(listYear, listMonth + 1) <= since)) break;
    char value[8];
    snprintf(value, sizeof(value), "%04d-%02d", listYear, listMonth);
    bool selected = listYear == year && listMonth == month;
    listed = listed || selected;
    page += String("<option value=\"") + value + (selected ? "\" selected>" : "\">") +
            stamp(monthStart(listYear, listMonth), "%B %Y") + "</option>";
    if (--listMonth == 0) {
      listMonth = 12;
      listYear--;
    }
  }
  if (!listed) {
    char value[8];
    snprintf(value, sizeof(value), "%04d-%02d", year, month);
    page += String("<option value=\"") + value + "\" selected>" + stamp(from, "%B %Y") +
            "</option>";
  }
  page += F("</select><button type=\"submit\">Show</button>"
            "<button type=\"button\" onclick=\"window.print()\">Print or save as PDF</button>"
            "</form>");

  page += "<h1>" + title + "</h1><p class=\"small\">Generated " + stamp(now, "%Y-%m-%d %H:%M") +
          " by Uptime Sentinel " + cfg::VERSION + ". All times are local time (" +
          stamp(now, "%Z, UTC%z") + ").</p>";

  page += F("<h2>Summary</h2><table class=\"sum\">");
  if (span == 0) {
    page += row("Period monitored", "The monitor was not running in this month");
  } else {
    page += row("Period monitored", stamp(watchFrom, "%Y-%m-%d %H:%M") + " to " +
                                        stamp(watchTo, "%Y-%m-%d %H:%M") +
                                        (watchTo == now ? " (month in progress)" : ""));
    if (powerOff) {
      page += row("Monitor without power", exact(powerOff) + " (not measured, not counted)");
    }
    page += row("ISP outages", String(outages));
    page += row("Total downtime", exact(down));
    char pct[16];
    snprintf(pct, sizeof(pct), "%.3f%%",
             measured ? 100.0 * (1.0 - static_cast<double>(down) / measured) : 100.0);
    page += row("Uptime", pct);
    page += row("Longest outage",
                longest ? exact(longest) + ", began " + stamp(longestStart) : String("None"));
    char day[12] = "";
    if (worst) daily::formatDay(worst->day, day, sizeof(day));
    page += row("Worst day",
                worst ? String(day) + ", " + exact(worst->downSec) + " down" : String("None"));
  }
  page += F("</table>");

  // The log only reaches back 200 entries.
  if (storage::count() == cfg::LOG_CAPACITY && span != 0) {
    uint32_t oldest = storage::get(storage::count() - 1).start;
    if (oldest > watchFrom) {
      page += "<p class=\"note\">The monitor keeps its latest " + String(cfg::LOG_CAPACITY) +
              " log entries. Entries from before " + stamp(oldest, "%Y-%m-%d %H:%M") +
              " have been replaced by newer ones, so this month is incomplete: the true "
              "figures are at least as bad as those shown.</p>";
    }
  }

  page += F("<h2>ISP outages</h2><table><thead><tr><th>#</th><th>Start</th><th>End</th>"
            "<th class=\"n\">Duration</th><th>Type</th></tr></thead><tbody>");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", page);
  page = "";

  // Second pass: the rows, oldest first.
  unsigned number = 0;
  for (size_t i = total; i-- > 0;) {
    logview::Row r = logview::get(i);
    if (r.start == 0 || !isIsp(r.cause) || !inMonth(r, from, to)) continue;
    page += "<tr><td>" + String(++number) + "</td><td>" + stamp(r.start) + "</td><td>" +
            (r.active ? String("still down") : stamp(r.start + r.duration)) +
            "</td><td class=\"n\">" + exact(r.duration) + "</td><td>" +
            monitor::causeName(r.cause) + "</td></tr>";
    if (page.length() > 1000) {
      server.sendContent(page);
      page = "";
    }
  }
  if (number == 0) page += F("<tr><td colspan=\"5\">No ISP outages recorded.</td></tr>");
  page += F("</tbody></table>");

  const settings::Config &conf = settings::get();
  page += F("<div class=\"note\"><p><b>How this was measured.</b> These figures come from "
            "one monitoring device at one point on the customer's network. They show what "
            "that single device observed and are not an independent or certified "
            "measurement of the service.</p><p>Every ");
  page += String(cfg::PING_INTERVAL_MS / 1000);
  page += F(" seconds the device pings its own router and two internet addresses (");
  page += html::escape(conf.target[0]) + " and " + html::escape(conf.target[1]);
  page += F("). An ISP outage is recorded only when the router answers and neither internet "
            "address does for ");
  page += String(cfg::FAIL_ROUNDS);
  page += F(" checks in a row, so a fault inside the home network (WiFi or router) is not "
            "counted. Outages shorter than about ");
  page += String(cfg::FAIL_ROUNDS * cfg::PING_INTERVAL_MS / 1000);
  page += F(" seconds are not detected. Types: ISP LOCAL means the first ISP device beyond "
            "the router stopped answering too; ISP UPSTREAM means it kept answering and the "
            "fault was further inside the ISP's network; ISP means the device could not tell "
            "which.</p></div></body></html>");
  server.sendContent(page);
  server.sendContent("");
}

}  // namespace report
