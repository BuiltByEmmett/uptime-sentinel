#include "net.h"

#include <WiFi.h>
#ifdef HAS_ETH
#include <ETH.h>
#endif

namespace net {
namespace {

constexpr uint32_t RECONNECT_MS = 30000;

SemaphoreHandle_t tlsMutex = nullptr;
String wifiSsid;
String wifiPass;
bool wifiWasUp = false;
uint32_t lastKick = 0;

#ifdef HAS_ETH
volatile bool ethHasIp = false;
bool ethInUse = false;
bool lastWired = false;

void onEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_GOT_IP: ethHasIp = true; break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP: ethHasIp = false; break;
    default: break;
  }
}
#endif

void joinWifi() {
  if (wifiSsid.length() == 0) return;
  WiFi.begin(wifiSsid.c_str(), wifiPass.length() ? wifiPass.c_str() : nullptr);
  lastKick = millis();
}

}  // namespace

#ifdef HAS_ETH

void beginEthernet() {
  WiFi.onEvent(onEvent);
  // Pins and PHY type come from the board definition (ETH_PHY_* in its
  // pins_arduino.h, or -D flags in platformio.ini).
  if (!ETH.begin()) Serial.println("[eth] could not start the Ethernet port");
}

bool ethernetReady() { return ethHasIp; }

bool waitEthernet(uint32_t ms) {
  uint32_t started = millis();
  while (!ethHasIp && millis() - started < ms) delay(20);
  return ethHasIp;
}

bool wired() { return lastWired; }

#else

void beginEthernet() {}
bool ethernetReady() { return false; }
bool waitEthernet(uint32_t) { return false; }
bool wired() { return false; }

#endif

void begin() { tlsMutex = xSemaphoreCreateMutex(); }

TlsLock::TlsLock() {
  if (tlsMutex) xSemaphoreTake(tlsMutex, portMAX_DELAY);
}

TlsLock::~TlsLock() {
  if (tlsMutex) xSemaphoreGive(tlsMutex);
}

void beginWifi(const String &ssid, const String &pass) {
  wifiSsid = ssid;
  wifiPass = pass;
  if (wifiSsid.length() == 0) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep would add tens of ms to every ping
  WiFi.setAutoReconnect(true);
  if (!ethernetReady()) joinWifi();
}

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

void loop() {
#ifdef HAS_ETH
  bool eth = ethHasIp;
  if (eth != ethInUse) {
    ethInUse = eth;
    if (eth) {
      lastWired = true;
      Serial.printf("[eth] connected ip=%s gateway=%s %uMbps\n", ETH.localIP().toString().c_str(),
                    ETH.gatewayIP().toString().c_str(), static_cast<unsigned>(ETH.linkSpeed()));
      // One link at a time, so every packet leaves by the port we report.
      if (wifiSsid.length()) WiFi.disconnect();
      wifiWasUp = false;
    } else {
      Serial.println("[eth] disconnected");
      joinWifi();
    }
  }
  if (eth) return;
#endif

  bool up = wifiUp();
  if (up != wifiWasUp) {
    wifiWasUp = up;
    if (up) {
#ifdef HAS_ETH
      lastWired = false;
#endif
      Serial.printf("[wifi] connected ip=%s gateway=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                    WiFi.RSSI());
    } else {
      Serial.println("[wifi] disconnected");
      lastKick = millis();
    }
  }
  if (!up && wifiSsid.length() && millis() - lastKick >= RECONNECT_MS) {
    lastKick = millis();
    WiFi.reconnect();
  }
}

bool up() { return ethernetReady() || wifiUp(); }

IPAddress localIP() {
#ifdef HAS_ETH
  if (ethHasIp) return ETH.localIP();
#endif
  return WiFi.localIP();
}

IPAddress gatewayIP() {
#ifdef HAS_ETH
  if (ethHasIp) return ETH.gatewayIP();
#endif
  return WiFi.gatewayIP();
}

IPAddress dnsIP() {
#ifdef HAS_ETH
  if (ethHasIp) return ETH.dnsIP(0);
#endif
  return WiFi.dnsIP(0);
}

String linkText() {
#ifdef HAS_ETH
  if (ethHasIp) return "Ethernet " + String(ETH.linkSpeed()) + " Mbps";
  if (wifiSsid.length() == 0) return "Ethernet unplugged";
#endif
  return "WiFi " + String(WiFi.RSSI()) + " dBm";
}

}  // namespace net
