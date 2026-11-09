// status.h - the one little enum that the network code and the drawing code share.
#pragma once
#include <stdint.h>

enum class NetStatus : uint8_t {
  WifiDown = 0,       // not connected to the router (quiet red dot)
  SearchingBulb = 1,  // Wi-Fi is fine, bulb not found / not answering (quiet amber dot)
  Online = 2          // all good (no dot drawn)
};
