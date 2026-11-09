// app.cpp - see app.h.
#include "app.h"
#include "config.h"
#include "display_config.h"
#include "draw.h"
#include "lifx.h"

#ifdef ARDUINO
  #include <Arduino.h>
#else
  #include <stdio.h>
#endif

namespace app {

namespace {

tog::Toggle   g_toggle;                 // all the animation state of screen one
ui::Controller g_ui(g_toggle);          // which screen is showing + what a finger does

uint32_t g_lastTapMs = 0;               // when you last tapped (0 = never)
bool     g_everTapped = false;
uint32_t g_lastBulbSeq = 0;             // the newest bulb answer we have already looked at
bool     g_wasMoving = false;           // was a screen change running last frame?
bool     g_lastToSliders = false;       // which way that change went

// Called when you tap the toggle: the animation has started already, the bulb follows.
void onUserTap(uint32_t nowMs) {
  g_everTapped = true;
  g_lastTapMs = nowMs;
  lifxnet::setDesired(g_toggle.theme() == tog::THEME_LIGHT);   // light mode = bulb on
}

// Keep the toggle in step with the real bulb (someone used the LIFX app, the wall switch, ...).
void followBulb(uint32_t nowMs) {
  // Order matters: ask "is my tap still on its way?" BEFORE reading the bulb's answer.
  // (The network task stores the confirmed answer first and only then clears "pending", so
  // this order can never show us "not pending" next to an old answer.)
  bool tapPending = lifxnet::writePending();
  lifxnet::Reading r = lifxnet::reading();
  if (!r.valid || r.seq == g_lastBulbSeq) return;            // nothing new to look at
  g_lastBulbSeq = r.seq;

  // Our own tap is still on its way to the bulb, or the bulb is mid-fade: its answer is
  // old news. Ignoring it stops the toggle from flipping back by itself.
  if (tapPending) return;
  if (g_everTapped && (uint32_t)(nowMs - g_lastTapMs) < LIFX_SYNC_HOLDOFF_MS) return;

  tog::Theme want = r.on ? tog::THEME_LIGHT : tog::THEME_DARK;
  if (want != g_toggle.theme()) {
#if defined(ARDUINO) && DEBUG_LOG
    Serial.printf("[sync] bulb is %s -> toggle follows\n", r.on ? "ON" : "OFF");
#endif
    g_toggle.setTheme(want);
  }
}

// The sliders moved: tell the bulb the new colour.
void sendColour() {
  lifxnet::setDesiredColor(g_ui.lifxHue(), g_ui.lifxBrightness());
}

}  // namespace

void begin(uint32_t seed) {
  g_toggle.begin(tog::THEME_DARK, seed);     // we start in dark mode; the first bulb answer moves it if the bulb is on
  gfx::Rect t = gfx::trackRect();
  ui::Rect hit = { (int16_t)(t.x - 12), (int16_t)(t.y - 12), (int16_t)(t.w + 24), (int16_t)(t.h + 24) };
  g_ui.begin(hit, 0.58f, 1.0f);              // hue starts at about 210 degrees (blue), brightness at 100%
  g_lastTapMs = 0;
  g_everTapped = false;
  g_lastBulbSeq = 0;
  g_wasMoving = false;
  gfx::invalidate();
}

bool frame(LGFX& lcd, const gesture::Event& ev, uint32_t nowMs, float dt) {
  // ---- finger ----
  ui::Actions act = g_ui.onTouch(ev);
  if (act.toggled) onUserTap(nowMs);
  if (act.colourChanged) sendColour();

  // ---- bulb ----
  followBulb(nowMs);

#if CYD_BENCH
  // Developer timing run: flip by itself every 3 s (the bulb is not told).
  static uint32_t lastAutoFlip = 0;
  if (!g_toggle.busy() && (uint32_t)(nowMs - lastAutoFlip) > 3000u) { g_toggle.flip(); lastAutoFlip = nowMs; }
#endif

  // ---- animate ----
  g_toggle.update(dt);
  g_ui.update(dt);

  // ---- draw ----
  NetStatus net = lifxnet::status();
  bool moving = g_ui.transitioning();
  bool animating = false;
  if (moving) {
    g_lastToSliders = g_ui.transitionUp();
    if (!g_wasMoving) gfx::beginTransition(g_lastToSliders);
    gfx::renderTransition(lcd, g_lastToSliders, g_ui.progress(), g_toggle, g_ui.hue(), g_ui.brightness(), net, nowMs);
    animating = true;
  } else {
    if (g_wasMoving) {                       // the change just finished: paint what is left of it, then hand over
      gfx::renderTransition(lcd, g_lastToSliders, 1.0f, g_toggle, g_ui.hue(), g_ui.brightness(), net, nowMs);
      gfx::endTransition(g_lastToSliders);
    }
    if (g_ui.screen() == ui::Screen::Toggle) {
      animating = g_toggle.busy();
      gfx::render(lcd, g_toggle, net, nowMs);
    } else {
      animating = g_ui.busy();
      gfx::renderSliders(lcd, g_ui.hue(), g_ui.brightness(), net, nowMs);
    }
  }
  g_wasMoving = moving;
  return animating;
}

const ui::Controller& controller() { return g_ui; }
const tog::Toggle& toggle() { return g_toggle; }

}  // namespace app
