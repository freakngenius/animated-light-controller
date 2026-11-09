// touch.h - reads the finger on the resistive screen and hands the readings to the gesture
// recogniser (gesture.h), which turns them into taps, drags and swipes.
// Also holds the one-time calibration (saved in the board's flash memory).
#pragma once
#include <stdint.h>
#include "gesture.h"

class LGFX;

namespace touchui {

// Load the saved calibration, or run the on-screen calibration when
//   * there is none yet (first run), or
//   * the BOOT button (GPIO 0) is held while the splash screen shows (after reset), or
//     a finger stays on the screen during the splash, or
//   * FORCE_TOUCH_CALIBRATION is 1 in config.h.
void begin(LGFX& lcd);

// Call every loop(). Cheap: it only talks to the touch chip every ~8 ms; in between it returns
// the recogniser's "nothing new" event (gesture::Event::sample is false).
gesture::Event poll(LGFX& lcd, uint32_t nowMs);

}  // namespace touchui
