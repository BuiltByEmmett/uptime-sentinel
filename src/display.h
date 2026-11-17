// Drawing primitives. The UI is laid out on a 240x135 grid; every coordinate
// passed in here is on that grid and is scaled and centred for the real
// panel (see UI_SCALE_NUM / UI_SCALE_DEN in board.h).
//
// Everything that changes is composed off-screen and pushed in one go, and
// each Label remembers what it last showed, so unchanged pixels are never
// rewritten and nothing flickers.
#pragma once
#include <Arduino.h>

#include "board.h"
#include "config.h"

namespace display {

void begin();

// Full-screen notice used during boot, setup and the wipe countdown.
// On headless builds this prints to serial instead.
void message(const char *title, const char *line1, const char *line2 = "",
             const char *line3 = "", const char *warning = "");
bool messageShowing();

}  // namespace display

#ifndef HEADLESS
#include <TFT_eSPI.h>

#include "fonts.h"

namespace display {

constexpr int16_t WIDTH = 240;   // design grid
constexpr int16_t HEIGHT = 135;
constexpr int16_t HEADER_H = 18;
constexpr int16_t PAD_TOP = 2;   // real pixels kept above capital height for tall glyphs

// Grid units to real pixels.
constexpr int16_t scaled(int v) { return (v * UI_SCALE_NUM + UI_SCALE_DEN / 2) / UI_SCALE_DEN; }
constexpr int16_t ORIGIN_X = (SCREEN_W - scaled(WIDTH)) / 2;
constexpr int16_t ORIGIN_Y = (SCREEN_H - scaled(HEIGHT)) / 2;
constexpr int16_t px(int x) { return ORIGIN_X + scaled(x); }
constexpr int16_t py(int y) { return ORIGIN_Y + scaled(y); }

enum Align : uint8_t { LEFT, CENTER, RIGHT };

// Shapes, in grid units. Lines are always one real pixel thick.
void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void hline(int16_t x, int16_t y, int16_t w, uint16_t color);
void vline(int16_t x, int16_t y, int16_t h, uint16_t color);

// A filled rectangle in real pixels, for things laid out to the pixel.
void fillPx(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// Off-screen canvas in real pixels, pre-filled with the background colour.
// endField() takes the real-pixel position to push it to.
TFT_eSprite &beginField(int16_t w, int16_t h);
void endField(int16_t x, int16_t y);

// One line of text in a fixed box. x, top and w are grid units; `top` is
// where the top of a capital letter lands. `cap` is the font's real capital
// height (CAPnn from fonts.h).
class Label {
 public:
  Label(int16_t x, int16_t top, int16_t w, const GFXfont *font, uint8_t cap, Align align,
        int8_t padBottom = -1);
  void set(const char *text, uint16_t color);
  void invalidate() { drawn_ = false; }
  void redraw();  // draw the same text again, whether or not it changed

 private:
  int16_t x_, top_, w_;  // real pixels
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

// Screenshots. Between beginCapture() and stopCapture() every drawing call
// lands in an off-screen picture instead of on the panel. The picture is a
// complete 16-colour BMP file, valid until freeCapture(). Nothing is read
// back from the panel: the screen is drawn a second time by the same code.
bool beginCapture();  // false when there is not enough memory
void stopCapture();
const uint8_t *captureFile(size_t &length);
void freeCapture();

// Draws the notice from message() again (into a capture, or onto the panel
// after one).
void redrawMessage();

}  // namespace display
#endif  // HEADLESS
