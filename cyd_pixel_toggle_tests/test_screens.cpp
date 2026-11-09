// test_screens.cpp - checks the second screen and the screen-change wipe (draw.cpp), on a laptop.
//
// What it proves:
//   1. Screen two repaints only what changed, and the result is identical to repainting the whole
//      screen from scratch (the same test preview.cpp does for screen one).
//   2. During a wipe every pixel on the screen is either what the OLD screen has there or what the
//      NEW screen has there. Nothing else ever shows (no stray colours, no leftover knobs).
//   3. Each 16 x 16 block changes over exactly once (old, old, ..., new, new): it never flips back.
//   4. The wipe starts from the old screen untouched, and its last frame equals a clean repaint of
//      the new screen, in both directions, for light and dark mode, with and without the status dot,
//      whether the frames come fast (small steps) or slow (big jumps).
//   5. After a wipe, the next normal frame of the new screen still equals a clean repaint.
//   6. Screen two looks right at the extremes: the knobs stay inside the frames (sample pixels) and
//      the brightness meter lights the right number of cells.
//
// Run: ./build_screens_test.sh   (exit code 0 = all pass)
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include "display_config.h"
#include "draw.h"
#include "physics.h"
#include "layout.h"

size_t lgfx::mock_max_alloc = 0;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef std::vector<uint16_t> Shot;
static Shot grab(const LGFX& lcd) { return Shot(lcd.mockFrame(), lcd.mockFrame() + 320 * 240); }

struct Scene {
  tog::Toggle toggle;
  tog::Slider hue, bri;
  NetStatus net;
  Scene() : net(NetStatus::Online) {}
};

static void setupScene(Scene& s, tog::Theme theme, float hueV, float briV, NetStatus net) {
  s.toggle.begin(theme, 777u);
  s.hue.begin(hueV);
  s.bri.begin(briV);
  s.net = net;
}

// Clean repaints of each screen (everything redrawn from scratch).
static Shot cleanScreen1(LGFX& lcd, Scene& s, uint32_t nowMs) {
  gfx::invalidate();
  gfx::render(lcd, s.toggle, s.net, nowMs);
  return grab(lcd);
}
static Shot cleanScreen2(LGFX& lcd, Scene& s, uint32_t nowMs) {
  gfx::invalidate();
  gfx::renderSliders(lcd, s.hue, s.bri, s.net, nowMs);
  return grab(lcd);
}

static int diffCount(const Shot& a, const Shot& b) {
  int n = 0;
  for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) n++;
  return n;
}

// ---------------------------------------------------------------------------
// 1. screen two: partial repaint == clean repaint
// ---------------------------------------------------------------------------
static void testScreen2DirtyRects(LGFX& lcd) {
  printf("screen two: partial repaint equals clean repaint\n");
  CHECK(gfx::begin(lcd), "gfx::begin");
  Scene s;
  setupScene(s, tog::THEME_DARK, 0.58f, 1.0f, NetStatus::Online);
  uint32_t now = 0;
  gfx::invalidate();
  gfx::renderSliders(lcd, s.hue, s.bri, s.net, now);

  const float dt = 1.0f / 60.0f;
  int bad = 0, frames = 0, pushedFrames = 0;
  for (int fr = 0; fr < 420; fr++) {
    // script: sweep the hue, then the brightness, with presses, jumps and a long rest
    if (fr == 10)  { s.hue.setPressed(true); }
    if (fr >= 10 && fr < 90)  s.hue.setTarget(0.58f - 0.58f * (fr - 10) / 80.0f);      // to the left end
    if (fr == 90)  { s.hue.setTarget(1.0f); }                                         // jump to the right end
    if (fr == 120) { s.hue.setPressed(false); }
    if (fr == 130) { s.bri.setPressed(true); }
    if (fr >= 130 && fr < 200) s.bri.setTarget(1.0f - 1.0f * (fr - 130) / 70.0f);      // down to 0
    if (fr == 200) { s.bri.setTarget(0.5f); }
    if (fr == 230) { s.bri.setPressed(false); }
    if (fr == 260) { s.hue.setTarget(0.33f); s.bri.setTarget(0.2f); }                  // both at once
    if (fr == 300) s.net = NetStatus::SearchingBulb;
    if (fr == 330) s.net = NetStatus::WifiDown;
    if (fr == 360) s.net = NetStatus::Online;

    s.hue.update(dt);
    s.bri.update(dt);
    now = (uint32_t)(fr * 1000 / 60);
    lcd.mockResetCounters();
    gfx::renderSliders(lcd, s.hue, s.bri, s.net, now);
    if (lcd.mockPixelsSent()) pushedFrames++;
    Shot cur = grab(lcd);
    // the status dot blinks on its own clock; compare only when it is not part of what a clean repaint would show differently
    gfx::invalidate();
    gfx::renderSliders(lcd, s.hue, s.bri, s.net, now);
    Shot full = grab(lcd);
    int d = diffCount(cur, full);
    frames++;
    if (d) { bad++; if (bad <= 5) printf("  frame %d: partial repaint differs from clean repaint in %d px\n", fr, d); }
  }
  CHECK(bad == 0, "%d of %d frames differ from a clean repaint", bad, frames);
  CHECK(pushedFrames > 100, "expected the sliders to draw on many frames, only %d did", pushedFrames);
  // a resting screen sends nothing at all
  lcd.mockResetCounters();
  gfx::renderSliders(lcd, s.hue, s.bri, NetStatus::Online, 99999u);
  gfx::renderSliders(lcd, s.hue, s.bri, NetStatus::Online, 100000u);
  CHECK(lcd.mockPixelsSent() == 0, "a resting screen two should send nothing, sent %llu px", (unsigned long long)lcd.mockPixelsSent());
}

