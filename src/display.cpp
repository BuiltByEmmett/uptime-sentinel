#include "display.h"

#ifdef HEADLESS

namespace display {

void begin() {}

void message(const char *title, const char *line1, const char *line2, const char *line3,
             const char *warning) {
  // Repeated calls with the same text (countdowns aside) would flood the
  // log, so only print when something changed.
  static String last;
  String text = String("[screen] ") + title + ": " + line1 + " " + line2 + " " + line3 + " " +
                warning;
  text.trim();
  if (text != last) {
    last = text;
    Serial.println(text);
  }
}

bool messageShowing() { return false; }

}  // namespace display

#else

namespace display {
namespace {

TFT_eSPI screen;
TFT_eSprite canvas(&screen);
bool canvasReady = false;
bool showingMessage = false;

Label msgTitle(6, 4, 228, &Font10, CAP10, LEFT);
Label msgLine1(6, 34, 228, &Font10, CAP10, LEFT);
Label msgLine2(6, 54, 228, &Font10, CAP10, LEFT);
Label msgLine3(6, 74, 228, &Font10, CAP10, LEFT);
Label msgWarn(6, 104, 228, &Font10, CAP10, LEFT);

// ---- screenshot capture: a 4-bit-per-pixel BMP built in memory ----------
constexpr size_t CAP_COLORS = 16;
constexpr size_t CAP_HEADER = 14 + 40 + CAP_COLORS * 4;
constexpr size_t CAP_STRIDE = ((SCREEN_W * 4 + 31) / 32) * 4;  // rows are padded to 4 bytes
constexpr size_t CAP_SIZE = CAP_HEADER + CAP_STRIDE * SCREEN_H;

uint8_t *capFile = nullptr;
bool capturing = false;
uint16_t capPalette[CAP_COLORS];
uint8_t capUsed = 0;

// The UI uses a handful of flat colours, so each one gets a palette slot the
// first time it is drawn. A seventeenth colour would get the closest slot.
uint8_t capIndex(uint16_t color) {
  for (uint8_t i = 0; i < capUsed; i++) {
    if (capPalette[i] == color) return i;
  }
  if (capUsed < CAP_COLORS) {
    capPalette[capUsed] = color;
    return capUsed++;
  }
  uint8_t best = 0;
  uint32_t bestDistance = UINT32_MAX;
  for (uint8_t i = 0; i < CAP_COLORS; i++) {
    int dr = (capPalette[i] >> 11) - (color >> 11);
    int dg = ((capPalette[i] >> 5) & 0x3F) - ((color >> 5) & 0x3F);
    int db = (capPalette[i] & 0x1F) - (color & 0x1F);
    uint32_t distance = dr * dr * 4 + dg * dg + db * db * 4;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void capFill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w <= 0 || h <= 0) return;
  uint8_t index = capIndex(color);
  for (int32_t row = y; row < y + h; row++) {
    uint8_t *line = capFile + CAP_HEADER + row * CAP_STRIDE;
    for (int32_t col = x; col < x + w; col++) {
      uint8_t &cell = line[col >> 1];
      cell = (col & 1) ? ((cell & 0xF0) | index) : ((cell & 0x0F) | (index << 4));
    }
  }
}

void put16(uint8_t *at, uint16_t v) {
  at[0] = v & 0xFF;
  at[1] = v >> 8;
}

void put32(uint8_t *at, uint32_t v) {
  put16(at, v & 0xFFFF);
  put16(at + 2, v >> 16);
}

}  // namespace

void begin() {
#if POWER_ON_PIN >= 0
  pinMode(POWER_ON_PIN, OUTPUT);
  digitalWrite(POWER_ON_PIN, HIGH);
#endif
  screen.init();
  screen.setRotation(TFT_ROTATION);
  screen.fillScreen(color::BG);
  canvas.setColorDepth(16);
}

void fillPx(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (capturing) capFill(x, y, w, h, color);
  else screen.fillRect(x, y, w, h, color);
}

// Scale the edges rather than the size, so neighbouring shapes never leave
// a gap or overlap after rounding.
void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t x0 = px(x), y0 = py(y);
  fillPx(x0, y0, px(x + w) - x0, py(y + h) - y0, color);
}

void hline(int16_t x, int16_t y, int16_t w, uint16_t color) {
  int16_t x0 = px(x);
  fillPx(x0, py(y), px(x + w) - x0, 1, color);
}

void vline(int16_t x, int16_t y, int16_t h, uint16_t color) {
  int16_t y0 = py(y);
  fillPx(px(x), y0, 1, py(y + h) - y0, color);
}

TFT_eSprite &beginField(int16_t w, int16_t h) {
  canvasReady = canvas.createSprite(w, h) != nullptr;
  if (canvasReady) canvas.fillSprite(color::BG);
  return canvas;
}

void endField(int16_t x, int16_t y) {
  if (!canvasReady) return;
  if (capturing) {
    for (int16_t row = 0; row < canvas.height(); row++) {
      for (int16_t col = 0; col < canvas.width(); col++) {
        capFill(x + col, y + row, 1, 1, canvas.readPixel(col, row));
      }
    }
  } else {
    canvas.pushSprite(x, y);
  }
  canvas.deleteSprite();
  canvasReady = false;
}

