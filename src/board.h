// Per-board settings. Every value can be overridden with a -D build flag in
// platformio.ini, so adding a board is a new [env] block and no code changes.
#pragma once

// HEADLESS            no display: serial log and Discord only
// SCREEN_W, SCREEN_H  panel size in pixels after rotation
// TFT_ROTATION        TFT_eSPI rotation (0-3); add 2 if the picture is upside down
// UI_SCALE_NUM/DEN    the UI is designed at 240x135 and drawn at NUM/DEN size
// BTN1_PIN            button 1 (active low), or -1 if the board has no button at all
// BTN2_PIN            button 2 (active low), or -1 if the board has one button
// TOUCH_IRQ_PIN       XPT2046 touch interrupt pin (active low), or -1
// POWER_ON_PIN        pin that must be driven high to keep the screen powered, or -1
// HAS_ETH             the board has a wired network port (see net.h). Its pins come
//                     from the board definition, or from ETH_PHY_* build flags.

#ifndef SCREEN_W
#define SCREEN_W 240
#endif
#ifndef SCREEN_H
#define SCREEN_H 135
#endif
#ifndef TFT_ROTATION
#define TFT_ROTATION 1
#endif
#ifndef UI_SCALE_NUM
#define UI_SCALE_NUM 1
#endif
#ifndef UI_SCALE_DEN
#define UI_SCALE_DEN 1
#endif
#ifndef BTN1_PIN
#define BTN1_PIN 0
#endif
#ifndef BTN2_PIN
#define BTN2_PIN -1
#endif
#ifndef TOUCH_IRQ_PIN
#define TOUCH_IRQ_PIN -1
#endif
#ifndef POWER_ON_PIN
#define POWER_ON_PIN -1
#endif
