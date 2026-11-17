#include "storage.h"

#include <LittleFS.h>

#include "monitor.h"
#include "timekeeper.h"

namespace storage {
namespace {

constexpr char LOG_PATH[] = "/outages.bin";
constexpr char TMP_PATH[] = "/outages.tmp";
constexpr char OPEN_PATH[] = "/open.bin";
constexpr char ALIVE_PATH[] = "/alive.bin";
constexpr uint32_t LOG_MAGIC = 0x474C5055;   // "UPLG"
constexpr uint32_t OPEN_MAGIC = 0x4E504F55;  // "UOPN"
constexpr uint32_t MARKER_REFRESH_SEC = 60;
constexpr size_t MAX_PENDING = 8;

struct LogHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
};

// Written when an outage starts and refreshed while it lasts, so an outage
// that is cut short by a power loss can still be logged on the next boot.
struct OpenMarker {
  uint32_t magic;
  uint32_t start;
  uint32_t lastSeen;
  uint8_t cause;
  uint8_t reserved[3];
};

struct Pending {
  Cause cause;
  uint32_t startMono;
  uint32_t endMono;
};

bool mounted = false;
OutageRecord records[cfg::LOG_CAPACITY];  // oldest first
size_t recordCount = 0;

Pending pending[MAX_PENDING];
size_t pendingCount = 0;

bool markerActive = false;
OpenMarker marker;
uint32_t markerWrittenMono = 0;

// Heartbeat: the last time the monitor knew it was running.
uint32_t lastAliveEpoch = 0;   // read at boot, 0 = none
bool powerGapChecked = false;
uint32_t aliveWrittenMono = 0;

bool loadFrom(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  LogHeader h;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            h.magic == LOG_MAGIC && h.version == 1 && h.count <= cfg::LOG_CAPACITY;
  if (ok) {
    size_t bytes = h.count * sizeof(OutageRecord);
    ok = f.read(reinterpret_cast<uint8_t *>(records), bytes) == bytes;
    if (ok) recordCount = h.count;
  }
  f.close();
  return ok;
}

// Write the whole log to a temp file, then rename it over the real one.
bool persist() {
  if (!mounted) return false;
  File f = LittleFS.open(TMP_PATH, "w");
  if (!f) return false;
  LogHeader h = {LOG_MAGIC, 1, static_cast<uint16_t>(recordCount)};
  size_t bytes = recordCount * sizeof(OutageRecord);
  bool ok = f.write(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            f.write(reinterpret_cast<uint8_t *>(records), bytes) == bytes;
  f.close();
  if (!ok) return false;
  if (!LittleFS.rename(TMP_PATH, LOG_PATH)) {
    LittleFS.remove(LOG_PATH);
    ok = LittleFS.rename(TMP_PATH, LOG_PATH);
  }
  return ok;
}

void append(uint32_t start, uint32_t duration, Cause cause) {
  if (recordCount == cfg::LOG_CAPACITY) {
    memmove(&records[0], &records[1], (cfg::LOG_CAPACITY - 1) * sizeof(OutageRecord));
    recordCount--;
  }
  OutageRecord &r = records[recordCount++];
  memset(&r, 0, sizeof(r));
  r.start = start;
  r.duration = duration;
  r.cause = static_cast<uint8_t>(cause);

  char stamp[16], dur[16];
  timekeeper::formatStamp(start, stamp, sizeof(stamp));
  timekeeper::formatDuration(duration, dur, sizeof(dur));
  Serial.printf("[log] %s %s %s -> %s (%u stored)\n", stamp, dur, monitor::causeName(cause),
                persist() ? "saved" : "SAVE FAILED", static_cast<unsigned>(recordCount));
}

void writeMarker() {
  File f = LittleFS.open(OPEN_PATH, "w");
  if (!f) return;
  f.write(reinterpret_cast<uint8_t *>(&marker), sizeof(marker));
  f.close();
  markerWrittenMono = timekeeper::mono();
}

void recoverInterruptedOutage() {
  File f = LittleFS.open(OPEN_PATH, "r");
  if (!f) return;
  OpenMarker m;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&m), sizeof(m)) == sizeof(m) &&
            m.magic == OPEN_MAGIC && m.lastSeen >= m.start &&
            m.cause <= CAUSE_MAX;
  f.close();
  LittleFS.remove(OPEN_PATH);
  if (!ok) return;
  Serial.println("[log] recovering an outage that was interrupted by a power loss");
  append(m.start, m.lastSeen - m.start, static_cast<Cause>(m.cause));
}

