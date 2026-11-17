// The four screens: STATUS, LATENCY, OUTAGES, UPTIME.
#pragma once
#include <Arduino.h>

namespace screens {

void show();      // (re)draw the current screen from scratch
void next();      // button 1
void action();    // button 2: scroll the log / change the graph range
void update();    // redraw whatever changed; cheap to call often

}  // namespace screens
