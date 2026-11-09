// config.h - every setting you might want to change lives in this one file.
// You normally only touch the first two lines (and maybe the LIFX section).
#pragma once

// ---------------------------------------------------------------------------
// 1) SCREEN VARIANT  <- if the colours look wrong, change this ONE number
//
//    1 = ILI9341 screen, normal colours      (most boards with ONE usb port)
//    2 = ILI9341 screen, inverted colours
//    3 = ST7789 screen, normal colours       (some boards with TWO usb ports)
//    4 = ST7789 screen, inverted colours     (common fix for the two-port boards)
//
//    Symptom -> try:  everything looks like a photo negative -> flip 1<->2 or 3<->4
//                     white screen / garbage / nothing      -> flip 1<->3 or 2<->4
// ---------------------------------------------------------------------------
#define CYD_VARIANT 3

// Orange looks blue and blue looks orange? Set to 1. (Rare. Leave at 0 first.)
#define CYD_SWAP_RED_BLUE 0

// Landscape direction. 1 or 3. If the picture is upside down, use the other one.
// After changing this, recalibrate the touch (see SETUP.md).
#define CYD_ROTATION 1

// Backlight brightness, 0 (off) to 255 (full).
#define CYD_BACKLIGHT 220

// Screen bus speed in MHz. 40 is safe. If you see sparkles, try 27.
#define CYD_SPI_MHZ 40

// ---------------------------------------------------------------------------
// 2) MEMORY: how many strips the off-screen picture is cut into.
//    1 = fastest, uses ~93 KB of RAM.   2 = ~47 KB (recommended).   4 = ~24 KB.
//    If the first number cannot be allocated, the sketch silently falls back to
//    the next bigger number and prints what it chose on the Serial Monitor.
// ---------------------------------------------------------------------------
#define SPRITE_BANDS 2

// ---------------------------------------------------------------------------
// 3) TOUCH CALIBRATION
//    The sketch calibrates by itself the very first time it runs.
//    To redo it later: press RESET, then HOLD the BOOT button (or keep a finger on
//    the screen) while the "PIXEL TOGGLE" splash is showing (about 1.6 seconds).
//    Do NOT hold BOOT while pressing RESET: that puts the chip into upload mode.
//    Or set this to 1, upload, calibrate, set it back to 0, upload again.
// ---------------------------------------------------------------------------
#define FORCE_TOUCH_CALIBRATION 0

// ---------------------------------------------------------------------------
// 4) LIFX bulb  (Wi-Fi name and password go in secrets.h, not here)
// ---------------------------------------------------------------------------
// The sketch finds your bulb automatically. If it cannot (some routers block
// broadcasts), type the bulb's IP address here, for example "192.168.1.42".
// Leave "" to use automatic discovery only.
#define LIFX_FALLBACK_IP ""

// Only needed if you own SEVERAL LIFX devices and want one specific bulb.
// 12 hex characters, no colons, for example "d073d5a1b2c3". The Serial Monitor
// lists the id of every LIFX device that answered. "" = use the first one found.
#define LIFX_TARGET_MAC ""

#define LIFX_FADE_MS            300     // fade time when the bulb turns on/off
#define LIFX_POLL_MS            3000    // how often we ask the bulb "are you on?"
#define LIFX_SYNC_HOLDOFF_MS    2500    // after you tap, ignore bulb readings this long
#define LIFX_INTENT_TIMEOUT_MS  20000   // give up delivering a tap after this long

// ---------------------------------------------------------------------------
// 5) Debug
// ---------------------------------------------------------------------------
// 1 = print frames-per-second, free memory and Wi-Fi/LIFX events to the
// Serial Monitor (115200 baud).
#define DEBUG_LOG 1

// Developer only. 1 = flip the toggle by itself every few seconds and print how long each
// drawing layer takes (microseconds per frame). Used to find what makes the animation slow.
// Leave at 0 for normal use. (Builds can also switch it on with -DCYD_BENCH=1.)
#ifndef CYD_BENCH
#define CYD_BENCH 0
#endif
