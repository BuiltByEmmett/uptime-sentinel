// Two debounced buttons. A press is reported on release, and not at all if
// both buttons were down together, so the wipe gesture never changes screens.
#pragma once
#include <Arduino.h>

namespace buttons {

enum Event : uint8_t { NONE, BUTTON1, BUTTON2 };

void begin();
Event poll();           // call often
uint32_t bothHeldMs();  // how long both buttons have been held, 0 if not

}  // namespace buttons
