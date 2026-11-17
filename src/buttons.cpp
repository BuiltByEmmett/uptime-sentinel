#include "buttons.h"

#include "config.h"

namespace buttons {
namespace {

constexpr uint32_t DEBOUNCE_MS = 25;

struct Button {
  uint8_t pin;
  bool down;          // debounced state
  bool raw;
  uint32_t changedAt;
};

Button btn[2] = {{cfg::PIN_BTN1, false, false, 0}, {cfg::PIN_BTN2, false, false, 0}};
bool chord = false;       // both were down at some point during this press
uint32_t bothSince = 0;

// Returns true when the debounced state just went from down to up.
bool update(Button &b, uint32_t now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw) {
    b.raw = raw;
    b.changedAt = now;
  }
  if (b.raw != b.down && now - b.changedAt >= DEBOUNCE_MS) {
    b.down = b.raw;
    return !b.down;
  }
  return false;
}

}  // namespace

void begin() {
  pinMode(cfg::PIN_BTN1, INPUT_PULLUP);
  pinMode(cfg::PIN_BTN2, INPUT);  // GPIO35 is input-only; the board has its own pull-up
}

Event poll() {
  uint32_t now = millis();
  bool released1 = update(btn[0], now);
  bool released2 = update(btn[1], now);

  static bool wasBoth = false;
  bool both = btn[0].down && btn[1].down;
  if (both && !wasBoth) bothSince = now;
  if (both) chord = true;
  wasBoth = both;

  Event ev = NONE;
  if (!chord) {
    if (released1) ev = BUTTON1;
    else if (released2) ev = BUTTON2;
  }
  if (!btn[0].down && !btn[1].down) chord = false;
  return ev;
}

uint32_t bothHeldMs() {
  if (!(btn[0].down && btn[1].down)) return 0;
  uint32_t held = millis() - bothSince;
  return held ? held : 1;
}

}  // namespace buttons