Label::Label(int16_t x, int16_t top, int16_t w, const GFXfont *font, uint8_t cap, Align align,
             int8_t padBottom)
    : x_(px(x)), top_(py(top)), w_(px(x + w) - px(x)), font_(font), cap_(cap), align_(align),
      padBottom_(padBottom < 0 ? cap / 4 + 1 : scaled(padBottom)), lastColor_(0),
      drawn_(false) {
  last_[0] = '\0';
}

void Label::set(const char *text, uint16_t color) {
  if (drawn_ && color == lastColor_ && strncmp(text, last_, sizeof(last_) - 1) == 0) return;

  TFT_eSprite &s = beginField(w_, PAD_TOP + cap_ + padBottom_);
  s.setFreeFont(font_);
  s.setTextColor(color);
  int16_t baseline = PAD_TOP + cap_;
  if (align_ == LEFT) {
    s.setTextDatum(L_BASELINE);
    s.drawString(text, 0, baseline);
  } else if (align_ == CENTER) {
    s.setTextDatum(C_BASELINE);
    s.drawString(text, w_ / 2, baseline);
  } else {
    s.setTextDatum(R_BASELINE);
    s.drawString(text, w_, baseline);
  }
  endField(x_, top_ - PAD_TOP);

  strlcpy(last_, text, sizeof(last_));
  lastColor_ = color;
  drawn_ = true;
}

void Label::redraw() {
  char text[sizeof(last_)];
  strlcpy(text, last_, sizeof(text));
  drawn_ = false;
  set(text, lastColor_);
}

void drawHeader(int8_t page, int8_t pageCount) {
  hline(0, HEADER_H - 1, WIDTH, color::DIVIDER);
  if (page < 0) return;
  // 4px squares with 4px gaps, right-aligned with a 6px margin.
  int16_t x = WIDTH - 6 - (pageCount * 4 + (pageCount - 1) * 4);
  for (int8_t i = 0; i < pageCount; i++) {
    fill(x + i * 8, 7, 4, 4, i == page ? color::TEXT : color::DIM);
  }
}

void clearBody() { fill(0, HEADER_H, WIDTH, HEIGHT - HEADER_H, color::BG); }

void clearAll() {
  screen.fillScreen(color::BG);
  showingMessage = false;
}

bool beginCapture() {
  freeCapture();
  capFile = static_cast<uint8_t *>(malloc(CAP_SIZE));
  if (!capFile) return false;
  capUsed = 0;
  capIndex(color::BG);  // slot 0, which the zeroed picture starts out as
  memset(capFile, 0, CAP_SIZE);
  capturing = true;
  return true;
}

void stopCapture() { capturing = false; }

const uint8_t *captureFile(size_t &length) {
  length = 0;
  if (!capFile) return nullptr;
  // BITMAPFILEHEADER, BITMAPINFOHEADER, then the palette as blue, green, red, 0.
  capFile[0] = 'B';
  capFile[1] = 'M';
  put32(capFile + 2, CAP_SIZE);
  put32(capFile + 10, CAP_HEADER);
  put32(capFile + 14, 40);
  put32(capFile + 18, SCREEN_W);
  put32(capFile + 22, static_cast<uint32_t>(-static_cast<int32_t>(SCREEN_H)));  // top row first
  put16(capFile + 26, 1);
  put16(capFile + 28, 4);
  put32(capFile + 34, CAP_STRIDE * SCREEN_H);
  put32(capFile + 38, 2835);  // 72 dpi
  put32(capFile + 42, 2835);
  put32(capFile + 46, CAP_COLORS);
  for (size_t i = 0; i < CAP_COLORS; i++) {
    uint16_t c = i < capUsed ? capPalette[i] : 0;
    uint8_t *entry = capFile + 54 + i * 4;
    entry[0] = (c & 0x1F) * 255 / 31;
    entry[1] = ((c >> 5) & 0x3F) * 255 / 63;
    entry[2] = (c >> 11) * 255 / 31;
    entry[3] = 0;
  }
  length = CAP_SIZE;
  return capFile;
}

void freeCapture() {
  capturing = false;
  free(capFile);
  capFile = nullptr;
}

void redrawMessage() {
  drawHeader(-1, 0);
  msgTitle.redraw();
  msgLine1.redraw();
  msgLine2.redraw();
  msgLine3.redraw();
  msgWarn.redraw();
}

void message(const char *title, const char *line1, const char *line2, const char *line3,
             const char *warning) {
  if (!showingMessage) {
    screen.fillScreen(color::BG);
    drawHeader(-1, 0);
    msgTitle.invalidate();
    msgLine1.invalidate();
    msgLine2.invalidate();
    msgLine3.invalidate();
    msgWarn.invalidate();
    showingMessage = true;
  }
  msgTitle.set(title, color::LABEL);
  msgLine1.set(line1, color::LABEL);
  msgLine2.set(line2, color::TEXT);
  msgLine3.set(line3, color::LABEL);
  msgWarn.set(warning, color::ORANGE);
}

bool messageShowing() { return showingMessage; }

}  // namespace display

#endif  // HEADLESS
