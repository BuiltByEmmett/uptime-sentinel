#include "screens.h"

#include "board.h"

#ifdef HEADLESS

namespace screens {
void show() {}
void next() {}
void action() {}
void update() {}
}  // namespace screens

#else

#include <math.h>

#include "display.h"
#include "monitor.h"
#include "stats.h"
#include <WiFi.h>

#include "logview.h"
#include "settings.h"
#include "timekeeper.h"

namespace screens {
namespace {

using display::CENTER;
using display::Label;
using display::LEFT;
using display::RIGHT;

enum Screen : int8_t { STATUS, LATENCY, OUTAGES, UPTIME, SCREEN_COUNT };

constexpr int16_t STRIP_TOP = 103;  // divider row above the 32px bottom strip
constexpr int16_t STRIP_NAME_Y = 108;
constexpr int16_t STRIP_VALUE_Y = 122;

int8_t current = STATUS;

uint16_t causeColor(Cause c) {
  switch (c) {
    case Cause::ISP: return color::ORANGE;
    case Cause::SLOW:
    case Cause::DNS: return color::YELLOW;
    default: return color::LABEL;  // WIFI and POWER do not count against the ISP
  }
}
Label title(6, 4, 180, &Font10, CAP10, LEFT);

// Bottom strip split into equal columns: a grey name over a value.
void drawStripFrame(int columns) {
  display::hline(0, STRIP_TOP, display::WIDTH, color::DIVIDER);
  int16_t colW = display::WIDTH / columns;
  for (int i = 1; i < columns; i++) {
    display::vline(i * colW, STRIP_TOP + 1, display::HEIGHT - STRIP_TOP - 1, color::DIVIDER);
  }
}

// ---------------------------------------------------------------- STATUS

Label stWord(6, 26, 228, &Font36, CAP36, LEFT, 2);
Label stLossName(122, 88, 34, &Font10, CAP10, LEFT);
Label stLoss(160, 76, 74, &Font22, CAP22, RIGHT, 3);
Label stName[3] = {Label(1, STRIP_NAME_Y, 78, &Font10, CAP10, CENTER, 1),
                   Label(81, STRIP_NAME_Y, 78, &Font10, CAP10, CENTER, 1),
                   Label(161, STRIP_NAME_Y, 78, &Font10, CAP10, CENTER, 1)};
Label stValue[3] = {Label(1, STRIP_VALUE_Y, 78, &Font10, CAP10, CENTER),
                    Label(81, STRIP_VALUE_Y, 78, &Font10, CAP10, CENTER),
                    Label(161, STRIP_VALUE_Y, 78, &Font10, CAP10, CENTER)};
char stLatencyShown[8] = "";
bool stLatencyDrawn = false;

// Big number with a small "ms" tucked against it, drawn as one field.
void drawLatency(int16_t ms) {
  char text[8];
  if (ms < 0) strlcpy(text, "--", sizeof(text));
  else snprintf(text, sizeof(text), "%d", ms > 999 ? 999 : ms);
  if (stLatencyDrawn && strcmp(text, stLatencyShown) == 0) return;

  const int16_t baseline = display::PAD_TOP + CAP28;
  TFT_eSprite &s = display::beginField(display::scaled(112), baseline + display::scaled(3));
  s.setTextDatum(L_BASELINE);
  s.setFreeFont(&Font28);
  s.setTextColor(color::TEXT);
  int16_t w = s.textWidth(text);
  s.drawString(text, 0, baseline);
  s.setFreeFont(&Font10);
  s.setTextColor(color::LABEL);
  s.drawString("ms", w + display::scaled(5), baseline);
  display::endField(display::px(6), display::py(70) - display::PAD_TOP);

  strlcpy(stLatencyShown, text, sizeof(stLatencyShown));
  stLatencyDrawn = true;
}

void enterStatus() {
  drawStripFrame(3);
  stWord.invalidate();
  stLossName.invalidate();
  stLoss.invalidate();
  for (int i = 0; i < 3; i++) {
    stName[i].invalidate();
    stValue[i].invalidate();
  }
  stLatencyDrawn = false;
}

void updateStatus() {
  const char *const NAMES[3] = {"ROUTER", settings::targetLabel(0), settings::targetLabel(1)};
  const monitor::State &m = monitor::state();
  title.set("STATUS", color::LABEL);

  uint16_t wordColor = color::GREEN;
  if (!m.haveData) wordColor = color::LABEL;
  else if (m.outage) wordColor = color::ORANGE;
  else if (m.degraded) wordColor = color::YELLOW;
  stWord.set(monitor::stateName(), wordColor);

  drawLatency(m.haveData ? m.internetMs : -1);

  monitor::Summary sum = monitor::summarize(cfg::ROUNDS_5MIN);
  char text[12];
  snprintf(text, sizeof(text), "%d%%", sum.lossPct);
  stLossName.set("LOSS", color::LABEL);
  stLoss.set(text, sum.lossPct > 0 ? color::ORANGE : color::TEXT);

  for (int i = 0; i < 3; i++) {
    stName[i].set(NAMES[i], color::LABEL);
    if (m.haveData && m.rtt[i] >= 0) {
      snprintf(text, sizeof(text), "%dms", m.rtt[i]);
      stValue[i].set(text, color::TEXT);
    } else {
      stValue[i].set("--", m.haveData ? color::ORANGE : color::LABEL);
    }
  }
}

// --------------------------------------------------------------- LATENCY

// 46 bars across a 228-unit-wide, 72-unit-tall area. At 240x135 that is
// exactly 3px bars with 2px gaps; larger screens get wider bars. These are
// real pixels, worked out from the grid so the graph stays centred.
constexpr int BARS = 46;
constexpr int16_t BAR_GAP = 2;
constexpr int16_t BAR_PITCH = (display::scaled(228) + BAR_GAP) / BARS;
constexpr int16_t BAR_W = BAR_PITCH - BAR_GAP;
constexpr int16_t GRAPH_W = BARS * BAR_PITCH - BAR_GAP;
constexpr int16_t GRAPH_X = display::px(6) + (display::scaled(228) - GRAPH_W) / 2;
constexpr int16_t GRAPH_TOP = display::py(24);
constexpr int16_t GRAPH_H = display::py(96) - display::py(24);

enum BarKind : uint8_t { BAR_EMPTY, BAR_OK, BAR_FAIL };

bool hourRange = false;
int16_t barHeight[BARS];
uint8_t barKind[BARS];
bool barsDrawn = false;

Label grName[4] = {Label(1, STRIP_NAME_Y, 58, &Font10, CAP10, CENTER, 1),
                   Label(61, STRIP_NAME_Y, 58, &Font10, CAP10, CENTER, 1),
                   Label(121, STRIP_NAME_Y, 58, &Font10, CAP10, CENTER, 1),
                   Label(181, STRIP_NAME_Y, 58, &Font10, CAP10, CENTER, 1)};
Label grValue[4] = {Label(1, STRIP_VALUE_Y, 58, &Font10, CAP10, CENTER),
                    Label(61, STRIP_VALUE_Y, 58, &Font10, CAP10, CENTER),
                    Label(121, STRIP_VALUE_Y, 58, &Font10, CAP10, CENTER),
                    Label(181, STRIP_VALUE_Y, 58, &Font10, CAP10, CENTER)};

void enterLatency() {
  drawStripFrame(4);
  display::tft().drawFastHLine(GRAPH_X, GRAPH_TOP + GRAPH_H, GRAPH_W, color::DIVIDER);
  barsDrawn = false;
  for (int i = 0; i < 4; i++) {
    grName[i].invalidate();
    grValue[i].invalidate();
  }
}

void setMs(Label &label, int16_t ms) {
  char text[12];
  if (ms < 0) {
    label.set("--", color::LABEL);
  } else {
    snprintf(text, sizeof(text), "%dms", ms);
    label.set(text, color::TEXT);
  }
}

void updateLatency() {
  static const char *const NAMES[4] = {"MIN", "AVG", "MAX", "LOSS"};
  const size_t range = hourRange ? cfg::HISTORY_ROUNDS : cfg::ROUNDS_5MIN;
  title.set(hourRange ? "LATENCY 1 HOUR" : "LATENCY 5 MIN", color::LABEL);

  // Bucket the range into BARS columns, oldest on the left.
  int16_t avg[BARS];
  uint8_t kind[BARS];
  int16_t peak = 0;
  size_t have = monitor::historyCount();
  for (int j = 0; j < BARS; j++) {
    size_t ageFrom = (BARS - 1 - j) * range / BARS;
    size_t ageTo = (BARS - j) * range / BARS;
    uint32_t sum = 0, ok = 0;
    bool failed = false, any = false;
    for (size_t age = ageFrom; age < ageTo && age < have; age++) {
      uint16_t v = monitor::historyAt(age);
      any = true;
      if (v == monitor::HIST_FAIL) failed = true;
      else {
        sum += v;
        ok++;
      }
    }
    kind[j] = !any ? BAR_EMPTY : (failed ? BAR_FAIL : BAR_OK);
    avg[j] = ok ? sum / ok : 0;
    if (kind[j] == BAR_OK && avg[j] > peak) peak = avg[j];
  }

  // Scale to the tallest bar, rounded up to a multiple of 20ms.
  int16_t scale = ((peak + 19) / 20) * 20;
  if (scale < 20) scale = 20;

  TFT_eSPI &tft = display::tft();
  for (int j = 0; j < BARS; j++) {
    int16_t h = 0;
    if (kind[j] == BAR_FAIL) h = GRAPH_H;
    else if (kind[j] == BAR_OK) {
      h = static_cast<int32_t>(avg[j]) * GRAPH_H / scale;
      if (h < 1) h = 1;
      if (h > GRAPH_H) h = GRAPH_H;
    }
    if (barsDrawn && h == barHeight[j] && kind[j] == barKind[j]) continue;
    int16_t x = GRAPH_X + j * BAR_PITCH;
    // Two fills, empty part then bar, so the column never blinks to blank.
    if (h < GRAPH_H) tft.fillRect(x, GRAPH_TOP, BAR_W, GRAPH_H - h, color::BG);
    if (h > 0) {
      tft.fillRect(x, GRAPH_TOP + GRAPH_H - h, BAR_W, h,
                   kind[j] == BAR_FAIL ? color::ORANGE : color::GREEN);
    }
    barHeight[j] = h;
    barKind[j] = kind[j];
  }
  barsDrawn = true;

  monitor::Summary sum = monitor::summarize(range);
  for (int i = 0; i < 4; i++) grName[i].set(NAMES[i], color::LABEL);
  setMs(grValue[0], sum.minMs);
  setMs(grValue[1], sum.avgMs);
  setMs(grValue[2], sum.maxMs);
  if (sum.samples == 0) {
    grValue[3].set("--", color::LABEL);
  } else {
    char text[8];
    snprintf(text, sizeof(text), "%d%%", sum.lossPct);
    grValue[3].set(text, sum.lossPct > 0 ? color::ORANGE : color::TEXT);
  }
}

// --------------------------------------------------------------- OUTAGES

constexpr int ROWS = 4;
constexpr int16_t ROW_TOP = 20;
constexpr int16_t ROW_H = 28;

size_t scroll = 0;
bool ouFrameDrawn = false;   // row dividers are on screen
bool ouEmptyDrawn = false;   // the "nothing yet" notice is on screen

Label ouEmpty(6, 62, 228, &Font10, CAP10, CENTER);
Label ouStamp[ROWS] = {Label(6, ROW_TOP + 9, 92, &Font10, CAP10, LEFT),
                       Label(6, ROW_TOP + ROW_H + 9, 92, &Font10, CAP10, LEFT),
                       Label(6, ROW_TOP + ROW_H * 2 + 9, 92, &Font10, CAP10, LEFT),
                       Label(6, ROW_TOP + ROW_H * 3 + 9, 92, &Font10, CAP10, LEFT)};
Label ouDuration[ROWS] = {Label(108, ROW_TOP + 9, 72, &Font10, CAP10, LEFT),
                          Label(108, ROW_TOP + ROW_H + 9, 72, &Font10, CAP10, LEFT),
                          Label(108, ROW_TOP + ROW_H * 2 + 9, 72, &Font10, CAP10, LEFT),
                          Label(108, ROW_TOP + ROW_H * 3 + 9, 72, &Font10, CAP10, LEFT)};
Label ouTag[ROWS] = {Label(194, ROW_TOP + 9, 40, &Font10, CAP10, RIGHT),
                     Label(194, ROW_TOP + ROW_H + 9, 40, &Font10, CAP10, RIGHT),
                     Label(194, ROW_TOP + ROW_H * 2 + 9, 40, &Font10, CAP10, RIGHT),
                     Label(194, ROW_TOP + ROW_H * 3 + 9, 40, &Font10, CAP10, RIGHT)};

void invalidateRows() {
  for (int i = 0; i < ROWS; i++) {
    ouStamp[i].invalidate();
    ouDuration[i].invalidate();
    ouTag[i].invalidate();
  }
}

void enterOutages() {
  ouFrameDrawn = false;
  ouEmptyDrawn = false;
  ouEmpty.invalidate();
  invalidateRows();
}

void updateOutages() {
  size_t total = logview::count();
  if (scroll >= total) scroll = 0;

  char text[32];
  if (total > ROWS) {
    size_t last = scroll + ROWS < total ? scroll + ROWS : total;
    snprintf(text, sizeof(text), "OUTAGES %u-%u/%u", static_cast<unsigned>(scroll + 1),
             static_cast<unsigned>(last), static_cast<unsigned>(total));
    title.set(text, color::LABEL);
  } else {
    title.set("OUTAGES", color::LABEL);
  }

  if (total == 0) {
    if (ouFrameDrawn) {
      display::clearBody();
      ouFrameDrawn = false;
      ouEmpty.invalidate();
    }
    ouEmpty.set("No outages logged yet", color::LABEL);
    ouEmptyDrawn = true;
    return;
  }

  if (!ouFrameDrawn) {
    if (ouEmptyDrawn) display::clearBody();
    ouEmptyDrawn = false;
    for (int i = 1; i < ROWS; i++) {
      display::hline(6, ROW_TOP + i * ROW_H - 1, 228, color::DIVIDER);
    }
    invalidateRows();
    ouFrameDrawn = true;
  }

  for (int i = 0; i < ROWS; i++) {
    size_t idx = scroll + i;
    if (idx >= total) {
      ouStamp[i].set("", color::TEXT);
      ouDuration[i].set("", color::TEXT);
      ouTag[i].set("", color::TEXT);
      continue;
    }
    logview::Row r = logview::get(idx);
    uint16_t tagColor = causeColor(r.cause);
    if (r.active && r.start == 0) strlcpy(text, "NOW", sizeof(text));
    else timekeeper::formatStamp(r.start, text, sizeof(text));
    ouStamp[i].set(text, color::TEXT);
    // Something still in progress counts up in its own colour.
    timekeeper::formatDuration(r.duration, text, sizeof(text));
    ouDuration[i].set(text, r.active ? tagColor : color::TEXT);
    ouTag[i].set(monitor::causeName(r.cause), tagColor);
  }
}

// ---------------------------------------------------------------- UPTIME

Label upName24(6, 24, 112, &Font10, CAP10, LEFT);
Label upName7(126, 24, 112, &Font10, CAP10, LEFT);
Label upPct24(6, 42, 112, &Font22, CAP22, LEFT, 3);
Label upPct7(126, 42, 112, &Font22, CAP22, LEFT, 3);
Label upRowName[3] = {Label(6, 82, 104, &Font10, CAP10, LEFT),
                      Label(6, 99, 104, &Font10, CAP10, LEFT),
                      Label(6, 116, 104, &Font10, CAP10, LEFT)};
Label upRowValue[3] = {Label(122, 82, 112, &Font10, CAP10, RIGHT),
                       Label(122, 99, 112, &Font10, CAP10, RIGHT),
                       Label(122, 116, 112, &Font10, CAP10, RIGHT)};

void enterUptime() {
  display::hline(0, 73, display::WIDTH, color::DIVIDER);
  upName24.invalidate();
  upName7.invalidate();
  upPct24.invalidate();
  upPct7.invalidate();
  for (int i = 0; i < 3; i++) {
    upRowName[i].invalidate();
    upRowValue[i].invalidate();
  }
}

void setPct(Label &label, float pct) {
  char text[12];
  if (isnan(pct)) {
    label.set("--", color::LABEL);
  } else if (pct >= 99.995f) {
    label.set("100%", color::GREEN);
  } else {
    snprintf(text, sizeof(text), "%.2f%%", pct);
    label.set(text, color::GREEN);
  }
}

void updateUptime() {
  static const char *const NAMES[3] = {"ISP OUTAGES", "LONGEST", "WATCHING FOR"};
  stats::Stats s = stats::compute();
  // The address of the web page lives here, for when uptime.local will not resolve.
  char heading[32];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(heading, sizeof(heading), "UPTIME %s", WiFi.localIP().toString().c_str());
  } else {
    strlcpy(heading, "UPTIME", sizeof(heading));
  }
  title.set(heading, color::LABEL);
  upName24.set("24 HOURS", color::LABEL);
  upName7.set("7 DAYS", color::LABEL);
  setPct(upPct24, s.pct24h);
  setPct(upPct7, s.pct7d);

