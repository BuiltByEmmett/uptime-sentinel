#include "buttons.h"

#include "board.h"

namespace buttons {
namespace {

constexpr uint32_t DEBOUNCE_MS = 25;
constexpr uint32_t LONG_PRESS_MS = 600;
constexpr uint32_t SINGLE_WIPE_WARN_MS = 2000;
constexpr bool TWO_BUTTONS = BTN2_PIN >= 0;

struct Input {
  int8_t pin;
  bool down;           // debounced state
  bool raw;
  uint32_t changedAt;
  uint32_t downAt;
  uint32_t heldFor;    // length of the press that just ended
};

Input btn1 = {BTN1_PIN, false, false, 0, 0, 0};
Input btn2 = {BTN2_PIN, false, false, 0, 0, 0};
Input touch = {TOUCH_IRQ_PIN, false, false, 0, 0, 0};

bool chord = false;      // both buttons were down at some point during this press
bool wasBoth = false;
uint32_t bothSince = 0;

// Returns true when the debounced state just went from down to up.
bool update(Input &in, uint32_t now) {
  if (in.pin < 0) return false;
  bool raw = digitalRead(in.pin) == LOW;
  if (raw != in.raw) {
    in.raw = raw;
    in.changedAt = now;
  }
  if (in.raw != in.down && now - in.changedAt >= DEBOUNCE_MS) {
    in.down = in.raw;
    if (in.down) {
      in.downAt = now;
    } else {
      in.heldFor = now - in.downAt;
      return true;
    }
  }
  return false;
}

Event shortOrLong(const Input &in, uint32_t maxMs) {
  if (in.heldFor >= maxMs) return NONE;  // that was a wipe hold that got cancelled
  return in.heldFor >= LONG_PRESS_MS ? BUTTON2 : BUTTON1;
}

}  // namespace

void begin() {
  // INPUT_PULLUP is ignored on input-only pins (34-39); those boards have
  // their own pull-up resistors.
  if (btn1.pin >= 0) pinMode(btn1.pin, INPUT_PULLUP);
  if (btn2.pin >= 0) pinMode(btn2.pin, INPUT_PULLUP);
  if (touch.pin >= 0) pinMode(touch.pin, INPUT_PULLUP);
}

Event poll() {
  uint32_t now = millis();
  bool released1 = update(btn1, now);
  bool released2 = update(btn2, now);
  bool releasedTouch = update(touch, now);

  if (releasedTouch) return shortOrLong(touch, UINT32_MAX);

  if (!TWO_BUTTONS) {
    return released1 ? shortOrLong(btn1, SINGLE_WIPE_WARN_MS) : NONE;
  }

  bool both = btn1.down && btn2.down;
  if (both && !wasBoth) bothSince = now;
  if (both) chord = true;
  wasBoth = both;

  Event ev = NONE;
  if (!chord) {
    if (released1) ev = BUTTON1;
    else if (released2) ev = BUTTON2;
  }
  if (!btn1.down && !btn2.down) chord = false;
  return ev;
}

uint32_t wipeHeldMs() {
  bool active = TWO_BUTTONS ? (btn1.down && btn2.down) : btn1.down;
  if (!active) return 0;
  uint32_t held = millis() - (TWO_BUTTONS ? bothSince : btn1.downAt);
  return held ? held : 1;
}

uint32_t wipeWarnMs() { return TWO_BUTTONS ? 600 : SINGLE_WIPE_WARN_MS; }

}  // namespace buttons
