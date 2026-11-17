// Drawing primitives for the 240x135 landscape screen. Everything that
// changes is composed off-screen and pushed in one go, and each Label
// remembers what it last showed, so unchanged pixels are never rewritten.
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

#include "config.h"
#include "fonts.h"

namespace display {

constexpr int16_t WIDTH = 240;
constexpr int16_t HEIGHT = 135;
constexpr int16_t HEADER_H = 18;
constexpr int16_t PAD_TOP = 2;  // rows above the capital height kept for tall glyphs

enum Align : uint8_t { LEFT, CENTER, RIGHT };

void begin();
TFT_eSPI &tft();

// Off-screen canvas, pre-filled with the background colour.
TFT_eSprite &beginField(int16_t w, int16_t h);
void endField(int16_t x, int16_t y);

// One line of text in a fixed box. `top` is where the top of a capital
// letter lands; `cap` is the font's capital height.
class Label {
 public:
  Label(int16_t x, int16_t top, int16_t w, const GFXfont *font, uint8_t cap, Align align,
        int8_t padBottom = -1);
  void set(const char *text, uint16_t color);
  void invalidate() { drawn_ = false; }

 private:
  int16_t x_, top_, w_;
  const GFXfont *font_;
  uint8_t cap_;
  Align align_;
  uint8_t padBottom_;
  char last_[32];
  uint16_t lastColor_;
  bool drawn_;
};

// Screen chrome: page squares and the divider under the header.
// page < 0 draws no squares.
void drawHeader(int8_t page, int8_t pageCount);
void clearBody();
void clearAll();

// Full-screen notice used during boot, setup and the wipe countdown.
void message(const char *title, const char *line1, const char *line2 = "",
             const char *line3 = "", const char *warning = "");
bool messageShowing();

}  // namespace display
