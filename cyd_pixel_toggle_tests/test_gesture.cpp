// test_gesture.cpp - checks the touch gesture recogniser (gesture.cpp) on the laptop.
//
// Run with ./build_gesture_test.sh. Exit code 0 = everything passed.
// Build with -DBREAK_ON_PURPOSE to prove the test can fail (that run MUST exit non-zero).
#include <stdio.h>
#include <stdlib.h>
#include "gesture.h"

using namespace gesture;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { g_fails++; printf("FAIL line %d: %s\n", __LINE__, msg); } } while (0)

// A little finger simulator: one reading per call, 8 ms apart.
struct Finger {
  Recognizer r;
  uint32_t t = 0;
  int downs = 0, ups = 0, taps = 0, moves = 0, swUp = 0, swDown = 0;
  Event last{};
  int swipeAtStep = -1;
  int steps = 0;

  void feed(bool touched, int x, int y) {
    t += 8; steps++;
    Event e = r.feed(t, touched, x, y);
    last = e;
    downs += e.down; ups += e.up; taps += e.tap; moves += e.moved;
    if (e.swipeUp) { swUp++; if (swipeAtStep < 0) swipeAtStep = steps; }
    if (e.swipeDown) swDown++;
  }
  void touch(int x, int y) { feed(true, x, y); }
  void lift() { for (int i = 0; i < 3; i++) feed(false, 0, 0); }
  // move in a straight line from (x0,y0) to (x1,y1) in n readings
  void line(int x0, int y0, int x1, int y1, int n) {
    for (int i = 0; i <= n; i++) touch(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n);
  }
};

static void testDebounce() {
  Finger f;
  f.touch(100, 100);
  CHECK(f.downs == 0, "one touched reading must not be a touch");
  f.feed(false, 0, 0);
  f.touch(100, 100);
  CHECK(f.downs == 0, "touched, empty, touched must not be a touch");
  f.touch(101, 100);
  CHECK(f.downs == 1, "two touched readings in a row = down");
  CHECK(f.last.sx == 101 && f.last.sy == 100, "start point is the second reading");
  CHECK(f.last.held, "held while down");
  // one stray empty reading inside a touch does not lift
  f.feed(false, 0, 0);
  f.touch(101, 100);
  f.feed(false, 0, 0); f.feed(false, 0, 0);
  CHECK(f.ups == 0, "two empty readings in a row are not a lift yet");
  f.feed(false, 0, 0);
  CHECK(f.ups == 1, "three empty readings = up");
  CHECK(f.taps == 1, "a motionless touch is a tap");
  CHECK(!f.last.held, "not held after the lift");
}

static void testNoiseNeverTouches() {
  Finger f;
  for (int i = 0; i < 100; i++) f.feed(i % 2 == 0, 50, 50);
  CHECK(f.downs == 0, "alternating readings never make a touch");
}

static void testTapWithJitter() {
  Finger f;
  f.touch(160, 120); f.touch(161, 121);
  for (int i = 0; i < 6; i++) f.touch(160 + (i % 3) - 1, 120 + (i % 2) * 3);
  f.lift();
  CHECK(f.taps == 1 && f.ups == 1, "a jittery touch inside the slop circle is a tap");
  CHECK(f.moves == 0, "no move event for a tap");
  CHECK(f.swUp == 0 && f.swDown == 0, "a tap is not a swipe");
}

static void testSwipeUpFiresOnceMidGesture() {
  Finger f;
  f.line(160, 200, 160, 120, 20);          // 80 px up over 20 readings (4 px per reading)
  CHECK(f.swUp == 1, "swipe up fires exactly once");
  CHECK(f.swDown == 0, "no swipe down");
  CHECK(f.moves == 1 && f.last.axis == Axis::Vertical, "axis is vertical");
  CHECK(f.swipeAtStep > 0 && f.swipeAtStep < f.steps, "the swipe fires before the finger lifts");
  f.lift();
  CHECK(f.taps == 0 && f.ups == 1, "a swipe is not a tap");
  CHECK(f.swUp == 1, "lifting does not fire a second swipe");
}

