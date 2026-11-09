// physics.h - the toggle's brain: springs, timeline and the pixel-cell particles.
//
// This is a straight port of the animation code in theme-toggle/index.html.
// It has NO Arduino calls, so the exact same file also runs on a laptop for tests.
//
// All numbers here are in DESIGN pixels: the same 392 x 198 space the web
// version uses. draw.cpp scales them to the real screen.
#pragma once
#include <stdint.h>

namespace tog {

enum Theme : uint8_t { THEME_DARK = 0, THEME_LIGHT = 1 };

// ---- geometry of the design (identical to the web version) ----------------
constexpr float TRACK_W    = 392.0f;
constexpr float TRACK_H    = 198.0f;
constexpr float KNOB       = 180.0f;   // knob size at rest
constexpr float PAD        = 9.0f;
constexpr float KNOB_Y     = 99.0f;    // knob centre, measured from the top of the track
constexpr float REST_DARK  = 99.0f;    // knob centre x when dark  (PAD + KNOB/2)
constexpr float REST_LIGHT = 293.0f;   // knob centre x when light (TRACK_W - PAD - KNOB/2)
constexpr float ANGLE_DARK  = 188.0f;  // rim glow angle, degrees clockwise from 12 o'clock
constexpr float ANGLE_LIGHT = 48.0f;
constexpr float PITCH = 6.5f;          // distance between pixel-cell centres
constexpr float CELL  = 5.3f;          // size of one pixel cell

// ---- one spring ------------------------------------------------------------
// Semi-implicit Euler, retargetable at any moment (just change t).
struct Spring {
  float x, v, t;     // position, velocity, target
  float k, c;        // stiffness, damping (c = 2 * zeta * sqrt(k))
  void init(float x0, float stiffness, float zeta);
  void step(float dt);
  bool still() const;          // |x - t| < 0.005 and |v| < 0.01
  void snap(float x0);         // jump: x = t = x0, v = 0
};

// ---- pixel cells (the moon / sun are made of these) ------------------------
enum CellRole : uint8_t { ROLE_BRIGHT = 0, ROLE_GHOST, ROLE_STAR, ROLE_CORE, ROLE_RAY };

struct Slot {                 // where a particle rests in one theme
  float x, y, a;              // px from knob centre, opacity 0..1
  uint8_t role;               // CellRole
  bool used;                  // false = this particle has no cell in that theme
};

struct Particle {
  // --- what draw.cpp reads each frame ---
  float x, y, a;              // current position (px from knob centre, design px) and opacity 0..1
  bool  star;                 // true for the moon's stars: draw.cpp makes them twinkle when idle
  float phase;                // per-particle phase for the twinkle
  // --- internals of the dissolve / re-assemble morph ---
  Slot m, s;                  // resting slot in dark (moon) and light (sun)
  float cloudX, cloudY, cloudA;
  float fromX, fromY, fromA;
  float toX, toY, toA;
  float d1, d2, curl;
};

// ---- the toggle ---------------------------------------------------------------
class Toggle {
 public:
  static const int MAX_PARTICLES = 92;    // the real table has 87 (checked by test_physics)

  // Builds the particle tables and snaps everything to `start`. `seed` feeds a tiny
  // built-in random generator (pass esp_random() on the board, any number in tests).
  void begin(Theme start, uint32_t seed);

  // Change theme. Safe to call at any moment and at any rate (fast repeated taps).
  // Returns true if the theme actually changed. instant=true jumps with no animation.
  bool setTheme(Theme t, bool instant = false);
  void flip() { setTheme(theme_ == THEME_DARK ? THEME_LIGHT : THEME_DARK); }

  // Finger is down on the toggle (the knob leans into the press), up again.
  void setPressed(bool p);

  // Advance the simulation. dtSeconds is clamped to 0.05; stepped inside in fixed 1/480 s slices.
  void update(float dtSeconds);

  // true while anything is still moving (then the screen needs a redraw every frame).
  bool busy() const;

  // ---- read-only outputs for the drawing code ----
  Theme theme() const { return theme_; }
  bool  pressed() const { return pressed_; }
  float centerX() const { return c_.x; }          // knob centre x, design px, 0..392
  float knobWidth() const { return w_.x; }        // knob width, design px (180 at rest, stretches when fast)
  float knobScaleY() const;                       // squash factor, 0.85 .. 1.06 (height = 180 * this)
  float themeProgress() const;                    // 0 = dark .. 1 = light (fast spring, clamped 0..1)
  float rimAngleDeg() const { return a_.x; }      // rim glow angle (can be any real number; use fmod)
  float flash() const;                            // rim flash 0 .. 1.4, decays after each flip
  float simTime() const { return simTime_; }      // seconds of simulated time (for twinkle etc.)
  bool  morphing() const { return morphOn_; }
  int   particleCount() const { return n_; }
  const Particle& particle(int i) const { return parts_[i]; }

 private:
  // A tiny scheduler: "in `left` seconds do `kind`, but only if no newer tap has happened (tok == epoch_)".
  struct Event { float left; uint8_t kind; uint32_t tok; bool used; };

  void snapAll();
  void setRest(Theme t);
  void startMorph(Theme to);
  void evalMorph(Particle& p, float tn);
  void buildParticles();
  void stepOnce(float h);
  void normalizeRimAngle();
  float centerTarget() const;
  float ccwTarget(float cur, float base) const;
  void later(float ms, uint8_t kind);
  float rnd();                 // 0..1

  Spring c_, w_, p_, a_, fl_;  // centre, width, theme progress, rim angle, rim flash
  Theme theme_;
  bool pressed_;
  float acc_;                  // leftover time that did not fill a whole 1/480 s step
  float simTime_;
  uint32_t epoch_;
  Event queue_[4];
  Particle parts_[MAX_PARTICLES];
  int n_;
  bool morphOn_;
  float morphMs_;
  uint32_t rng_;
};

// ---- a slider knob (the second screen) ---------------------------------------
// One of these drives the hue slider and one the brightness slider. The value runs 0..1
// along the slider. The finger sets a target; the knob follows it on a spring, stretches a
// little along the way when it moves fast, leans in while pressed and glows more while held.
// Same spring style as the toggle knob. No Arduino calls in here.
class Slider {
 public:
  void begin(float value);               // snap to `value` (0..1), everything at rest
  void setTarget(float value);           // where the finger wants the knob (clamped to 0..1)
  void setPressed(bool p);               // finger is on this slider (or not)
  void update(float dtSeconds);          // advance (fixed 1/480 s slices inside, dt clamped to 0.05)
  bool busy() const;                     // true while anything still moves

  float target() const { return target_; }    // what the finger asked for, 0..1
  float pos() const { return pos_.x; }        // where the knob is drawn, 0..1 (can overshoot a little)
  float stretch() const { return st_.x; }     // 0..1, how stretched the knob is by its own speed
  float press() const { return pr_.x; }       // 0..1, how far the knob is pressed in
  float glow() const { return gl_.x; }        // 0..1, rim glow strength
  bool  pressed() const { return pressed_; }

 private:
  Spring pos_, st_, pr_, gl_;
  float target_ = 0.0f;
  bool  pressed_ = false;
  float acc_ = 0.0f;
};

}  // namespace tog
