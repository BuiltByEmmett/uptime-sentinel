// Timing constants and the colour palette. Board pins live in board.h.
#pragma once
#include <Arduino.h>

namespace cfg {
constexpr uint32_t PING_INTERVAL_MS = 2000;
constexpr uint32_t PING_TIMEOUT_MS = 1000;
constexpr uint8_t FAIL_ROUNDS = 3;          // consecutive failed rounds before an outage
constexpr size_t LOG_CAPACITY = 200;        // outages kept on flash
constexpr size_t HISTORY_ROUNDS = 1800;     // one hour of rounds kept in RAM
constexpr size_t ROUNDS_5MIN = 150;

constexpr uint32_t WIPE_HOLD_MS = 5000;     // wipe gesture held this long erases settings
constexpr uint32_t FIRST_JOIN_TIMEOUT_MS = 30000;

constexpr char AP_NAME[] = "UptimeMonitor-Setup";
constexpr char TIMEZONE[] = "EST5EDT,M3.2.0,M11.1.0";  // America/New_York
}  // namespace cfg

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

namespace color {
constexpr uint16_t BG = rgb565(0x0B, 0x0E, 0x11);
constexpr uint16_t TEXT = rgb565(0xE8, 0xED, 0xF2);
constexpr uint16_t LABEL = rgb565(0x9A, 0xA4, 0xAF);
constexpr uint16_t DIVIDER = rgb565(0x1C, 0x23, 0x2B);
constexpr uint16_t DIM = rgb565(0x3A, 0x44, 0x4F);     // inactive page squares
constexpr uint16_t GREEN = rgb565(0x3D, 0xDC, 0x84);
constexpr uint16_t ORANGE = rgb565(0xFF, 0x8A, 0x3D);
}  // namespace color

enum class Cause : uint8_t { ISP = 0, WIFI = 1 };
