// cyd_pixel_toggle.ino
//
// A pixel-art dark/light toggle on the ESP32-2432S028R "Cheap Yellow Display".
// Tap it: the moon and sun swap with a spring animation, and your LIFX bulb
// fades off (dark) or on (light) over your home Wi-Fi.
// Swipe up: a second screen with a hue slider and a brightness slider for the bulb.
// Swipe down on its left two thirds: back to the toggle.
//
// How the files fit together:
//   cyd_pixel_toggle.ino  this file: start-up and the main loop
//   config.h              the settings you may want to change
//   secrets.h             your Wi-Fi name and password
//   display_config.h      how the screen and touch chip are wired (LovyanGFX)
//   app.h/.cpp            one pass of the loop: finger -> screens -> bulb -> physics -> drawing
//   screens.h/.cpp        which screen is showing and what a finger does on each (no hardware in here)
//   gesture.h/.cpp        turns touch readings into taps, drags and swipes (no hardware in here)
//   layout.h              where things sit on screen two
//   physics.h/.cpp        the springs and the pixel-cell animation (no hardware in here)
//   draw.h/.cpp           draws both screens into an off-screen buffer, sends it to the screen
//   touch.h/.cpp          reads your finger, saves the touch calibration
//   lifx.h/.cpp           Wi-Fi + LIFX bulb on a background task so the animation never waits
//   lifx_proto.h/.cpp     builds and reads the raw LIFX network packets
//
// Arduino IDE settings are listed in SETUP.md.

#include <Arduino.h>
#include "config.h"
#include "display_config.h"
#include "app.h"
#include "draw.h"
#include "touch.h"
#include "lifx.h"

static LGFX lcd;                 // the screen (and, inside it, the touch chip)

static uint32_t lastLoopUs = 0;

// Debug counters (only used when DEBUG_LOG is 1)
static uint32_t statMs = 0;
static uint32_t statFrames = 0;
static uint32_t statBusyMs = 0;   // time spent while the toggle was moving
static uint32_t lastFrameMs = 0;

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(CYD_ROTATION);
  lcd.setBrightness(CYD_BACKLIGHT);
  lcd.fillScreen(0x0000);

  // 1) Reserve the off-screen picture memory FIRST, before Wi-Fi grabs its share of RAM.
  if (!gfx::begin(lcd)) {
    lcd.setTextColor(0xFFFF, 0x0000);
    lcd.setTextSize(2);
    lcd.setCursor(10, 90);
    lcd.print("Not enough RAM for\nthe picture buffer.\nSet SPRITE_BANDS 4\nin config.h");
    while (true) delay(1000);
  }

#if DEBUG_LOG
  Serial.printf("[draw] picture buffer: %u bytes in %u strip(s)\n", (unsigned)gfx::bufferBytes(), (unsigned)gfx::bandCount());
  Serial.printf("[mem] free heap %u, biggest free block %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
#endif

  // 2) Start Wi-Fi + bulb search in the background (runs on the other CPU core).
  lifxnet::begin();

  // 3) Splash screen + touch calibration (first run, or when you ask for it).
  touchui::begin(lcd);

  // 4) Start the app. We begin in dark mode; the first bulb answer moves the toggle if the bulb is on.
  app::begin(esp_random());
  lastLoopUs = micros();
}

void loop() {
  uint32_t nowUs = micros();
  float dt = (uint32_t)(nowUs - lastLoopUs) * 1e-6f;
  lastLoopUs = nowUs;
  uint32_t nowMs = millis();

  // finger -> screens -> bulb -> physics -> drawing (see app.cpp)
  gesture::Event ev = touchui::poll(lcd, nowMs);
  bool animating = app::frame(lcd, ev, nowMs, dt);

#if DEBUG_LOG
  if (animating) { statFrames++; statBusyMs += (uint32_t)(nowMs - lastFrameMs); }
  lastFrameMs = nowMs;
  if (nowMs - statMs >= 2000) {
    if (statFrames > 0) {
      // fps = frames painted divided by the time something was actually moving
      unsigned fps = statBusyMs ? (unsigned)(statFrames * 1000UL / statBusyMs) : 0u;
      Serial.printf("[fps] %u fps while moving (%u frames in %u ms) | last frame: draw %u us, send %u us | free heap %u\n",
                    fps, (unsigned)statFrames, (unsigned)statBusyMs, (unsigned)gfx::lastDrawUs(),
                    (unsigned)gfx::lastPushUs(), (unsigned)ESP.getFreeHeap());
#if CYD_BENCH
      uint32_t lu[8], lf;
      gfx::layerStats(lu, &lf);
      if (lf > 0)
        Serial.printf("[bench] avg us per frame (%u frames): margin %u track %u shadow %u wall %u knob %u icon %u rim %u setup %u\n",
                      (unsigned)lf, (unsigned)(lu[0] / lf), (unsigned)(lu[1] / lf), (unsigned)(lu[2] / lf), (unsigned)(lu[3] / lf),
                      (unsigned)(lu[4] / lf), (unsigned)(lu[5] / lf), (unsigned)(lu[6] / lf), (unsigned)(lu[7] / lf));
#endif
    }
    statFrames = 0;
    statBusyMs = 0;
    statMs = nowMs;
  }
#endif

  // Nothing moving? Take a short nap so the CPU stays cool. Animating? Just yield to Wi-Fi tasks.
  if (animating) yield(); else delay(3);
}