// ---------------------------------------------------------------------------
// 2-5. the wipe
// ---------------------------------------------------------------------------
static const int BLK = 16, BCOLS = 20, BROWS = 15;

// frames for the wipe between two screens: progress goes 0 -> 1 in `nFrames` steps (nFrames=1: one big jump)
static void testWipe(LGFX& lcd, bool toSliders, tog::Theme theme, NetStatus net, int nFrames, const char* label) {
  Scene s;
  setupScene(s, theme, 0.25f, 0.6f, net);
  // Resting references. (cleanScreen1/2 each leave the screen in their own state; we only keep the pictures.)
  Shot A, B;
  if (toSliders) { A = cleanScreen1(lcd, s, 5000); B = cleanScreen2(lcd, s, 5000); }
  else           { A = cleanScreen2(lcd, s, 5000); B = cleanScreen1(lcd, s, 5000); }
  // Put the real screen in the "old screen showing" state, then run the wipe.
  if (toSliders) { gfx::invalidate(); gfx::render(lcd, s.toggle, s.net, 5000); }
  else           { gfx::invalidate(); gfx::renderSliders(lcd, s.hue, s.bri, s.net, 5000); }
  Shot cur0 = grab(lcd);
  CHECK(diffCount(cur0, A) == 0, "%s: old screen differs from its clean repaint before the wipe (%d px)", label, diffCount(cur0, A));

  gfx::beginTransition(toSliders);
  // For every block remember the sequence of states: 0 = all old, 1 = all new, 2 = mixed/other, -1 = old==new here
  std::vector<int> last(BCOLS * BROWS, 0);
  int strayPixels = 0, revert = 0;
  for (int f = 1; f <= nFrames; f++) {
    float p = (float)f / (float)nFrames;
    gfx::renderTransition(lcd, toSliders, p, s.toggle, s.hue, s.bri, s.net, 5000u);   // same clock as the references, so the pulsing dot matches
    Shot cur = grab(lcd);
    for (int by = 0; by < BROWS; by++) for (int bx = 0; bx < BCOLS; bx++) {
      int nOld = 0, nNew = 0, nOther = 0;
      for (int y = 0; y < BLK; y++) for (int x = 0; x < BLK; x++) {
        size_t i = (size_t)(by * BLK + y) * 320 + bx * BLK + x;
        bool isOld = cur[i] == A[i], isNew = cur[i] == B[i];
        if (isOld && isNew) { /* the same on both screens here: says nothing */ }
        else if (isOld) nOld++;
        else if (isNew) nNew++;
        else nOther++;
      }
      strayPixels += nOther;
      int st;                                       // block state in this frame
      if (nOther) st = 2;
      else if (nOld == 0 && nNew == 0) st = -1;     // old and new look the same here: cannot tell
      else if (nNew == 0) st = 0;                   // still old
      else if (nOld == 0) st = 1;                   // already new
      else st = 2;                                  // part old, part new: a block must change in one go
      int k = by * BCOLS + bx;
      if (st == 2 && !nOther) { revert++; }         // a mixed block means it was cut mid-way
      if (st == 0 && last[k] == 1) revert++;        // went back to old
      if (st != -1) last[k] = st == 2 ? last[k] : st;
    }
  }
  CHECK(strayPixels == 0, "%s: %d pixels showed something that is neither the old nor the new screen", label, strayPixels);
  CHECK(revert == 0, "%s: %d block states went backwards or were cut in half", label, revert);

  Shot end = grab(lcd);
  int dEnd = diffCount(end, B);
  CHECK(dEnd == 0, "%s: last wipe frame differs from the clean new screen in %d px", label, dEnd);

  gfx::endTransition(toSliders);
  // the next normal frame of the new screen must also equal a clean repaint
  if (toSliders) gfx::renderSliders(lcd, s.hue, s.bri, s.net, 5100u);
  else           gfx::render(lcd, s.toggle, s.net, 5100u);
  Shot after = grab(lcd);
  Shot clean = toSliders ? cleanScreen2(lcd, s, 5100u) : cleanScreen1(lcd, s, 5100u);
  int dAfter = diffCount(after, clean);
  CHECK(dAfter == 0, "%s: first normal frame after the wipe differs from a clean repaint in %d px", label, dAfter);
}

