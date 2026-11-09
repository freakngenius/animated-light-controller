// lifx.h - everything that touches Wi-Fi and the bulb, kept OFF the drawing thread.
//
// begin() starts one background task (FreeRTOS, core 0) that
//   * joins Wi-Fi and reconnects whenever it drops,
//   * finds the bulb (UDP broadcast on port 56700, or the fixed IP in config.h),
//   * asks the bulb "are you on?" every LIFX_POLL_MS,
//   * delivers your taps ("turn on/off, fade over LIFX_FADE_MS") and retries until the bulb confirms,
//   * turns a stream of colour/brightness wishes (a finger dragging a slider) into a calm trickle
//     of SetColor packets: at most 10 per second, the newest value always wins, and each final
//     value is sent three times because UDP packets can get lost.
// The animation loop never waits for any of this: it only reads a few numbers.
//
// Implemented in lifx.cpp (uses lifx_proto.h for the raw packet bytes).
#pragma once
#include <stdint.h>
#include "status.h"

namespace lifxnet {

struct Reading {
  bool     valid;   // false until the first answer from the bulb
  bool     on;      // bulb power: true = on
  uint32_t seq;     // goes up by one every time a FRESH answer arrives
  uint32_t atMs;    // millis() when that answer arrived
};

// Start Wi-Fi + LIFX background task. Returns immediately. Call AFTER gfx::begin().
void begin();

// "Make the bulb on / off." Non-blocking, can be called as fast as you like;
// only the newest request is delivered.
void setDesired(bool on);

// True while a request from setDesired() has not been confirmed by the bulb yet
// (and has not timed out after LIFX_INTENT_TIMEOUT_MS).
bool writePending();

// "Make the bulb this colour and brightness." For the colour screen's sliders.
//   hue         0..65535 on the LIFX scale (65536 would be a full 360 degrees round the colour wheel)
//   brightness  0..65535
// Saturation is always 65535 (full colour) and the fade is a short 100 ms so a moving slider
// looks smooth. Non-blocking and safe to call from the drawing loop as often as you like (even
// every frame while a finger drags): the network task sends at most one SetColor per 100 ms,
// always the NEWEST values, and keeps sending the final value two more times in case a packet
// is lost. Calling it again with the values it already has does nothing.
// It never turns the bulb on or off (use setDesired() for that). Nothing is sent while Wi-Fi is
// down or the bulb is unknown; the newest values are kept and sent as soon as it is possible.
// Nothing at all is sent unless this has been called at least once.
// Call it from ONE task only (the drawing loop): unlike setDesired() it has no compare-and-swap loop.
void setDesiredColor(uint16_t hue, uint16_t brightness);

// True from the moment setDesiredColor() records a new value until that value has been sent
// completely (the first send and both repeats). Stays true while waiting for Wi-Fi or the bulb.
bool colorPending();

// Where the connection stands (drives the quiet corner dot).
NetStatus status();

// Last power answer from the bulb.
Reading reading();

}  // namespace lifxnet
