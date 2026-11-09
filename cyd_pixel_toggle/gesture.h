// gesture.h - turns raw touch readings into taps, drags and swipes.
//
// There is NO hardware in here. touch.cpp reads the touch chip and hands every reading to
// Recognizer::feed(); the same file also runs on a laptop for the tests and for cyd_sim.
//
// What it does, in plain English:
//   * Debounce: two touched readings in a row = the finger landed, three empty readings in a
//     row = the finger lifted (single odd readings are electrical noise on a resistive screen).
//   * Tap: the finger lands and lifts again without ever leaving a small circle (SLOP_PX).
//   * Axis: the first time the finger leaves that circle, we look at which way it went
//     (mostly sideways or mostly up/down) and remember it for the rest of the touch.
//   * Swipe: a touch whose axis is "vertical" and that has travelled SWIPE_PX up or down
//     (and is still mostly vertical) fires swipeUp / swipeDown ONCE, right away, without
//     waiting for the finger to lift. A sideways touch can never become a swipe.
#pragma once
#include <stdint.h>

namespace gesture {

constexpr int SLOP_PX  = 8;    // movement smaller than this is still a tap
constexpr int SWIPE_PX = 36;   // vertical travel that counts as a swipe

enum class Axis : uint8_t { None = 0, Horizontal, Vertical };

struct Event {
  bool sample;      // a new reading was processed in this call (positions are fresh)
  bool down;        // the finger just landed (debounced); sx, sy = where
  bool up;          // the finger just lifted
  bool tap;         // lifted without ever leaving the slop circle (fires together with `up`)
  bool moved;       // the finger just left the slop circle (once per touch); `axis` is set
  bool swipeUp;     // fires once per touch, mid-gesture
  bool swipeDown;   // fires once per touch, mid-gesture
  bool held;        // the finger is on the glass right now
  Axis axis;        // None until the finger leaves the slop circle
  int16_t x, y;     // finger position now (see the note in gesture.cpp about the lift-off jump)
  int16_t sx, sy;   // where this touch started
};

class Recognizer {
 public:
  Recognizer() { reset(); }
  void reset();

  // Call once per reading of the touch chip (every ~8 ms). `touched` = the chip saw a finger,
  // x and y = screen pixels (only looked at when touched).
  Event feed(uint32_t nowMs, bool touched, int x, int y);

  // The state between readings: no event flags set, positions and `held` current.
  Event idle() const;

 private:
  Event base() const;

  bool    down_;          // finger is on the glass (debounced)
  uint8_t seenDown_;      // consecutive touched readings
  uint8_t seenUp_;        // consecutive empty readings
  bool    moved_;         // left the slop circle during this touch
  bool    swiped_;        // a swipe already fired during this touch
  Axis    axis_;
  int16_t sx_, sy_;       // touch start
  int16_t x_, y_;         // reported position
  int16_t lx_, ly_;       // newest raw reading
  uint8_t have_;          // how many readings this touch has delivered (0, 1, 2+)
};

}  // namespace gesture