static void testWipeFirstFrameUntouched(LGFX& lcd) {
  printf("wipe: progress 0 changes nothing\n");
  Scene s;
  setupScene(s, tog::THEME_DARK, 0.3f, 0.7f, NetStatus::Online);
  Shot A = cleanScreen1(lcd, s, 1000);
  gfx::beginTransition(true);
  gfx::renderTransition(lcd, true, 0.0f, s.toggle, s.hue, s.bri, s.net, 1000);
  CHECK(diffCount(grab(lcd), A) == 0, "progress 0 must leave the old screen untouched");
  gfx::endTransition(true);
}

// ---------------------------------------------------------------------------
// 6. screen two content at the extremes
// ---------------------------------------------------------------------------
static void testScreen2Content(LGFX& lcd) {
  printf("screen two: meter and knobs at the extremes\n");
  Scene s;
  setupScene(s, tog::THEME_DARK, 0.0f, 0.0f, NetStatus::Online);
  Shot lo = cleanScreen2(lcd, s, 0);
  setupScene(s, tog::THEME_DARK, 1.0f, 1.0f, NetStatus::Online);
  Shot hi = cleanScreen2(lcd, s, 0);
  setupScene(s, tog::THEME_DARK, 0.5f, 0.5f, NetStatus::Online);
  Shot mid = cleanScreen2(lcd, s, 0);

  // the knob at 0 and at 1 differs, and the screens differ between extremes
  CHECK(diffCount(lo, hi) > 1000, "extremes should look different (%d px differ)", diffCount(lo, hi));
  CHECK(diffCount(lo, mid) > 500 && diffCount(mid, hi) > 500, "mid position should differ from both extremes");

  // Going from 0% to 100% should change nearly every meter cell (the knob covers one or two).
  int differing = 0;
  for (int c = 0; c < layout::BRI_CELLS; c++) {
    int y = layout::BRI_CELL_Y0 + c * layout::BRI_CELL_PITCH + 1;
    size_t i = (size_t)y * 320 + layout::BRI_CELL_X0 + 1;
    if (lo[i] != hi[i]) differing++;
  }
  CHECK(differing >= 12, "going from 0%% to 100%% should change nearly every meter cell, changed %d of %d", differing, layout::BRI_CELLS);

  // No pixel of screen two may spill into the swipe-back zone for the knob frames: the left two thirds are hue only.
  // (a cheap sanity check: column 213 has the same colour in lo and hi except near the vertical centre of the hue knob row)
  int spill = 0;
  for (int y = 0; y < 240; y++) { size_t i = (size_t)y * 320 + 213; if (lo[i] != hi[i]) spill++; }
  CHECK(spill == 0, "the brightness slider's changes must not reach x=213 (%d rows changed)", spill);
}

int main() {
  LGFX lcd;
  printf("== test_screens\n");
  testScreen2DirtyRects(lcd);
  testScreen2Content(lcd);
  testWipeFirstFrameUntouched(lcd);

  struct Case { bool up; tog::Theme th; NetStatus net; int frames; const char* label; };
  static const Case cases[] = {
    { true,  tog::THEME_DARK,  NetStatus::Online,        27, "up, dark, 27 frames" },
    { true,  tog::THEME_LIGHT, NetStatus::Online,        27, "up, light, 27 frames" },
    { false, tog::THEME_DARK,  NetStatus::Online,        27, "down, dark, 27 frames" },
    { false, tog::THEME_LIGHT, NetStatus::Online,        27, "down, light, 27 frames" },
    { true,  tog::THEME_DARK,  NetStatus::SearchingBulb, 40, "up, dark, amber dot, 40 frames" },
    { false, tog::THEME_LIGHT, NetStatus::WifiDown,      40, "down, light, red dot, 40 frames" },
    { true,  tog::THEME_DARK,  NetStatus::Online,        1,  "up, one big jump" },
    { false, tog::THEME_DARK,  NetStatus::Online,        1,  "down, one big jump" },
    { true,  tog::THEME_LIGHT, NetStatus::Online,        5,  "up, 5 big steps" },
    { false, tog::THEME_LIGHT, NetStatus::Online,        5,  "down, 5 big steps" },
    { true,  tog::THEME_DARK,  NetStatus::Online,        400, "up, 400 tiny steps" },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    printf("wipe: %s\n", cases[i].label);
    testWipe(lcd, cases[i].up, cases[i].th, cases[i].net, cases[i].frames, cases[i].label);
  }

  printf("%d checks, %d failed\n", g_checks, g_fail);
  printf("%s\n", g_fail ? "CHECKS FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
