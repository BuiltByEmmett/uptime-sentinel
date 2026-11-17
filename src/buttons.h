// Debounced input. Presses are reported on release.
//
// Two-button boards: button 1 = BUTTON1, button 2 = BUTTON2, and holding
// both is the wipe gesture.
// One-button boards: a short press = BUTTON1, a longer press (over 0.6s) =
// BUTTON2, and holding it is the wipe gesture.
// Touch screens: a tap anywhere = BUTTON1, touch-and-hold = BUTTON2. Touch
// never triggers a wipe, so something resting on the screen is harmless.
#pragma once
#include <Arduino.h>

namespace buttons {

enum Event : uint8_t { NONE, BUTTON1, BUTTON2 };

void begin();
Event poll();              // call often
uint32_t wipeHeldMs();     // how long the wipe gesture has been held, 0 if not
uint32_t wipeWarnMs();     // hold time after which to show the countdown

}  // namespace buttons
