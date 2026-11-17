#include "daily.h"

#include <LittleFS.h>
#include <time.h>

#include "config.h"
#include "monitor.h"
#include "storage.h"
#include "timekeeper.h"

namespace daily {
namespace {

constexpr char PATH[] = "/days.bin";
constexpr char TMP_PATH[] = "/days.tmp";
constexpr uint32_t MAGIC = 0x59445055;  // "UPDY"
constexpr size_t MAX_QUEUE = 10;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
};

struct Span {
  uint32_t start;
  uint32_t duration;
};

Entry entries[DAYS];  // oldest first
size_t entryCount = 0;
bool ready = false;        // begin() has run and the filesystem is there
bool needRebuild = false;  // no saved calendar: build one from the outage log

// Outages wait here until the clock (and with it the timezone) is known.
Span queue[MAX_QUEUE];
size_t queueCount = 0;

// Days since 1970-01-01 for a calendar date, and back.
uint16_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<uint16_t>(era * 146097 + doe - 719468);
}

void civilFromDays(uint16_t day, int &y, int &m, int &d) {
  int z = day + 719468;
  int era = z / 146097;
  int doe = z - era * 146097;
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yoe + era * 400 + (m <= 2);
}

bool load(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  Header h;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            h.magic == MAGIC && h.version == 1 && h.count <= DAYS;
  if (ok) {
    size_t bytes = h.count * sizeof(Entry);
    ok = f.read(reinterpret_cast<uint8_t *>(entries), bytes) == bytes;
    if (ok) entryCount = h.count;
  }
  f.close();
  return ok;
}

// Temp file then rename, so a power cut leaves the old file or the new one.
bool persist() {
  if (!ready) return false;
  File f = LittleFS.open(TMP_PATH, "w");
  if (!f) return false;
  Header h = {MAGIC, 1, static_cast<uint16_t>(entryCount)};
  size_t bytes = entryCount * sizeof(Entry);
  bool ok = f.write(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            f.write(reinterpret_cast<uint8_t *>(entries), bytes) == bytes;
  f.close();
  if (!ok) return false;
  if (!LittleFS.rename(TMP_PATH, PATH)) {
    LittleFS.remove(PATH);
    ok = LittleFS.rename(TMP_PATH, PATH);
  }
  return ok;
}

// Adds to one day of a sorted list, making room by dropping the oldest day.
void addTo(Entry *list, size_t &n, size_t max, uint16_t day, uint32_t sec, uint16_t outages) {
  size_t i = 0;
  while (i < n && list[i].day < day) i++;
  if (i == n || list[i].day != day) {
    if (n == max) {
      if (i == 0) return;  // older than everything kept
      memmove(&list[0], &list[1], (n - 1) * sizeof(Entry));
      n--;
      i--;
    }
    memmove(&list[i + 1], &list[i], (n - i) * sizeof(Entry));
    list[i] = {day, 0, 0};
    n++;
  }
  list[i].outages += outages;
  list[i].downSec += sec;
  if (list[i].downSec > 86400UL + 3600UL) list[i].downSec = 86400UL + 3600UL;
}

// Splits [start, start + duration) at each local midnight.
void addSpan(Entry *list, size_t &n, size_t max, uint32_t start, uint32_t duration) {
  uint32_t end = start + duration;
  uint32_t t = start;
  bool first = true;
  for (int guard = 0; guard < 400; guard++) {
    time_t tt = t;
    struct tm tm;
    localtime_r(&tt, &tm);
    uint16_t day = daysFromCivil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    tm.tm_mday += 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    uint32_t midnight = static_cast<uint32_t>(mktime(&tm));
    if (midnight <= t) midnight = t + 86400;
    uint32_t until = end < midnight ? end : midnight;
    addTo(list, n, max, day, until - t, first ? 1 : 0);
    first = false;
    if (until >= end) break;
    t = until;
  }
}

void prune(Entry *list, size_t &n, uint16_t today) {
  size_t drop = 0;
  while (drop < n && list[drop].day + DAYS <= today) drop++;
  if (drop) {
    memmove(&list[0], &list[drop], (n - drop) * sizeof(Entry));
    n -= drop;
  }
}

void rebuild() {
  entryCount = 0;
  for (size_t i = storage::count(); i-- > 0;) {
    const storage::OutageRecord &r = storage::get(i);
    if (isIsp(static_cast<Cause>(r.cause))) addSpan(entries, entryCount, DAYS, r.start, r.duration);
  }
  prune(entries, entryCount, dayOf(timekeeper::now()));
  Serial.printf("[days] calendar built from the outage log: %u days with downtime, %s\n",
                static_cast<unsigned>(entryCount), persist() ? "saved" : "SAVE FAILED");
}

}  // namespace

uint16_t dayOf(uint32_t epoch) {
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  return daysFromCivil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

void formatDay(uint16_t day, char *out, size_t len) {
  int y, m, d;
  civilFromDays(day, y, m, d);
  snprintf(out, len, "%04d-%02d-%02d", y, m, d);
}

void begin() {
  if (!storage::available()) return;
  ready = true;
  if (!load(PATH) && !load(TMP_PATH)) needRebuild = true;
  LittleFS.remove(TMP_PATH);
  Serial.printf("[days] %u days with downtime loaded\n", static_cast<unsigned>(entryCount));
}

void loop() {
  if (!ready || !timekeeper::valid()) return;
  if (needRebuild) {
    // The log already holds everything that is waiting in the queue.
    needRebuild = false;
    queueCount = 0;
    rebuild();
    return;
  }
  if (queueCount == 0) return;
  for (size_t i = 0; i < queueCount; i++) {
    addSpan(entries, entryCount, DAYS, queue[i].start, queue[i].duration);
  }
  queueCount = 0;
  prune(entries, entryCount, dayOf(timekeeper::now()));
  if (!persist()) Serial.println("[days] SAVE FAILED");
}

void add(uint32_t start, uint32_t duration) {
  if (queueCount < MAX_QUEUE) queue[queueCount++] = {start, duration};
}

size_t collect(Entry *out, size_t max) {
  size_t n = entryCount < max ? entryCount : max;
  memcpy(out, entries + (entryCount - n), n * sizeof(Entry));
  if (!timekeeper::valid()) return n;
  uint32_t now = timekeeper::now();
  const monitor::State &m = monitor::state();
  if (m.outage && isIsp(m.cause)) {
    uint32_t start = timekeeper::toEpoch(m.outageStartMono);
    if (now > start) addSpan(out, n, max, start, now - start);
  }
  prune(out, n, dayOf(now));
  return n;
}

void simulate() {
  if (!timekeeper::valid()) return;
  static const struct {
    uint8_t ago;
    uint16_t outages;
    uint32_t sec;
  } SAMPLE[] = {{2, 1, 25},   {5, 2, 240},   {9, 1, 1500},  {10, 3, 5400}, {23, 1, 45},
                {31, 4, 900}, {47, 1, 7300}, {62, 2, 130},  {88, 1, 3000}};
  uint16_t today = dayOf(timekeeper::now());
  for (const auto &s : SAMPLE) addTo(entries, entryCount, DAYS, today - s.ago, s.sec, s.outages);
  persist();
}

void clear() {
  entryCount = 0;
  queueCount = 0;
  needRebuild = true;
}

}  // namespace daily
