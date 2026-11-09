// screens.cpp - see screens.h.
#include "screens.h"

namespace ui {

namespace {
inline float easeInOut(float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t < 0.5f ? 2.0f * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) / 2.0f;
}
}

bool Controller::inside(const Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void Controller::begin(const Rect& toggleHit, float hueValue, float brightnessValue) {
  toggleHit_ = toggleHit;
  hue_.begin(hueValue);
  bri_.begin(brightnessValue);
  screen_ = to_ = Screen::Toggle;
  moving_ = false;
  t_ = 0.0f;
  changes_ = 0;
  drag_ = Drag::None;
  ownsTouch_ = false;
}

float Controller::progress() const {
  if (moving_) return easeInOut(t_);
  return 0.0f;
}

// Let go of whatever the finger was holding (the knob stops leaning in).
void Controller::releaseAll() {
  if (drag_ == Drag::Toggle) toggle_.setPressed(false);
  hue_.setPressed(false);
  bri_.setPressed(false);
  drag_ = Drag::None;
}

void Controller::startTransition(Screen to) {
  releaseAll();
  ownsTouch_ = false;          // the finger that made the swipe does nothing more, even after the change ends
  to_ = to;
  moving_ = true;
  t_ = 0.0f;
}

void Controller::update(float dt) {
  hue_.update(dt);
  bri_.update(dt);
  if (moving_) {
    t_ += dt / TRANSITION_SECONDS;
    if (t_ >= 1.0f) {
      moving_ = false;
      t_ = 0.0f;
      screen_ = to_;
      changes_++;
    }
  }
}

Actions Controller::onTouch(const gesture::Event& ev) {
  Actions act = { false, false };

  // While the screen is changing, touches are ignored. A finger that was already down when the
  // change started (or lands during it) stays ignored until it lifts, so nothing gets stuck.
  if (moving_) {
    if (ev.up) ownsTouch_ = false;
    else if (ev.down) ownsTouch_ = false;
    return act;
  }
  if (ev.down) {
    ownsTouch_ = true;
  } else if (!ownsTouch_) {
    return act;
  }

  if (screen_ == Screen::Toggle) {
    // ---------------- screen one ----------------
    if (ev.down) {
      if (inside(toggleHit_, ev.sx, ev.sy)) { drag_ = Drag::Toggle; toggle_.setPressed(true); }
      else drag_ = Drag::None;
    }
    if (ev.moved && drag_ == Drag::Toggle) {          // the finger slid: not a tap any more
      toggle_.setPressed(false);
      drag_ = Drag::None;
    }
    if (ev.swipeUp) {
      startTransition(Screen::Sliders);
      return act;
    }
    if (ev.up) {
      bool tapOnToggle = ev.tap && drag_ == Drag::Toggle;
      if (drag_ == Drag::Toggle) toggle_.setPressed(false);
      drag_ = Drag::None;
      if (tapOnToggle) { toggle_.flip(); act.toggled = true; }
    }
  } else {
    // ---------------- screen two ----------------
    if (ev.down) {
      if (ev.sx >= layout::SPLIT_X) {
        drag_ = Drag::Brightness;
        bri_.setPressed(true);
      } else if (ev.sy >= layout::HUE_HIT_Y0 && ev.sy < layout::HUE_HIT_Y1) {
        drag_ = Drag::HueMaybe;                        // sideways = hue drag, downwards = swipe back
        hue_.setPressed(true);
      } else {
        drag_ = Drag::None;                            // only a swipe down can start here
      }
    }

    if (ev.moved && drag_ == Drag::HueMaybe) {
      if (ev.axis == gesture::Axis::Horizontal) {
        drag_ = Drag::Hue;
      } else {                                         // mostly vertical: let go of the knob, it may be a swipe back
        hue_.setPressed(false);
        drag_ = Drag::None;
      }
    }

    // a drag moves its slider on every fresh reading
    if (ev.sample && ev.held) {
      if (drag_ == Drag::Hue) {
        float v = layout::hueValueFromX(ev.x);
        if (v != hue_.target()) { hue_.setTarget(v); act.colourChanged = true; }
      } else if (drag_ == Drag::Brightness) {
        float v = layout::briValueFromY(ev.y);
        if (v != bri_.target()) { bri_.setTarget(v); act.colourChanged = true; }
      }
    }

    if (ev.swipeDown && ev.sx < layout::SPLIT_X && drag_ != Drag::Hue && drag_ != Drag::Brightness) {
      startTransition(Screen::Toggle);
      return act;
    }

    if (ev.up) {
      if (ev.tap && drag_ == Drag::HueMaybe) {         // a tap on the rainbow bar: knob goes there
        float v = layout::hueValueFromX(ev.sx);
        if (v != hue_.target()) { hue_.setTarget(v); act.colourChanged = true; }
      } else if (ev.tap && drag_ == Drag::Brightness) {
        float v = layout::briValueFromY(ev.sy);
        if (v != bri_.target()) { bri_.setTarget(v); act.colourChanged = true; }
      }
      hue_.setPressed(false);
      bri_.setPressed(false);
      drag_ = Drag::None;
    }
  }

  if (ev.up) ownsTouch_ = false;
  return act;
}

}  // namespace ui
