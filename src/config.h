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

// DNS: one lookup every DNS_EVERY_ROUNDS rounds, every round while failing.
constexpr uint32_t DNS_EVERY_ROUNDS = 15;
constexpr uint32_t DNS_TIMEOUT_MS = 900;

// SLOW: judged over the last minute of rounds.
constexpr size_t SLOW_WINDOW_ROUNDS = 30;
constexpr uint8_t SLOW_LOSS_ENTER_PCT = 20;
constexpr uint8_t SLOW_LOSS_EXIT_PCT = 10;

// POWER: the monitor notes the time once a minute; a longer silence found
// at boot is logged as time spent without power.
constexpr uint32_t ALIVE_INTERVAL_SEC = 60;
constexpr uint32_t POWER_GAP_MIN_SEC = 120;

constexpr uint32_t WIPE_HOLD_MS = 5000;     // wipe gesture held this long erases settings
constexpr uint32_t FIRST_JOIN_TIMEOUT_MS = 30000;

constexpr char AP_NAME[] = "Uptime Sentinel";
constexpr char MDNS_NAME[] = "uptime";      // http://uptime.local
constexpr char RELEASES_URL[] = "https://github.com/TradeSolana/uptime-sentinel/releases";

// Which platformio.ini environment this firmware was built for. Every image
// carries BOARD_MARKER, and the update page refuses a file without the
// marker of the board it is running on.
#ifndef BOARD_ENV
#define BOARD_ENV "custom"
#endif
constexpr char BOARD[] = BOARD_ENV;
constexpr char BOARD_MARKER[] = "UPTIME-SENTINEL-BOARD:" BOARD_ENV ";";
constexpr char VERSION[] = "1.2.0";
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
constexpr uint16_t YELLOW = rgb565(0xFF, 0xD2, 0x3D);  // degraded: slow or DNS trouble
}  // namespace color

// What an entry in the outage log was. Stored on flash: never renumber.
//   ISP    router answers, internet does not          (counts against uptime)
//   WIFI   router does not answer, monitor on WiFi
//   SLOW   internet reachable but lossy or very slow
//   DNS    internet reachable but name lookups fail
//   POWER  the monitor itself was without power
//   LAN    router does not answer, monitor on a wired port
enum class Cause : uint8_t { ISP = 0, WIFI = 1, SLOW = 2, DNS = 3, POWER = 4, LAN = 5 };
constexpr uint8_t CAUSE_MAX = 5;  // highest value a stored log entry may carry
