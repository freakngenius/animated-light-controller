// gesture.cpp - see gesture.h.
#include "gesture.h"

namespace gesture {

namespace {
inline int iabs(int v) { return v < 0 ? -v : v; }
}

void Recognizer::reset() {
  down_ = false;
  seenDown_ = seenUp_ = 0;
  moved_ = swiped_ = false;
  axis_ = Axis::None;
  sx_ = sy_ = x_ = y_ = lx_ = ly_ = 0;
  have_ = 0;
}

Event Recognizer::base() const {
  Event e;
  e.sample = e.down = e.up = e.tap = e.moved = e.swipeUp = e.swipeDown = false;
  e.held = down_;
  e.axis = axis_;
  e.x = x_; e.y = y_;
  e.sx = sx_; e.sy = sy_;
  return e;
}

Event Recognizer::idle() const { return base(); }

Event Recognizer::feed(uint32_t /*nowMs*/, bool touched, int x, int y) {
  Event e = base();
  e.sample = true;

  if (touched) { seenUp_ = 0;   if (seenDown_ < 255) seenDown_++; }
  else         { seenDown_ = 0; if (seenUp_ < 255)   seenUp_++; }

  if (!down_) {
    // Two readings in a row = a real touch. The start point is the second reading: the very
    // first one, taken as the finger lands, is often a little off.
    if (touched && seenDown_ >= 2) {
      down_ = true;
      moved_ = swiped_ = false;
      axis_ = Axis::None;
      sx_ = x_ = lx_ = (int16_t)x;
      sy_ = y_ = ly_ = (int16_t)y;
      have_ = 1;
      e.down = true;
    }
  } else if (touched) {
    // A resistive screen often reports a wild position in the last reading before the finger
    // lifts. So the position we report is always the PREVIOUS reading: by the time the finger
    // lifts, the wild one has not been used for anything.
    x_ = lx_; y_ = ly_;
    lx_ = (int16_t)x; ly_ = (int16_t)y;
    if (have_ < 2) have_++;

    int dx = x_ - sx_, dy = y_ - sy_;
    if (!moved_ && dx * dx + dy * dy >= SLOP_PX * SLOP_PX) {
      moved_ = true;
      axis_ = iabs(dy) > iabs(dx) ? Axis::Vertical : Axis::Horizontal;
      e.moved = true;
    }
    if (moved_ && !swiped_ && axis_ == Axis::Vertical && iabs(dy) >= SWIPE_PX && iabs(dy) * 5 >= iabs(dx) * 6) {
      // iabs(dy) * 5 >= iabs(dx) * 6  means  |dy| >= 1.2 * |dx|: still mostly vertical
      swiped_ = true;
      if (dy < 0) e.swipeUp = true; else e.swipeDown = true;
    }
  } else if (seenUp_ >= 3) {
    // Three empty readings in a row = the finger really lifted.
    down_ = false;
    e.up = true;
    e.tap = !moved_;
    have_ = 0;
  }

  e.held = down_;
  e.axis = axis_;
  e.x = x_; e.y = y_;
  e.sx = sx_; e.sy = sy_;
  return e;
}

}  // namespace gesture
