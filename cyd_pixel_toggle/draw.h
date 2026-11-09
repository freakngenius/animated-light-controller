// draw.h - turns the toggle's numbers (physics.h) into pixels on the CYD screen.
//
// How it stays flicker-free: everything is drawn into an off-screen buffer (a
// "sprite") in RAM first, then the finished picture is sent to the screen in one go.
// The buffer only covers the toggle area, not the whole screen, to save memory.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "physics.h"
#include "status.h"

class LGFX;   // the screen object, defined in display_config.h

namespace gfx {

// 392 x 198 design px -> 274 x 139 screen px on the 320 x 240 screen.
constexpr float SCALE = 0.70f;

struct Rect { int16_t x, y, w, h; };

// Where the toggle track sits on the real screen (touch.cpp uses this for hit-testing).
Rect trackRect();

// Allocate the off-screen buffer(s) and paint the first frame's backdrop.
// MUST be called BEFORE Wi-Fi starts (Wi-Fi takes big chunks of RAM).
// Uses SPRITE_BANDS from config.h; if that does not fit in RAM it falls back to
// more, thinner bands (1 -> 2 -> 4) and prints what it chose.
// Returns false only if even the thinnest attempt fails.
bool begin(LGFX& lcd);

// Draw one frame and push only the part of the screen that changed.
void render(LGFX& lcd, const tog::Toggle& toggle, NetStatus net, uint32_t nowMs);

// ---- screen two: the hue slider (left two thirds) and the brightness slider (right third) ----
// Draw one frame of screen two and send only what changed. The sliders' springs come from physics.h.
void renderSliders(LGFX& lcd, const tog::Slider& hue, const tog::Slider& bri, NetStatus net, uint32_t nowMs);

// ---- the screen change: a pixel-block wipe ----
// Call beginTransition() when a change starts, then renderTransition() every frame with the eased
// progress 0..1, then (when it is over) renderTransition(... 1.0) once more and endTransition().
// `toSliders` true = going to screen two (the wave sweeps up), false = back to screen one (sweeps down).
// Each block is painted straight from the screen being entered, so nothing about the old one is needed.
void beginTransition(bool toSliders);
void renderTransition(LGFX& lcd, bool toSliders, float progress, const tog::Toggle& toggle,
                      const tog::Slider& hue, const tog::Slider& bri, NetStatus net, uint32_t nowMs);
void endTransition(bool toSliders);

// The next render() repaints and re-sends EVERYTHING (call after a full-screen
// message such as the calibration screen).
void invalidate();

// Reporting (for the Serial Monitor and the write-up).
size_t   bufferBytes();   // total RAM held by the off-screen buffer(s)
uint8_t  bandCount();     // 1, 2 or 4
uint32_t lastDrawUs();    // microseconds the last frame spent drawing into RAM
uint32_t lastPushUs();    // microseconds the last frame spent sending to the screen

// Developer timing (CYD_BENCH=1): microseconds spent in each drawing layer, summed over the
// frames painted since the last call, and how many frames that was. Resets the sums.
// Layers: 0 margin, 1 track, 2 knob shadow, 3 wall, 4 knob, 5 pixel icon, 6 rim glow, 7 set-up.
void layerStats(uint32_t out[8], uint32_t* frames);

}  // namespace gfx