static void testSwipeDown() {
  Finger f;
  f.line(100, 60, 100, 200, 25);
  f.lift();
  CHECK(f.swDown == 1 && f.swUp == 0, "swipe down fires once");
}

static void testShortDragIsNotASwipe() {
  Finger f;
  f.line(100, 100, 100, 100 + SWIPE_PX - 6, 10);   // a bit short of the swipe distance
  f.lift();
  CHECK(f.swUp + f.swDown == 0, "a short vertical drag is not a swipe");
  CHECK(f.taps == 0, "but it is not a tap either");
}

static void testSidewaysNeverSwipes() {
  Finger f;
  f.line(40, 120, 190, 122, 30);           // long slide right
  f.line(190, 122, 190, 230, 30);          // then wander far downwards
  f.lift();
  CHECK(f.last.axis == Axis::Horizontal, "a sideways start stays sideways");
  CHECK(f.swUp + f.swDown == 0, "a sideways drag can never become a swipe");
}

static void testDiagonalNeedsMostlyVertical() {
  Finger a;   // 40 right, 45 up: more than 1/1.2 sideways -> not a swipe
  a.line(100, 200, 140, 155, 20);
  a.lift();
  CHECK(a.swUp == 0, "a diagonal swipe that is too sideways does not fire");

  Finger b;   // 20 right, 60 up: mostly vertical -> swipe
  b.line(100, 200, 120, 140, 20);
  b.lift();
  CHECK(b.swUp == 1, "a mostly vertical diagonal fires");
}

static void testWobblySwipe() {
  Finger f;
  // a hand drifts left and right by a few pixels while it goes up (a swing every 4 readings)
  for (int i = 0; i <= 24; i++) f.touch(150 + (((i / 4) % 2) == 0 ? -4 : 4), 210 - i * 4);
  f.lift();
  CHECK(f.swUp == 1, "a wobbly upward swipe still fires");
}

static void testLiftOffJumpIsIgnored() {
  Finger f;
  f.touch(160, 120); f.touch(160, 120);
  for (int i = 0; i < 4; i++) f.touch(160, 120);
  f.touch(300, 20);                        // the wild last reading before the finger lifts
  f.lift();
  CHECK(f.taps == 1, "a lift-off jump does not stop a tap");
  CHECK(f.last.x == 160 && f.last.y == 120, "the reported position is the one before the jump");
  CHECK(f.swUp + f.swDown == 0, "a lift-off jump is not a swipe");
}

static void testTwoTouchesAreIndependent() {
  Finger f;
  f.line(160, 200, 160, 120, 20);
  f.lift();
  CHECK(f.swUp == 1, "first touch swipes");
  f.touch(50, 50); f.touch(50, 50);
  CHECK(f.last.sx == 50 && f.last.sy == 50, "second touch starts fresh");
  CHECK(f.last.axis == Axis::None, "axis is reset");
  f.line(50, 50, 50, 140, 20);
  f.lift();
  CHECK(f.swDown == 1 && f.swUp == 1, "second touch can swipe too");
}

static void testIdleHasNoFlags() {
  Finger f;
  f.touch(10, 10); f.touch(10, 10);
  Event e = f.r.idle();
  CHECK(!e.sample && !e.down && !e.up && !e.tap && !e.moved && !e.swipeUp && !e.swipeDown, "idle() carries no event flags");
  CHECK(e.held, "idle() still says the finger is held");
}

int main() {
  testDebounce();
  testNoiseNeverTouches();
  testTapWithJitter();
  testSwipeUpFiresOnceMidGesture();
  testSwipeDown();
  testShortDragIsNotASwipe();
  testSidewaysNeverSwipes();
  testDiagonalNeedsMostlyVertical();
  testWobblySwipe();
  testLiftOffJumpIsIgnored();
  testTwoTouchesAreIndependent();
  testIdleHasNoFlags();
#ifdef BREAK_ON_PURPOSE
  CHECK(SWIPE_PX == 999, "deliberately wrong expectation (BREAK_ON_PURPOSE)");
#endif
  printf("gesture: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