  for (int i = 0; i < 3; i++) upRowName[i].set(NAMES[i], color::LABEL);
  char text[16];
  snprintf(text, sizeof(text), "%u", s.ispOutages);
  upRowValue[0].set(text, color::TEXT);
  if (s.longestSec == 0) {
    upRowValue[1].set("--", color::LABEL);
  } else {
    timekeeper::formatDuration(s.longestSec, text, sizeof(text));
    upRowValue[1].set(text, color::TEXT);
  }
  timekeeper::formatDuration(s.watchingSec, text, sizeof(text));
  upRowValue[2].set(text, color::TEXT);
}

}  // namespace

void show() {
  display::clearAll();
  display::drawHeader(current, SCREEN_COUNT);
  title.invalidate();
  switch (current) {
    case STATUS: enterStatus(); break;
    case LATENCY: enterLatency(); break;
    case OUTAGES: enterOutages(); break;
    default: enterUptime(); break;
  }
  update();
}

void next() {
  current = (current + 1) % SCREEN_COUNT;
  show();
}

void action() {
  if (current == LATENCY) {
    hourRange = !hourRange;
  } else if (current == OUTAGES) {
    scroll += ROWS;  // wraps back to the newest in updateOutages()
  }
  update();
}

void update() {
  if (display::messageShowing()) return;
  switch (current) {
    case STATUS: updateStatus(); break;
    case LATENCY: updateLatency(); break;
    case OUTAGES: updateOutages(); break;
    default: updateUptime(); break;
  }
}

}  // namespace screens

#endif  // HEADLESS
