// Host test for physics.cpp. Build + run: ./build_physics_test.sh
// Checks the things that matter for "fast repeated taps never glitch":
//  - particle table sizes, the morph really ends on the right cells
//  - settling: after any tap storm the toggle lands exactly on the right resting pose
//  - no NaN / no runaway numbers, rim angle stays bounded after thousands of flips
//  - stale delayed events from earlier taps cannot hijack the newest tap
#include "../cyd_pixel_toggle/physics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

using namespace tog;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool finite(float v) { return v == v && fabsf(v) < 1e7f; }

static void checkSane(const Toggle& t, const char* where) {
  CHECK(finite(t.centerX()) && finite(t.knobWidth()) && finite(t.rimAngleDeg()) && finite(t.flash()) && finite(t.themeProgress()),
        "non-finite state at %s", where);
  CHECK(t.centerX() > -50 && t.centerX() < 450, "centre out of range %f at %s", t.centerX(), where);
  CHECK(t.knobWidth() > 120 && t.knobWidth() < 260, "width out of range %f at %s", t.knobWidth(), where);
  CHECK(t.knobScaleY() >= 0.85f && t.knobScaleY() <= 1.06f, "scaleY out of range at %s", where);
  for (int i = 0; i < t.particleCount(); i++) {
    const Particle& p = t.particle(i);
    CHECK(finite(p.x) && finite(p.y) && finite(p.a), "particle %d not finite at %s", i, where);
    CHECK(fabsf(p.x) < 200 && fabsf(p.y) < 200, "particle %d wandered off (%f,%f) at %s", i, p.x, p.y, where);
  }
}

static void settle(Toggle& t, float seconds = 3.0f) {
  for (float s = 0; s < seconds; s += 1.0f / 60.0f) t.update(1.0f / 60.0f);
}

static void checkRest(Toggle& t, Theme th, const char* where) {
  float restX = th == THEME_LIGHT ? REST_LIGHT : REST_DARK;
  CHECK(!t.busy(), "still busy at %s", where);
  CHECK(fabsf(t.centerX() - restX) < 0.05f, "centre %f != %f at %s", t.centerX(), restX, where);
  CHECK(fabsf(t.knobWidth() - KNOB) < 0.05f, "width %f at %s", t.knobWidth(), where);
  CHECK(fabsf(t.themeProgress() - (th == THEME_LIGHT ? 1.0f : 0.0f)) < 0.01f, "progress %f at %s", t.themeProgress(), where);
  float want = th == THEME_LIGHT ? ANGLE_LIGHT : ANGLE_DARK;
  float d = fmodf(t.rimAngleDeg() - want, 360.0f);
  if (d < 0) d += 360.0f;
  CHECK(d < 0.1f || d > 359.9f, "rim angle %f is not %f (mod 360) at %s", t.rimAngleDeg(), want, where);
  CHECK(!t.morphing(), "still morphing at %s", where);
  CHECK(t.theme() == th, "wrong theme at %s", where);
  for (int i = 0; i < t.particleCount(); i++) {
    const Particle& p = t.particle(i);
    const Slot& s = th == THEME_LIGHT ? p.s : p.m;
    float ex = s.used ? s.x : 0, ey = s.used ? s.y : 0, ea = s.used ? s.a : 0;
    CHECK(fabsf(p.x - ex) < 0.01f && fabsf(p.y - ey) < 0.01f && fabsf(p.a - ea) < 0.01f, "particle %d not at rest slot at %s", i, where);
  }
}

