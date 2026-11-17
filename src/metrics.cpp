#include "metrics.h"

#include <math.h>

#include "config.h"
#include "monitor.h"
#include "ping.h"
#include "settings.h"
#include "stats.h"
#include "storage.h"
#include "timekeeper.h"

namespace metrics {
namespace {

void head(String &out, const char *name, const char *type, const char *help) {
  out += "# HELP uptime_sentinel_";
  out += name;
  out += ' ';
  out += help;
  out += "\n# TYPE uptime_sentinel_";
  out += name;
  out += ' ';
  out += type;
  out += '\n';
}

// One sample line: uptime_sentinel_<name>{<labels>} <value>
void sample(String &out, const char *name, const String &labels, const String &value) {
  out += "uptime_sentinel_";
  out += name;
  if (labels.length()) out += "{" + labels + "}";
  out += ' ';
  out += value;
  out += '\n';
}

String seconds(int ms) { return String(ms / 1000.0f, 3); }

// "ISP LOCAL" -> "isp_local"
String labelValue(const char *text) {
  String out = text;
  out.toLowerCase();
  out.replace(' ', '_');
  return out;
}

}  // namespace

String render() {
  const monitor::State &m = monitor::state();
  monitor::Summary sum = monitor::summarize(cfg::ROUNDS_5MIN);
  stats::Stats s = stats::compute();
  const settings::Config &conf = settings::get();
  String out;
  out.reserve(3600);

  head(out, "info", "gauge", "Firmware version and board build; always 1.");
  sample(out, "info", String("version=\"") + cfg::VERSION + "\",board=\"" + cfg::BOARD + "\"", "1");

  head(out, "device_uptime_seconds", "counter", "Seconds since the monitor started.");
  sample(out, "device_uptime_seconds", "", String(timekeeper::mono()));

  head(out, "free_heap_bytes", "gauge", "Free memory on the monitor.");
  sample(out, "free_heap_bytes", "", String(ESP.getFreeHeap()));

  head(out, "state", "gauge", "Connection state; the current one is 1.");
  static const char *const STATES[] = {"ONLINE", "OFFLINE", "SLOW", "NO DNS", "..."};
  static const char *const LABELS[] = {"online", "offline", "slow", "no_dns", "starting"};
  for (size_t i = 0; i < 5; i++) {
    sample(out, "state", String("state=\"") + LABELS[i] + "\"",
           strcmp(monitor::stateName(), STATES[i]) == 0 ? "1" : "0");
  }

  head(out, "internet_up", "gauge", "1 when the internet is reachable, 0 during an outage.");
  sample(out, "internet_up", "", m.haveData && !m.outage ? "1" : "0");

  const String names[ping::TARGETS] = {"router", conf.target[0], conf.target[1], "isp_hop"};
  head(out, "target_up", "gauge", "1 when the target answered the latest ping.");
  for (int i = 0; i < ping::TARGETS; i++) {
    if (i == ping::HOP && !m.hopActive) continue;
    sample(out, "target_up", "target=\"" + names[i] + "\"", m.haveData && m.rtt[i] >= 0 ? "1" : "0");
  }
  head(out, "latency_seconds", "gauge", "Reply time of the latest ping; absent with no reply.");
  for (int i = 0; i < ping::TARGETS; i++) {
    if ((i == ping::HOP && !m.hopActive) || !m.haveData || m.rtt[i] < 0) continue;
    sample(out, "latency_seconds", "target=\"" + names[i] + "\"", seconds(m.rtt[i]));
  }

  head(out, "packet_loss_ratio", "gauge", "Share of internet pings lost in the last 5 minutes.");
  sample(out, "packet_loss_ratio", "", String(sum.lossPct / 100.0f, 2));

  head(out, "jitter_seconds", "gauge", "Internet latency jitter over the last 5 minutes.");
  if (sum.jitterMs >= 0) sample(out, "jitter_seconds", "", seconds(sum.jitterMs));

  head(out, "dns_up", "gauge", "1 when the latest DNS lookup worked.");
  if (m.dnsOk >= 0) sample(out, "dns_up", "", m.dnsOk ? "1" : "0");

  head(out, "uptime_ratio", "gauge", "Internet uptime, counting ISP outages only.");
  if (!isnan(s.pct24h)) {
    sample(out, "uptime_ratio", "window=\"24h\"", String(s.pct24h / 100.0f, 6));
    sample(out, "uptime_ratio", "window=\"7d\"", String(s.pct7d / 100.0f, 6));
  }

  uint16_t counts[CAUSE_MAX + 1] = {};
  for (size_t i = 0; i < storage::count(); i++) {
    uint8_t cause = storage::get(i).cause;
    if (cause <= CAUSE_MAX) counts[cause]++;
  }
  head(out, "logged_events", "gauge", "Entries in the outage log (the latest 200), by cause.");
  for (uint8_t c = 0; c <= CAUSE_MAX; c++) {
    sample(out, "logged_events",
           "cause=\"" + labelValue(monitor::causeName(static_cast<Cause>(c))) + "\"",
           String(counts[c]));
  }
  return out;
}

}  // namespace metrics
