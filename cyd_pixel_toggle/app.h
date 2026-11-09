// app.h - one pass of the main loop, after the finger has been read:
//   touch -> screens (ui::Controller) -> bulb -> physics -> drawing.
//
// This used to live in the .ino. It is here so the laptop tool cyd_sim can run exactly the same
// code as the board. The only things it needs from the outside are the screen object, the
// finger event and the clock; the bulb goes through lifx.h, which cyd_sim replaces with a fake.
#pragma once
#include <stdint.h>
#include "gesture.h"
#include "screens.h"
#include "physics.h"

class LGFX;

namespace app {

// Set everything up (call after gfx::begin()). `seed` feeds the toggle's random pixel clouds.
void begin(uint32_t seed);

// Run one loop pass. Returns true while something is animating (the sketch naps when it is not).
bool frame(LGFX& lcd, const gesture::Event& ev, uint32_t nowMs, float dtSeconds);

// What the sketch's debug counters need.
const ui::Controller& controller();
const tog::Toggle& toggle();

}  // namespace app
