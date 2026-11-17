#include "monitor.h"

namespace monitor {
namespace {

State st;
Handlers handlers = {nullptr, nullptr};

uint16_t history[cfg::HISTORY_ROUNDS];
size_t histHead = 0;   // next write position
size_t histCount = 0;

uint8_t failStreak = 0;      // consecutive failed rounds while no outage is declared
uint32_t streakStart = 0;
uint8_t switchStreak = 0;    // consecutive rounds whose cause differs from the declared one
uint32_t switchStart = 0;

Cause toCause(RoundClass c) { return c == RoundClass::WIFI ? Cause::WIFI : Cause::ISP; }

void pushHistory(int16_t internetMs) {
  history[histHead] = internetMs < 0 ? HIST_FAIL : static_cast<uint16_t>(internetMs);
  histHead = (histHead + 1) % cfg::HISTORY_ROUNDS;
  if (histCount < cfg::HISTORY_ROUNDS) histCount++;
}

void declare(Cause cause, uint32_t startMono) {
  st.outage = true;
  st.cause = cause;
  st.outageStartMono = startMono;
  switchStreak = 0;
  Serial.printf("[monitor] %s outage declared\n", causeName(cause));
  if (handlers.onStart) handlers.onStart(cause, startMono);
}

void finish(uint32_t endMono, bool recovered) {
  st.outage = false;
  Serial.printf("[monitor] %s outage ended after %us%s\n", causeName(st.cause),
                static_cast<unsigned>(endMono - st.outageStartMono),
                recovered ? "" : " (cause changed)");
  if (handlers.onEnd) handlers.onEnd(st.cause, st.outageStartMono, endMono, recovered);
}

}  // namespace

void begin(const Handlers &h) { handlers = h; }

const State &state() { return st; }

// The rule from the spec, with one refinement: if an internet target answers,
// the round is fine even if the router itself ignored the ping, because the
// reply can only have come back through the router.
RoundClass classify(const ping::Round &r) {
  bool internet = r.rtt[ping::CLOUDFLARE] >= 0 || r.rtt[ping::GOOGLE] >= 0;
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

const char *causeName(Cause c) { return c == Cause::ISP ? "ISP" : "WIFI"; }

void onRound(const ping::Round &r) {
  RoundClass c = classify(r);

  int sum = 0, replies = 0;
  for (int t = ping::CLOUDFLARE; t <= ping::GOOGLE; t++) {
    if (r.rtt[t] >= 0) {
      sum += r.rtt[t];
      replies++;
    }
  }
  st.internetMs = replies ? static_cast<int16_t>(sum / replies) : -1;
  for (int t = 0; t < ping::TARGETS; t++) st.rtt[t] = r.rtt[t];
  st.lastClass = c;
  st.haveData = true;
  pushHistory(st.internetMs);

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

size_t historyCount() { return histCount; }

uint16_t historyAt(size_t age) {
  size_t idx = (histHead + cfg::HISTORY_ROUNDS - 1 - age) % cfg::HISTORY_ROUNDS;
  return history[idx];
}

Summary summarize(size_t rounds) {
  Summary s;
  size_t n = rounds < histCount ? rounds : histCount;
  uint32_t sum = 0, ok = 0;
  for (size_t age = 0; age < n; age++) {
    uint16_t v = historyAt(age);
    if (v == HIST_FAIL) continue;
    if (s.minMs < 0 || v < s.minMs) s.minMs = v;
    if (v > s.maxMs) s.maxMs = v;
    sum += v;
    ok++;
  }
  s.samples = n;
  if (ok) s.avgMs = sum / ok;
  if (n) s.lossPct = static_cast<uint8_t>(((n - ok) * 100 + n / 2) / n);
  return s;
}

}  // namespace monitor
