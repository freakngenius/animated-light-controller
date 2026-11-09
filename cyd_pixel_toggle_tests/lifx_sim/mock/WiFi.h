// WiFi.h - stand-in for the ESP32 Wi-Fi object (simulator only).
#pragma once
#include "Arduino.h"

typedef enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 } wl_status_t;
#define WIFI_STA 1

struct WiFiMock {
  void persistent(bool) {}
  bool mode(int) { return true; }
  bool setAutoReconnect(bool) { return true; }
  bool setSleep(bool) { return true; }
  void begin(const char* ssid, const char* pass);   // defined in sim_lifx.cpp
  bool disconnect();
  wl_status_t status();
  IPAddress localIP() { return IPAddress(192, 168, 1, 50); }
  IPAddress broadcastIP() { return IPAddress(192, 168, 1, 255); }
};
extern WiFiMock WiFi;
