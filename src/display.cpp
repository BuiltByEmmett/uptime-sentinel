#include "display.h"

namespace display {
namespace {

TFT_eSPI screen;
TFT_eSprite canvas(&screen);
bool canvasReady = false;
bool showingMessage = false;

Label msgTitle(6, 4, 228, &Font10, 10, LEFT);
Label msgLine1(6, 34, 228, &Font10, 10, LEFT);
Label msgLine2(6, 54, 228, &Font10, 10, LEFT);
Label msgLine3(6, 74, 228, &Font10, 10, LEFT);
Label msgWarn(6, 104, 228, &Font10, 10, LEFT);

}  // namespace

void begin() {
  screen.init();
  screen.setRotation(1);  // landscape, 240 wide x 135 tall
  screen.fillScreen(color::BG);
  canvas.setColorDepth(16);
}

TFT_eSPI &tft() { return screen; }

TFT_eSprite &beginField(int16_t w, int16_t h) {
  canvasReady = canvas.createSprite(w, h) != nullptr;
  if (canvasReady) canvas.fillSprite(color::BG);
  return canvas;
}

void endField(int16_t x, int16_t y) {
  if (!canvasReady) return;
  canvas.pushSprite(x, y);
  canvas.deleteSprite();
  canvasReady = false;
}

Label::Label(int16_t x, int16_t top, int16_t w, const GFXfont *font, uint8_t cap, Align align,
             int8_t padBottom)
    : x_(x), top_(top), w_(w), font_(font), cap_(cap), align_(align),
      padBottom_(padBottom < 0 ? cap / 4 + 1 : padBottom), lastColor_(0), drawn_(false) {
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

void drawHeader(int8_t page, int8_t pageCount) {
  screen.drawFastHLine(0, HEADER_H - 1, WIDTH, color::DIVIDER);
  if (page < 0) return;
  // 4px squares with 4px gaps, right-aligned with a 6px margin.
  int16_t x = WIDTH - 6 - (pageCount * 4 + (pageCount - 1) * 4);
  for (int8_t i = 0; i < pageCount; i++) {
    screen.fillRect(x + i * 8, 7, 4, 4, i == page ? color::TEXT : color::DIM);
  }
}

void clearBody() { screen.fillRect(0, HEADER_H, WIDTH, HEIGHT - HEADER_H, color::BG); }

void clearAll() {
  screen.fillScreen(color::BG);
  showingMessage = false;
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
