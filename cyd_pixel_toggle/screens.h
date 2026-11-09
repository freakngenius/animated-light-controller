// screens.h - which screen is showing, and what a finger does on each of them.
//
// There is NO hardware in here (no Arduino calls, no screen), so the same file runs on a
// laptop for the tests and for cyd_sim.
//
//   Screen one (Toggle):  tap the toggle = flip it; swipe up anywhere = go to screen two.
//   Screen two (Sliders): drag the rainbow bar sideways = hue; drag the brightness bar up or
//                         down = brightness; swipe down on the left two thirds = back.
//
// The controller does not draw and does not talk to the bulb. It tells the sketch what
// happened through `Actions`, and the sketch does the rest.
#pragma once
#include <stdint.h>
#include "gesture.h"
#include "physics.h"
#include "layout.h"

namespace ui {

enum class Screen : uint8_t { Toggle = 0, Sliders = 1 };

struct Rect { int16_t x, y, w, h; };

// What the sketch has to do after a touch.
struct Actions {
  bool toggled;         // the user tapped the toggle; it has been flipped already (tell the bulb)
  bool colourChanged;   // a slider target moved (tell the bulb the new colour)
};

constexpr float TRANSITION_SECONDS = 0.45f;

class Controller {
 public:
  explicit Controller(tog::Toggle& toggle) : toggle_(toggle) {}

  // `toggleHit` = where a touch counts as "on the toggle" (the track plus a little slack).
  void begin(const Rect& toggleHit, float hueValue, float brightnessValue);

  // Feed every event the touch driver gives you (also the empty ones between readings).
  Actions onTouch(const gesture::Event& ev);

  // Advance the sliders and the screen change by dtSeconds. (The toggle itself is advanced by the caller.)
  void update(float dtSeconds);

  // ---- what to draw ----
  Screen screen() const { return screen_; }                 // the screen the finger is on; while changing, the one we are leaving
  bool   transitioning() const { return moving_; }
  Screen transitionTo() const { return to_; }
  bool   transitionUp() const { return to_ == Screen::Sliders; }   // true: wipes in from the bottom; false: from the top
  float  progress() const;                                  // eased 0..1 while transitioning, else 0 (or 1 right after it ends)
  Screen visibleNow() const { return moving_ ? to_ : screen_; }

  const tog::Slider& hue() const { return hue_; }
  const tog::Slider& brightness() const { return bri_; }
  tog::Slider& hueMutable() { return hue_; }
  tog::Slider& brightnessMutable() { return bri_; }

  float    hueValue() const { return hue_.target(); }          // 0..1
  float    brightnessValue() const { return bri_.target(); }   // 0..1
  uint16_t lifxHue() const { return layout::lifxHue(hue_.target()); }
  uint16_t lifxBrightness() const { return layout::lifxBrightness(bri_.target()); }

  // true while anything the controller owns is moving (sliders, screen change)
  bool busy() const { return moving_ || hue_.busy() || bri_.busy(); }

  // Developer hook for the tests: how many screen changes have finished.
  uint32_t changes() const { return changes_; }

 private:
  enum class Drag : uint8_t { None, Toggle, HueMaybe, Hue, Brightness };

  void startTransition(Screen to);
  void releaseAll();
  static bool inside(const Rect& r, int x, int y);

  tog::Toggle& toggle_;
  tog::Slider  hue_, bri_;
  Rect   toggleHit_ = { 0, 0, 0, 0 };

  Screen screen_ = Screen::Toggle;
  Screen to_ = Screen::Toggle;
  bool   moving_ = false;
  float  t_ = 0.0f;
  uint32_t changes_ = 0;

  Drag drag_ = Drag::None;
  bool ownsTouch_ = false;      // the touch in progress began while the controller was listening
};

}  // namespace ui
