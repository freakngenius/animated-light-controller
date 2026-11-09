// touch.cpp - reads the finger on glass and feeds the gesture recogniser, plus the one-time calibration.
//
// The touch chip (XPT2046) reports raw numbers that do not line up with screen pixels
// (every board is a little different). Calibration measures the difference once:
// you touch four marks, and the result is saved in the board's flash memory, so it
// survives power-off and re-flashing of the sketch.
#include "touch.h"
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "display_config.h"

namespace touchui {

namespace {

const char*   NVS_NAMESPACE = "cydtoggle";
const char*   NVS_KEY       = "touchcal";
const uint8_t MAGIC         = 0xC7;           // marks "this blob is ours and complete"

// What we store: the 8 numbers LovyanGFX needs + which screen rotation they belong to.
struct SavedCalibration {
  uint16_t params[8];
  uint8_t  rotation;
  uint8_t  magic;
};

const uint16_t WHITE = 0xFFFF;
const uint16_t BLACK = 0x0000;
const uint16_t GREY  = 0x8410;
const uint16_t AMBER = 0xFD20;

bool loadCalibration(uint16_t out[8]) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;       // true = read-only
  SavedCalibration saved;
  size_t got = prefs.getBytes(NVS_KEY, &saved, sizeof(saved));
  prefs.end();
  if (got != sizeof(saved) || saved.magic != MAGIC || saved.rotation != CYD_ROTATION) return false;
  for (int i = 0; i < 8; i++) out[i] = saved.params[i];
  return true;
}

void saveCalibration(const uint16_t in[8]) {
  SavedCalibration saved;
  for (int i = 0; i < 8; i++) saved.params[i] = in[i];
  saved.rotation = CYD_ROTATION;
  saved.magic = MAGIC;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBytes(NVS_KEY, &saved, sizeof(saved));
  prefs.end();
}

void centred(LGFX& lcd, const char* text, int y, uint16_t colour, int size) {
  lcd.setTextSize(size);
  lcd.setTextColor(colour, BLACK);
  int w = lcd.textWidth(text);
  lcd.setCursor((lcd.width() - w) / 2, y);
  lcd.print(text);
}

// The four numbers pairs LovyanGFX returns are raw readings at the four screen corners:
// [x0,y0, x1,y1, x2,y2, x3,y3]. If the finger was already on the glass, or the taps were
// careless, the corners end up bunched together. A good calibration spreads across most of the
// chip's 0..4095 range on both axes.
bool calibrationLooksSane(const uint16_t p[8]) {
  uint16_t xmin = p[0], xmax = p[0], ymin = p[1], ymax = p[1];
  for (int i = 1; i < 4; i++) {
    uint16_t x = p[i * 2], y = p[i * 2 + 1];
    if (x < xmin) xmin = x;
    if (x > xmax) xmax = x;
    if (y < ymin) ymin = y;
    if (y > ymax) ymax = y;
  }
  return (xmax - xmin) >= 1000 && (ymax - ymin) >= 1000;
}

// Runs LovyanGFX's four-corner calibration, checks it, and saves the result.
void runCalibration(LGFX& lcd, uint16_t params[8]) {
  for (int attempt = 0; attempt < 3; attempt++) {
    lcd.fillScreen(BLACK);
    centred(lcd, "Touch calibration", 70, WHITE, 2);
    centred(lcd, "Tap the centre of each mark", 110, GREY, 1);
    centred(lcd, "that appears. Use a fingertip", 124, GREY, 1);
    centred(lcd, "or a blunt stylus.", 138, GREY, 1);
    delay(1800);

    // If a finger is still down (for example from holding the screen at start-up), its
    // position would be recorded as the first corner. Wait until the glass is free.
    int32_t tx, ty;
    if (lcd.getTouchRaw(&tx, &ty) > 0) {
      centred(lcd, "Lift your finger...", 170, AMBER, 1);
      while (lcd.getTouchRaw(&tx, &ty) > 0) delay(10);
    }
    delay(300);

    lcd.fillScreen(BLACK);
    lcd.calibrateTouch(params, WHITE, BLACK, 15);
    if (calibrationLooksSane(params)) {
      saveCalibration(params);
      lcd.fillScreen(BLACK);
      centred(lcd, "Saved. Thank you!", 108, WHITE, 2);
      delay(900);
      return;
    }
    lcd.fillScreen(BLACK);
    centred(lcd, "That did not look right.", 100, AMBER, 2);
    centred(lcd, "Let's try again.", 124, AMBER, 2);
    delay(1500);
  }
  // Three bad tries: keep using the last numbers for now but do not save them,
  // so the next start-up asks again.
}

// The recogniser (gesture.cpp) turns readings into taps and swipes; this file only reads the chip.
gesture::Recognizer g_rec;
uint32_t lastPoll = 0;

}  // namespace

void begin(LGFX& lcd) {
  pinMode(0, INPUT_PULLUP);                      // the BOOT button

  uint16_t params[8];
  bool have = loadCalibration(params);
  bool force = (FORCE_TOUCH_CALIBRATION != 0);

  // Friendly 1.6 s splash. It proves the screen works, and it is the window in which
  // you can ask for a new calibration: hold BOOT, or keep a finger on the screen.
  lcd.fillScreen(BLACK);
  centred(lcd, "PIXEL TOGGLE", 92, WHITE, 2);
  centred(lcd, have ? "hold a finger here to recalibrate touch" : "first run: touch calibration next", 126, GREY, 1);
  uint32_t t0 = millis();
  int32_t tx, ty;
  while (millis() - t0 < 1600) {
    if (digitalRead(0) == LOW) force = true;
    if (have && lcd.getTouchRaw(&tx, &ty) > 0) force = true;     // "raw" = works before calibration is loaded
    delay(10);
  }

  if (have && !force) {
    lcd.setTouchCalibrate(params);
#if DEBUG_LOG
    Serial.println("[touch] using saved calibration");
#endif
    return;
  }
  runCalibration(lcd, params);
#if DEBUG_LOG
  Serial.println("[touch] calibration done and saved");
#endif
}

gesture::Event poll(LGFX& lcd, uint32_t nowMs) {
  if (nowMs - lastPoll < 8) return g_rec.idle();   // the chip needs no more than ~120 reads per second
  lastPoll = nowMs;
  int32_t x = 0, y = 0;
  bool touched = lcd.getTouch(&x, &y) > 0;
  return g_rec.feed(nowMs, touched, (int)x, (int)y);
}

}  // namespace touchui
