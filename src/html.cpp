#include "html.h"

#include "settings.h"

namespace html {

namespace {

const char HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{background:#0B0E11;color:#E8EDF2;font-family:ui-monospace,Menlo,Consolas,monospace;
margin:0 auto;padding:20px;max-width:520px}
h1{font-size:18px;color:#3DDC84;margin:0 0 4px}
h2{font-size:13px;color:#9AA4AF;font-weight:normal;letter-spacing:1px;margin:28px 0 8px;
border-bottom:1px solid #1C232B;padding-bottom:6px}
p{color:#9AA4AF;font-size:13px;line-height:1.5}
label{display:block;color:#9AA4AF;font-size:13px;margin:18px 0 6px}
select,input[type=text],input[type=password],input[type=url],input[type=file]{width:100%;box-sizing:border-box;
padding:12px;background:#1C232B;color:#E8EDF2;border:1px solid #2A333D;border-radius:6px;
font:inherit;font-size:16px}
button{margin-top:24px;width:100%;padding:14px;background:#3DDC84;color:#0B0E11;border:0;
border-radius:6px;font:inherit;font-size:16px;font-weight:bold}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{padding:7px 4px;border-bottom:1px solid #1C232B;text-align:left}
th{color:#9AA4AF;font-weight:normal}
a{color:#9AA4AF}.warn{color:#FF8A3D}.big{font-size:44px;font-weight:bold;margin:14px 0 2px}
.g{color:#3DDC84}.o{color:#FF8A3D}.y{color:#FFD23D}.m{color:#9AA4AF}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px}
.cell{background:#12171C;border:1px solid #1C232B;border-radius:6px;padding:10px;font-size:13px}
.cell b{display:block;font-size:20px;margin-top:4px;color:#E8EDF2}
canvas{width:100%;height:90px;margin-top:12px;display:block}
button.sec{background:#1C232B;color:#E8EDF2;border:1px solid #2A333D;margin-top:12px;
font-weight:normal;font-size:14px;padding:10px}
h3{font-size:13px;color:#E8EDF2;margin:22px 0 0}
.check{display:flex;align-items:center;gap:8px;margin-top:12px;color:#9AA4AF;font-size:13px}
</style>)HTML";

}  // namespace

const char PAGE_END[] PROGMEM = "</body></html>";

String escape(const String &in) {
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

String pageStart(const char *title) {
  String page = FPSTR(HEAD);
  page += "<title>";
  page += title;
  page += "</title></head><body><h1>Uptime Sentinel</h1>";
  return page;
}

String optionFields() {
  const settings::Config &conf = settings::get();
  String out = F("<label for=\"tz\">Timezone</label><select id=\"tz\" name=\"tz\">");
  bool matched = false;
  for (size_t i = 0; i < settings::ZONE_COUNT; i++) {
    bool selected = conf.tz == settings::ZONES[i].tz;
    matched = matched || selected;
    out += "<option value=\"" + escape(settings::ZONES[i].tz) + "\"";
    if (selected) out += " selected";
    out += ">";
    out += settings::ZONES[i].label;
    out += "</option>";
  }
  out += "<option value=\"custom\"";
  if (!matched) out += " selected";
  out += F(">Other (type a POSIX TZ string below)</option></select>"
           "<label for=\"tzcustom\">Custom timezone, only if you chose Other</label>"
           "<input type=\"text\" id=\"tzcustom\" name=\"tzcustom\" maxlength=\"48\" value=\"");
  if (!matched) out += escape(conf.tz);
  out += F("\"><label for=\"target1\">Internet ping target 1 (IP address)</label>"
           "<input type=\"text\" id=\"target1\" name=\"target1\" maxlength=\"15\" "
           "inputmode=\"decimal\" value=\"");
  out += escape(conf.target[0]);
  out += F("\"><label for=\"target2\">Internet ping target 2 (IP address)</label>"
           "<input type=\"text\" id=\"target2\" name=\"target2\" maxlength=\"15\" "
           "inputmode=\"decimal\" value=\"");
  out += escape(conf.target[1]);
  out += "\">";
  return out;
}

bool readOptions(WebServer &server, String &tz, String &target1, String &target2,
                 String &error) {
  tz = server.arg("tz");
  if (tz == "custom" || tz.length() == 0) tz = server.arg("tzcustom");
  tz.trim();
  target1 = server.arg("target1");
  target2 = server.arg("target2");
  target1.trim();
  target2.trim();
  if (target1.length() == 0) target1 = settings::DEFAULT_TARGET1;
  if (target2.length() == 0) target2 = settings::DEFAULT_TARGET2;

  if (!settings::validTz(tz)) {
    error = "That timezone is not a valid POSIX TZ string.";
    return false;
  }
  if (!settings::validIp(target1) || !settings::validIp(target2)) {
    error = "Ping targets must be IP addresses such as 1.1.1.1";
    return false;
  }
  return true;
}

String adminFields() {
  String out;
  if (settings::hasAdmin()) {
    out += F("<p>An admin password is set. Leave both boxes empty to keep it.</p>");
  } else {
    out += F("<p>Optional. When one is set, it is needed to open Settings and to update "
             "the firmware. Updating from the dashboard stays switched off until a "
             "password is set.</p>");
  }
  out += F("<label for=\"admin\">New admin password (8 characters or more)</label>"
           "<input id=\"admin\" name=\"admin\" type=\"password\" maxlength=\"64\" "
           "autocomplete=\"new-password\">"
           "<label for=\"admin2\">The same password again</label>"
           "<input id=\"admin2\" name=\"admin2\" type=\"password\" maxlength=\"64\" "
           "autocomplete=\"new-password\">");
  if (settings::hasAdmin()) {
    out += F("<label class=\"check\"><input type=\"checkbox\" name=\"clearadmin\" "
             "value=\"1\"> Remove the admin password</label>");
  }
  return out;
}

bool readAdmin(WebServer &server, String &password, bool &change, String &error) {
  password = server.arg("admin");
  String again = server.arg("admin2");
  change = false;
  if (password.length() == 0 && again.length() == 0) {
    change = server.arg("clearadmin") == "1";
    return true;
  }
  if (password != again) {
    error = "The two admin passwords do not match.";
    return false;
  }
  if (!settings::validAdminPassword(password)) {
    error = "The admin password must be 8 to 64 characters long.";
    return false;
  }
  change = true;
  return true;
}

}  // namespace html
