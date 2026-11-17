// Device list: what else is on the local network. Every five minutes a
// background task sends an ARP request to each address of the board's own
// subnet (a /24 at most), a few at a time, and notes who answers. For each
// device it keeps the MAC and IP address, the maker (from the MAC prefix),
// a host name when the router's DNS or the device itself gives one, a ping
// time, and when it was first and last seen.
//
// All of it stays on the device: shown on one dashboard page, never in
// /metrics, the ISP report or a screenshot. The optional new-device alert
// carries the maker and host name only.
#pragma once
#include <Arduino.h>

namespace devices {

constexpr size_t MAX_DEVICES = 64;
constexpr size_t NICK_MAX_LEN = 20;
constexpr size_t HOST_MAX_LEN = 31;
constexpr uint32_t SCAN_INTERVAL_SEC = 300;
constexpr uint32_t FIRST_SCAN_SEC = 40;  // after power-on

struct View {
  uint8_t mac[6];
  uint32_t ip;            // network byte order
  const char *vendor;     // never null
  char host[HOST_MAX_LEN + 1];
  char nick[NICK_MAX_LEN + 1];
  bool trusted;
  bool online;            // answered in one of the last two scans
  bool gateway;
  int16_t pingMs;         // -1 = does not answer pings (many phones do not)
  uint32_t firstSeen;     // unix time, 0 = before the clock was set
  uint32_t lastSeen;
};

struct Status {
  bool enabled;
  bool scanned;       // at least one scan has finished since power-on
  bool scanning;
  uint32_t lastScan;  // unix time, 0 = clock not set
  uint16_t known;
  uint16_t online;
  bool onlyGateway;   // the last scan found nothing but the router
  bool capped;        // the subnet is larger than a /24; only the board's /24 is scanned
};

void begin();  // call after storage::begin(); does nothing when switched off in Settings
Status status();
size_t count();
bool get(size_t index, View &out);

bool validNick(const String &text);  // up to 20 plain characters; empty is valid
bool parseMac(const String &hex, uint8_t mac[6]);  // twelve hex digits
// Sets a device's nickname and trusted mark. False if the MAC is not known.
bool update(const uint8_t mac[6], const String &nick, bool trusted);

// Maker for a MAC address: from the built-in table, or "Unknown".
const char *vendorOf(const uint8_t mac[6]);

// For testing from the serial console: forget one device and scan again now,
// so that it is found as new.
void simulateNew();

}  // namespace devices
