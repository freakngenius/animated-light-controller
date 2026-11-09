// layout.h - where things sit on the second screen (320 x 240 pixels, landscape).
//
// Both draw.cpp (to paint them) and screens.cpp (to know where a finger is) read these
// numbers, so they can never disagree. No code and no hardware in here.
//
//        0                      213|214                  319
//      0 +------------------------+-----------------------+
//        |                        |        (sun icon)     |
//        |                        |        +-------+      |
//        |                        |        | meter |      |   brightness slider:
//        |     HUE SLIDER         |        | cells |      |   the right third
//        |   [ rainbow bar  ]     |        |       |      |   (x 214 .. 319)
//        |    ^ knob              |        +-------+      |
//        |                        |       (moon icon)     |
//    239 +------------------------+-----------------------+
//          the left two thirds        swipe down on the left two thirds goes back
#pragma once
#include <stdint.h>

namespace layout {

constexpr int SCR_W = 320;
constexpr int SCR_H = 240;

// The brightness slider owns everything from this x to the right edge. Swipe down only
// works to the left of it.
constexpr int SPLIT_X = 214;

// ---- hue slider (left two thirds) ----
constexpr int HUE_FRAME_X = 8,   HUE_FRAME_Y = 92,  HUE_FRAME_W = 198, HUE_FRAME_H = 56;   // the dark pixel frame
constexpr int HUE_CELLS = 26;                                                                // coloured pixel cells
constexpr int HUE_CELL_PITCH = 7;                                                            // 6 px cell + 1 px gap
constexpr int HUE_CELL_X0 = 16, HUE_CELL_Y = 102, HUE_CELL_H = 36;
constexpr int HUE_CY = HUE_CELL_Y + HUE_CELL_H / 2;                                          // knob centre line (120)
constexpr int HUE_TRAVEL_X0 = 25, HUE_TRAVEL_X1 = 189;   // knob centre x at value 0 and at value 1
constexpr int HUE_HIT_Y0 = 56,    HUE_HIT_Y1 = 184;      // a touch that starts in this band (and left of SPLIT_X) can grab the hue knob

// ---- brightness slider (right third) ----
constexpr int BRI_FRAME_X = 247, BRI_FRAME_Y = 24, BRI_FRAME_W = 42, BRI_FRAME_H = 192;
constexpr int BRI_CELLS = 15;                                                                // meter cells
constexpr int BRI_CELL_PITCH = 12;                                                           // 10 px cell + 2 px gap
constexpr int BRI_CELL_X0 = 253, BRI_CELL_W = 30, BRI_CELL_Y0 = 29;                          // top of the top cell
constexpr int BRI_CX = BRI_FRAME_X + BRI_FRAME_W / 2;                                        // 268
constexpr int BRI_TRAVEL_Y_TOP = 34, BRI_TRAVEL_Y_BOT = 202;   // knob centre y at value 1 and at value 0

// ---- value <-> pixel ----
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline float hueValueFromX(int x)  { return clamp01((float)(x - HUE_TRAVEL_X0) / (float)(HUE_TRAVEL_X1 - HUE_TRAVEL_X0)); }
inline float briValueFromY(int y)  { return clamp01((float)(BRI_TRAVEL_Y_BOT - y) / (float)(BRI_TRAVEL_Y_BOT - BRI_TRAVEL_Y_TOP)); }
inline float hueKnobX(float v)     { return HUE_TRAVEL_X0 + v * (float)(HUE_TRAVEL_X1 - HUE_TRAVEL_X0); }
inline float briKnobY(float v)     { return BRI_TRAVEL_Y_BOT - v * (float)(BRI_TRAVEL_Y_BOT - BRI_TRAVEL_Y_TOP); }

// ---- what the sliders mean for the bulb ----
// Slider value 0..1 -> hue in degrees 0..360, and on the LIFX scale 0..65535.
inline float    hueDegrees(float v) { return clamp01(v) * 360.0f; }
inline uint16_t lifxHue(float v)    { return (uint16_t)(clamp01(v) * 65535.0f + 0.5f); }
// The brightness slider runs from 1% to 100% (a bulb at 0% is "off", and power belongs to screen one).
inline float    brightnessFraction(float v) { return 0.01f + 0.99f * clamp01(v); }
inline uint16_t lifxBrightness(float v)     { return (uint16_t)(brightnessFraction(v) * 65535.0f + 0.5f); }

}  // namespace layout
