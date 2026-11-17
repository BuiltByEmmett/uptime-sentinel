// The screens: STATUS, LATENCY, OUTAGES, UPTIME, and SITES when at least
// one website is configured.
#pragma once
#include <Arduino.h>

namespace screens {

void show();      // (re)draw the current screen from scratch
void next();      // button 1
void action();    // button 2: scroll the log / change the graph range
void update();    // redraw whatever changed; cheap to call often

// Draws a screen into a display capture (see display.h) for /screenshot.bmp.
// `name` is "status", "latency", "outages", "uptime" or "sites"; empty means
// whatever the panel shows right now. Returns false for an unknown name, for
// "sites" with no website configured, when memory is short, or on a build
// with no display. On success the picture is ready in
// display::captureFile(); call display::freeCapture() afterwards.
bool capture(const char *name);

}  // namespace screens
