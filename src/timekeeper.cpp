#include "timekeeper.h"

#include <esp_timer.h>
#include <time.h>

#include "config.h"

namespace timekeeper {

void begin() {
  // SNTP keeps retrying in the background, so booting with no internet is
  // fine: the clock becomes valid whenever the first reply arrives.
  configTzTime(cfg::TIMEZONE, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
}

bool valid() { return time(nullptr) > 1700000000; }

uint32_t now() { return static_cast<uint32_t>(time(nullptr)); }

uint32_t mono() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

uint32_t toEpoch(uint32_t monoSec) { return now() - (mono() - monoSec); }

void formatStamp(uint32_t epoch, char *out, size_t len) {
  if (epoch == 0) {
    strlcpy(out, "--/-- --:--", len);
    return;
  }
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  snprintf(out, len, "%02d/%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void formatClock(uint32_t epoch, char *out, size_t len) {
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  snprintf(out, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void formatDuration(uint32_t s, char *out, size_t len) {
  if (s < 60) {
    snprintf(out, len, "%us", static_cast<unsigned>(s));
  } else if (s < 3600) {
    snprintf(out, len, "%um %us", static_cast<unsigned>(s / 60), static_cast<unsigned>(s % 60));
  } else if (s < 86400) {
    snprintf(out, len, "%uh %um", static_cast<unsigned>(s / 3600),
             static_cast<unsigned>((s % 3600) / 60));
  } else {
    snprintf(out, len, "%ud %uh", static_cast<unsigned>(s / 86400),
             static_cast<unsigned>((s % 86400) / 3600));
  }
}

}  // namespace timekeeper
