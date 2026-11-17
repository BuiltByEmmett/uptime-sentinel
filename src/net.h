// The network link the monitor is using. On most boards that is WiFi and
// nothing else. Boards built with HAS_ETH also have a wired port: it is used
// whenever its cable has a link and an address, and WiFi takes over when it
// does not.
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace net {

#ifdef HAS_ETH
constexpr bool HAS_ETHERNET = true;
#else
constexpr bool HAS_ETHERNET = false;
#endif

// Starts the wired port. Does nothing on boards without one.
void beginEthernet();
// The wired port has a link and an address. Always false without one.
bool ethernetReady();
// Waits up to `ms` for ethernetReady(). Returns at once without a wired port.
bool waitEthernet(uint32_t ms);

// Remembers the WiFi network (an empty name means none) and joins it unless
// the wired port is already in use.
void beginWifi(const String &ssid, const String &pass);
bool wifiUp();

void loop();  // switches between the two links and retries WiFi

bool up();     // some link has an address
bool wired();  // the link in use, or the last one used, is the wired port
IPAddress localIP();
IPAddress gatewayIP();
IPAddress dnsIP();
String linkText();  // for the dashboard footer, e.g. "WiFi -52 dBm"

}  // namespace net
