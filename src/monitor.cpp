#include "monitor.h"

namespace monitor {
namespace {

State st;
Handlers handlers = {nullptr, nullptr};

uint16_t history[cfg::HISTORY_ROUNDS];
size_t histHead = 0;   // next write position
size_t histCount = 0;

// ---- outage tracking (ISP / WIFI)
uint8_t failStreak = 0;      // consecutive failed rounds while no outage is declared
uint32_t streakStart = 0;
uint8_t switchStreak = 0;    // consecutive rounds whose cause differs from the declared one
uint32_t switchStart = 0;

// ---- degraded tracking (SLOW / DNS)
enum class Soft : uint8_t { NONE, SLOW, DNS };
size_t windowRounds = 0;     // rounds since the window was last reset
float baseline = 0;          // slow-moving average of normal latency
bool baselineReady = false;
bool slowNow = false;        // the last-minute window currently looks slow
Soft softStreakClass = Soft::NONE;
uint8_t softStreak = 0;
uint32_t softStreakStart = 0;

Cause toCause(RoundClass c) { return c == RoundClass::WIFI ? Cause::WIFI : Cause::ISP; }

void pushHistory(int16_t internetMs) {
  history[histHead] = internetMs < 0 ? HIST_FAIL : static_cast<uint16_t>(internetMs);
  histHead = (histHead + 1) % cfg::HISTORY_ROUNDS;
  if (histCount < cfg::HISTORY_ROUNDS) histCount++;
}

void finishDegraded(uint32_t endMono, bool recovered) {
  if (!st.degraded) return;
  st.degraded = false;
  if (endMono < st.degradedStartMono) endMono = st.degradedStartMono;
  Serial.printf("[monitor] %s ended after %us\n", causeName(st.degradedCause),
                static_cast<unsigned>(endMono - st.degradedStartMono));
  if (handlers.onEnd) handlers.onEnd(st.degradedCause, st.degradedStartMono, endMono, recovered);
}

void declare(Cause cause, uint32_t startMono) {
  // An outage outranks a degraded period: close that first.
  finishDegraded(startMono, false);
  softStreak = 0;
  st.outage = true;
  st.cause = cause;
  st.outageStartMono = startMono;
  switchStreak = 0;
  Serial.printf("[monitor] %s outage declared\n", causeName(cause));
  if (handlers.onStart) handlers.onStart(cause, startMono);
}

void finish(uint32_t endMono, bool recovered) {
  st.outage = false;
  // Failed rounds from the outage must not count as "packet loss" afterwards.
  windowRounds = 0;
  slowNow = false;
  Serial.printf("[monitor] %s outage ended after %us%s\n", causeName(st.cause),
                static_cast<unsigned>(endMono - st.outageStartMono),
                recovered ? "" : " (cause changed)");
  if (handlers.onEnd) handlers.onEnd(st.cause, st.outageStartMono, endMono, recovered);
}

void trackOutage(RoundClass c, const ping::Round &r) {
  if (!st.outage) {
    if (c == RoundClass::OK) {
      failStreak = 0;
      return;
    }
    if (failStreak == 0) streakStart = r.monoSec;
    if (++failStreak >= cfg::FAIL_ROUNDS) {
      failStreak = 0;
      declare(toCause(c), streakStart);
    }
    return;
  }

  if (c == RoundClass::OK) {
    finish(r.monoSec, true);
    return;
  }

  // Still down. If the cause looks different for long enough, close this
  // outage and open a new one so each log entry carries the right tag.
  if (toCause(c) != st.cause) {
    if (switchStreak == 0) switchStart = r.monoSec;
    if (++switchStreak >= cfg::FAIL_ROUNDS) {
      uint32_t at = switchStart;
      finish(at, false);
      declare(toCause(c), at);
    }
  } else {
    switchStreak = 0;
  }
}

// Looks at the last minute of rounds and decides whether the connection is
// usable but poor: too many lost rounds, or latency far above normal.
void updateSlow(RoundClass c) {
  if (windowRounds < cfg::SLOW_WINDOW_ROUNDS) windowRounds++;
  size_t n = windowRounds < histCount ? windowRounds : histCount;
  uint32_t sum = 0, ok = 0;
  for (size_t age = 0; age < n; age++) {
    uint16_t v = historyAt(age);
    if (v == HIST_FAIL) continue;
    sum += v;
    ok++;
  }
  float avg = ok ? static_cast<float>(sum) / ok : 0;
  uint8_t lossPct = n ? (n - ok) * 100 / n : 0;

  if (!baselineReady) {
    // Learn what "normal" is from the first full, clean window.
    if (n >= cfg::SLOW_WINDOW_ROUNDS && lossPct == 0) {
      baseline = avg;
      baselineReady = true;
    }
  } else if (!slowNow && c == RoundClass::OK && st.internetMs >= 0) {
    baseline += (st.internetMs - baseline) / 256.0f;
  }
  st.baselineMs = baselineReady ? static_cast<int16_t>(baseline + 0.5f) : -1;

  if (n < cfg::SLOW_WINDOW_ROUNDS / 2) {
    slowNow = false;
    return;
  }
  // Latency must be both several times normal and clearly high in absolute
  // terms; leaving the state needs a clear improvement, to avoid flapping.
  float enterMs = baseline * 3 > baseline + 100 ? baseline * 3 : baseline + 100;
  float exitMs = baseline * 2 > baseline + 60 ? baseline * 2 : baseline + 60;
  if (!slowNow) {
    slowNow = lossPct >= cfg::SLOW_LOSS_ENTER_PCT || (baselineReady && avg > enterMs);
  } else {
    slowNow = !(lossPct <= cfg::SLOW_LOSS_EXIT_PCT && (!baselineReady || avg < exitMs));
  }
}

void trackDegraded(RoundClass c, const ping::Round &r) {
  updateSlow(c);

  Soft now = Soft::NONE;
  if (st.dnsOk == 0) now = Soft::DNS;
  else if (slowNow) now = Soft::SLOW;

  if (st.degraded) {
    Soft current = st.degradedCause == Cause::DNS ? Soft::DNS : Soft::SLOW;
    if (now == current) return;
    finishDegraded(r.monoSec, now == Soft::NONE);
    softStreak = 0;
  }

  if (now == Soft::NONE) {
    softStreak = 0;
    return;
  }
  if (now != softStreakClass) {
    softStreakClass = now;
    softStreak = 0;
  }
  if (softStreak == 0) softStreakStart = r.monoSec;
  if (++softStreak >= cfg::FAIL_ROUNDS) {
    softStreak = 0;
    st.degraded = true;
    st.degradedCause = now == Soft::DNS ? Cause::DNS : Cause::SLOW;
    st.degradedStartMono = softStreakStart;
    Serial.printf("[monitor] %s declared\n", causeName(st.degradedCause));
    if (handlers.onStart) handlers.onStart(st.degradedCause, st.degradedStartMono);
  }
}

}  // namespace

void begin(const Handlers &h) { handlers = h; }

const State &state() { return st; }

bool activeEvent(Cause &cause, uint32_t &startMono) {
  if (st.outage) {
    cause = st.cause;
    startMono = st.outageStartMono;
    return true;
  }
  if (st.degraded) {
    cause = st.degradedCause;
    startMono = st.degradedStartMono;
    return true;
  }
  return false;
}

// The rule from the spec, with one refinement: if an internet target answers,
// the round is fine even if the router itself ignored the ping, because the
// reply can only have come back through the router.
RoundClass classify(const ping::Round &r) {
  bool internet = r.rtt[ping::TARGET1] >= 0 || r.rtt[ping::TARGET2] >= 0;
  if (internet) return RoundClass::OK;
  if (r.rtt[ping::ROUTER] >= 0) return RoundClass::ISP;
  return RoundClass::WIFI;
}

const char *className(RoundClass c) {
  switch (c) {
    case RoundClass::OK: return "OK";
    case RoundClass::ISP: return "ISP-FAIL";
    default: return "WIFI-FAIL";
  }
}

const char *causeName(Cause c) {
  switch (c) {
    case Cause::ISP: return "ISP";
    case Cause::WIFI: return "WIFI";
    case Cause::SLOW: return "SLOW";
    case Cause::DNS: return "DNS";
    default: return "POWER";
  }
}

const char *stateName() {
  if (!st.haveData) return "...";
  if (st.outage) return "OFFLINE";
  if (st.degraded) return st.degradedCause == Cause::DNS ? "NO DNS" : "SLOW";
  return "ONLINE";
}

void onRound(const ping::Round &r) {
  RoundClass c = classify(r);

  int sum = 0, replies = 0;
  for (int t = ping::TARGET1; t <= ping::TARGET2; t++) {
    if (r.rtt[t] >= 0) {
      sum += r.rtt[t];
      replies++;
    }
  }
  st.internetMs = replies ? static_cast<int16_t>(sum / replies) : -1;
  for (int t = 0; t < ping::TARGETS; t++) st.rtt[t] = r.rtt[t];
  if (r.dns >= 0) st.dnsOk = r.dns;
  st.lastClass = c;
  st.haveData = true;
  pushHistory(st.internetMs);

  trackOutage(c, r);
  if (!st.outage) trackDegraded(c, r);
}

size_t historyCount() { return histCount; }

uint16_t historyAt(size_t age) {
  size_t idx = (histHead + cfg::HISTORY_ROUNDS - 1 - age) % cfg::HISTORY_ROUNDS;
  return history[idx];
}

Summary summarize(size_t rounds) {
  Summary s;
  size_t n = rounds < histCount ? rounds : histCount;
  uint32_t sum = 0, ok = 0, jitterSum = 0, jitterPairs = 0;
  int32_t previous = -1;
  for (size_t age = 0; age < n; age++) {
    uint16_t v = historyAt(age);
    if (v == HIST_FAIL) {
      previous = -1;
      continue;
    }
    if (s.minMs < 0 || v < s.minMs) s.minMs = v;
    if (v > s.maxMs) s.maxMs = v;
    sum += v;
    ok++;
    if (previous >= 0) {
      jitterSum += v > previous ? v - previous : previous - v;
      jitterPairs++;
    }
    previous = v;
  }
  s.samples = n;
  if (ok) s.avgMs = sum / ok;
  if (jitterPairs) s.jitterMs = jitterSum / jitterPairs;
  if (n) s.lossPct = static_cast<uint8_t>(((n - ok) * 100 + n / 2) / n);
  return s;
}

}  // namespace monitor
