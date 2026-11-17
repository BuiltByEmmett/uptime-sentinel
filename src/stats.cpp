#include "stats.h"

#include <math.h>

#include "monitor.h"
#include "settings.h"
#include "storage.h"
#include "timekeeper.h"

namespace stats {
namespace {

// Seconds of [start, end) that fall inside [from, to).
uint32_t overlap(uint32_t start, uint32_t end, uint32_t from, uint32_t to) {
  uint32_t a = start > from ? start : from;
  uint32_t b = end < to ? end : to;
  return b > a ? b - a : 0;
}

float uptimePct(uint32_t window, uint32_t now, uint32_t since) {
  uint32_t watched = now > since ? now - since : 0;
  uint32_t span = watched < window ? watched : window;
  if (span == 0) return 100.0f;
  uint32_t from = now - span;

  uint32_t down = 0;
  for (size_t i = 0; i < storage::count(); i++) {
    const storage::OutageRecord &r = storage::get(i);
    if (!isIsp(static_cast<Cause>(r.cause))) continue;
    down += overlap(r.start, r.start + r.duration, from, now);
  }
  const monitor::State &m = monitor::state();
  if (m.outage && isIsp(m.cause)) {
    down += overlap(timekeeper::toEpoch(m.outageStartMono), now, from, now);
  }
  if (down > span) down = span;
  return 100.0f * (1.0f - static_cast<float>(down) / static_cast<float>(span));
}

}  // namespace

Stats compute() {
  Stats s;
  s.clockValid = timekeeper::valid();
  s.ispOutages = 0;
  s.longestSec = 0;
  for (size_t i = 0; i < storage::count(); i++) {
    const storage::OutageRecord &r = storage::get(i);
    if (!isIsp(static_cast<Cause>(r.cause))) continue;
    s.ispOutages++;
    if (r.duration > s.longestSec) s.longestSec = r.duration;
  }

  uint32_t since = settings::since();
  if (s.clockValid && since != 0) {
    uint32_t now = timekeeper::now();
    s.pct24h = uptimePct(24UL * 3600, now, since);
    s.pct7d = uptimePct(7UL * 24 * 3600, now, since);
    s.watchingSec = now > since ? now - since : 0;
  } else {
    s.pct24h = NAN;
    s.pct7d = NAN;
    s.watchingSec = timekeeper::mono();
  }
  return s;
}

}  // namespace stats