void writeAlive() {
  File f = LittleFS.open(ALIVE_PATH, "w");
  if (!f) return;
  uint32_t now = timekeeper::now();
  f.write(reinterpret_cast<uint8_t *>(&now), sizeof(now));
  f.close();
  aliveWrittenMono = timekeeper::mono();
}

// Runs once, when the clock first becomes valid after boot: if the last
// heartbeat is much older than this boot, the monitor was without power.
void checkPowerGap() {
  powerGapChecked = true;
  uint32_t bootEpoch = timekeeper::toEpoch(0);
  if (lastAliveEpoch == 0 || bootEpoch <= lastAliveEpoch) return;
  uint32_t gap = bootEpoch - lastAliveEpoch;
  if (gap < cfg::POWER_GAP_MIN_SEC || gap > 366UL * 86400) return;
  append(lastAliveEpoch, gap, Cause::POWER);
}

}  // namespace

void begin() {
  mounted = LittleFS.begin(true);
  if (!mounted) {
    Serial.println("[log] LittleFS mount failed; outages will not be saved");
    return;
  }
  // The temp file only survives if power was lost between remove and rename.
  if (!loadFrom(LOG_PATH)) loadFrom(TMP_PATH);
  LittleFS.remove(TMP_PATH);
  Serial.printf("[log] %u outages loaded\n", static_cast<unsigned>(recordCount));
  recoverInterruptedOutage();

  File f = LittleFS.open(ALIVE_PATH, "r");
  if (f) {
    if (f.read(reinterpret_cast<uint8_t *>(&lastAliveEpoch), sizeof(lastAliveEpoch)) !=
        sizeof(lastAliveEpoch)) {
      lastAliveEpoch = 0;
    }
    f.close();
  }
}

void loop() {
  if (pendingCount > 0 && timekeeper::valid()) {
    for (size_t i = 0; i < pendingCount; i++) {
      const Pending &p = pending[i];
      append(timekeeper::toEpoch(p.startMono), p.endMono - p.startMono, p.cause);
    }
    pendingCount = 0;
  }
  if (mounted && timekeeper::valid()) {
    if (!powerGapChecked) {
      checkPowerGap();
      writeAlive();
    } else if (timekeeper::mono() - aliveWrittenMono >= cfg::ALIVE_INTERVAL_SEC) {
      writeAlive();
    }
  }
  if (markerActive && timekeeper::mono() - markerWrittenMono >= MARKER_REFRESH_SEC) {
    marker.lastSeen = timekeeper::now();
    writeMarker();
  }
}

void outageStarted(Cause cause, uint32_t startMono) {
  if (!mounted || !timekeeper::valid()) return;
  memset(&marker, 0, sizeof(marker));
  marker.magic = OPEN_MAGIC;
  marker.start = timekeeper::toEpoch(startMono);
  marker.lastSeen = timekeeper::now();
  marker.cause = static_cast<uint8_t>(cause);
  markerActive = true;
  writeMarker();
}

void outageEnded(Cause cause, uint32_t startMono, uint32_t endMono) {
  if (timekeeper::valid()) {
    append(timekeeper::toEpoch(startMono), endMono - startMono, cause);
  } else if (pendingCount < MAX_PENDING) {
    // No clock yet (booted without internet): keep it in RAM and write it
    // with the right timestamp as soon as NTP answers.
    pending[pendingCount++] = {cause, startMono, endMono};
    Serial.println("[log] clock not set yet; outage will be saved after time sync");
  }
  if (markerActive) {
    markerActive = false;
    LittleFS.remove(OPEN_PATH);
  }
}

size_t count() { return recordCount; }

const OutageRecord &get(size_t index) { return records[recordCount - 1 - index]; }

void clear() {
  recordCount = 0;
  persist();
}

}  // namespace storage