int main() {
  Toggle t;
  t.begin(THEME_DARK, 12345);

  // ---- table sizes ----
  int moonCells = 0, sunCells = 0, paired = 0;
  for (int i = 0; i < t.particleCount(); i++) {
    const Particle& p = t.particle(i);
    if (p.m.used) moonCells++;
    if (p.s.used) sunCells++;
    if (p.m.used && p.s.used) paired++;
  }
  printf("particles=%d (max %d)  moon cells=%d  sun cells=%d  paired=%d\n", t.particleCount(), Toggle::MAX_PARTICLES, moonCells, sunCells, paired);
  CHECK(t.particleCount() > 60 && t.particleCount() <= Toggle::MAX_PARTICLES, "particle count %d", t.particleCount());
  CHECK(moonCells > 60 && sunCells > 40, "icon cell counts");
  printf("sizeof(Toggle)=%u bytes\n", (unsigned)sizeof(Toggle));

  checkRest(t, THEME_DARK, "boot");

  // ---- one clean flip, both ways ----
  CHECK(t.setTheme(THEME_LIGHT), "setTheme should report a change");
  CHECK(!t.setTheme(THEME_LIGHT), "same theme must report no change");
  CHECK(t.busy(), "should be busy right after a flip");
  float maxW = 0, maxV = 0;
  for (int i = 0; i < 180; i++) {
    t.update(1.0f / 60.0f);
    if (t.knobWidth() > maxW) maxW = t.knobWidth();
    checkSane(t, "flip");
  }
  (void)maxV;
  CHECK(maxW > KNOB + 5.0f, "knob should stretch while sliding (max width %f)", maxW);
  settle(t);
  checkRest(t, THEME_LIGHT, "after dark->light");
  t.flip();
  settle(t);
  checkRest(t, THEME_DARK, "after light->dark");

  // ---- the rim glow always turns counter-clockwise (angle only goes down) ----
  {
    Toggle r; r.begin(THEME_DARK, 7);
    float last = r.rimAngleDeg();
    r.flip();
    bool monotonic = true;
    for (int i = 0; i < 240; i++) {
      r.update(1.0f / 60.0f);
      // the rim spring is slightly underdamped (zeta .8): it overshoots ~1.5% of its travel
      // (up to ~6 deg on a full turn) and comes back; anything bigger would be a real reversal
      if (r.rimAngleDeg() > last + 8.0f) monotonic = false;
      if (r.rimAngleDeg() < last) last = r.rimAngleDeg();
    }
    CHECK(monotonic, "rim angle moved clockwise during a flip");
  }

  // ---- tap storms: random intervals, random frame times, with presses ----
  srand(42);
  for (int storm = 0; storm < 300; storm++) {
    Toggle s; s.begin(storm & 1 ? THEME_LIGHT : THEME_DARK, 1000 + storm);
    Theme expect = s.theme();
    int taps = 1 + rand() % 25;
    float clock = 0, nextTap = 0.02f * (rand() % 10);
    float endAt = 2.5f;
    float frame = (rand() % 3 == 0) ? (1.0f / 20.0f) : (rand() % 2 ? 1.0f / 30.0f : 1.0f / 60.0f);
    // sometimes a huge stall (WiFi hiccup) in the middle
    float stallAt = (rand() % 4 == 0) ? 0.4f : -1.0f;
    int done = 0;
    while (clock < endAt) {
      if (done < taps && clock >= nextTap) {
        if (rand() % 3 == 0) { s.setPressed(true); }
        s.setPressed(false);
        s.flip();
        expect = (expect == THEME_DARK) ? THEME_LIGHT : THEME_DARK;
        done++;
        nextTap = clock + 0.01f * (rand() % 40);   // 0..400 ms between taps
      }
      float dt = frame;
      if (stallAt >= 0 && clock >= stallAt) { dt = 0.6f; stallAt = -1.0f; }   // 600 ms stall, must be clamped
      s.update(dt);
      clock += frame;
      checkSane(s, "storm");
    }
    CHECK(s.theme() == expect, "storm %d: theme drifted", storm);
    settle(s, 4.0f);
    char label[64]; snprintf(label, sizeof label, "storm %d settle", storm);
    checkRest(s, expect, label);
  }

  // ---- the bulb changes the theme from outside (LIFX app) in the middle of a slide ----
  {
    Toggle s; s.begin(THEME_DARK, 99);
    s.flip();
    for (int i = 0; i < 12; i++) s.update(1.0f / 60.0f);
    s.setTheme(THEME_DARK);            // snap back mid-slide
    settle(s, 4.0f);
    checkRest(s, THEME_DARK, "reversed mid-slide");
    s.setTheme(THEME_LIGHT, true);     // instant
    CHECK(!s.busy(), "instant change must not leave anything moving");
    checkRest(s, THEME_LIGHT, "instant");
  }

  // ---- thousands of flips: rim angle must stay small ----
  {
    Toggle s; s.begin(THEME_DARK, 5);
    float worst = 0;
    for (int i = 0; i < 4000; i++) {
      s.flip();
      for (int f = 0; f < 90; f++) s.update(1.0f / 60.0f);       // 1.5 s per flip
      float a = fabsf(s.rimAngleDeg());
      if (a > worst) worst = a;
    }
    printf("after 4000 flips: worst |rim angle| seen at rest = %.1f deg\n", worst);
    CHECK(worst < 800.0f, "rim angle grew without bound (%f)", worst);
    settle(s, 3.0f);
    checkRest(s, THEME_DARK, "after 4000 flips");
  }

  // ---- long idle: simTime stays small ----
  {
    Toggle s; s.begin(THEME_DARK, 5);
    for (int i = 0; i < 60 * 3600; i++) s.update(1.0f / 60.0f);   // 1 hour of idling
    CHECK(s.simTime() >= 0 && s.simTime() < 32.0f, "simTime %f", s.simTime());
    CHECK(!s.busy(), "idle toggle must not be busy");
  }

  // ---- determinism: same seed, same script -> identical result ----
  {
    Toggle a, b; a.begin(THEME_DARK, 77); b.begin(THEME_DARK, 77);
    a.flip(); b.flip();
    for (int i = 0; i < 30; i++) { a.update(1.0f / 60.0f); b.update(1.0f / 60.0f); }
    bool same = a.centerX() == b.centerX();
    for (int i = 0; i < a.particleCount(); i++) same = same && a.particle(i).x == b.particle(i).x && a.particle(i).y == b.particle(i).y;
    CHECK(same, "not deterministic");
  }

  // ---- sliders (second screen) ----
  {
    Slider s; s.begin(0.5f);
    CHECK(!s.busy() && s.pos() == 0.5f && s.target() == 0.5f, "slider rests where it begins");
    s.setTarget(2.0f);
    CHECK(s.target() == 1.0f, "slider target is clamped high");
    s.setTarget(-3.0f);
    CHECK(s.target() == 0.0f, "slider target is clamped low");

    // a big jump: knob must overshoot a little but stay finite, then land exactly
    s.begin(0.0f);
    s.setPressed(true);
    s.setTarget(1.0f);
    float maxPos = 0, maxStretch = 0;
    for (int i = 0; i < 90; i++) {
      s.update(1.0f / 60.0f);
      if (s.pos() > maxPos) maxPos = s.pos();
      if (s.stretch() > maxStretch) maxStretch = s.stretch();
      CHECK(finite(s.pos()) && finite(s.stretch()) && finite(s.press()) && finite(s.glow()), "slider state not finite");
      CHECK(s.pos() > -0.2f && s.pos() < 1.25f, "slider wandered off (%f)", s.pos());
    }
    CHECK(maxStretch > 0.2f, "knob should stretch while it moves fast (max %f)", maxStretch);
    CHECK(s.press() > 0.9f && s.glow() > 0.9f, "pressed knob leans in and glows (%f, %f)", s.press(), s.glow());
    s.setPressed(false);
    for (int i = 0; i < 240; i++) s.update(1.0f / 60.0f);
    CHECK(!s.busy(), "slider must settle");
    CHECK(s.pos() == 1.0f && s.press() == 0.0f && fabsf(s.glow() - 0.25f) < 1e-6f, "slider rests exactly (%f %f %f)", s.pos(), s.press(), s.glow());
    CHECK(maxPos > 1.0f, "a spring this soft overshoots the end a little (max %f)", maxPos);

    // a drag: the target moves every 8 ms, the knob keeps up and never goes NaN
    Slider d; d.begin(0.1f); d.setPressed(true);
    for (int i = 0; i < 400; i++) {
      d.setTarget(0.5f + 0.5f * sinf(i * 0.05f));
      d.update(0.008f);
      CHECK(finite(d.pos()) && d.pos() > -0.3f && d.pos() < 1.3f, "drag state out of range (%f)", d.pos());
    }
    d.setPressed(false);
    for (int i = 0; i < 300; i++) d.update(1.0f / 60.0f);
    CHECK(!d.busy() && d.pos() == d.target(), "slider lands on the last target after a drag");

    // frame rate must not change where the knob ends up
    Slider a, b; a.begin(0.0f); b.begin(0.0f);
    a.setTarget(0.8f); b.setTarget(0.8f);
    for (int i = 0; i < 30; i++) a.update(1.0f / 60.0f);
    for (int i = 0; i < 60; i++) b.update(1.0f / 120.0f);
    CHECK(fabsf(a.pos() - b.pos()) < 0.02f, "frame rate changed the slider motion (%f vs %f)", a.pos(), b.pos());

    // a stall is clamped, not fast-forwarded
    Slider st; st.begin(0.0f); st.setTarget(1.0f);
    st.update(5.0f);
    CHECK(finite(st.pos()) && st.pos() < 1.3f, "stall clamp (%f)", st.pos());
  }

  if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
  printf("\nALL PHYSICS TESTS PASSED\n");
  return 0;
}
