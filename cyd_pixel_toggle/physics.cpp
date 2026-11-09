// physics.cpp - the toggle's springs, timeline and pixel-cell particles.
// A line-by-line port of the animation code in theme-toggle/index.html.
// No Arduino calls in here, so the same file is unit-tested on a laptop.
#include "physics.h"
#include <math.h>

namespace tog {

namespace {

// ---- timeline, straight from the web version (T = {flip, rim, morph}) ----
const float T_FLIP_MS  = 70.0f;    // theme colours flip
const float T_RIM_MS   = 40.0f;    // rim glow starts to rotate
const float T_MORPH_MS = 620.0f;   // pixel dissolve + re-assemble length
const float M1 = 0.27f;            // fraction of the morph when the cloud is reached
const float M2 = 0.40f;            // fraction of the morph when the cloud is released

const float STRETCH_AMP  = 34.0f;  // extra knob width at full speed
const float STRETCH_VREF = 1150.0f;

// Physics runs in fixed slices of 1/480 s so it behaves the same at any frame rate.
const float SUB = 1.0f / 480.0f;

// simTime wraps every 10*pi seconds: that is a whole number of cycles of the
// twinkle (2.4 rad/s) and the cloud shimmer (41 and 37 rad/s), so the wrap is
// invisible, and floats never get big enough to lose precision.
const float SIM_WRAP = 31.41592653589793f;

const uint8_t EV_RIM  = 1;
const uint8_t EV_FLIP = 2;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float easeInOut(float t) {
  return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f;
}
inline float easeOutBack(float t) {
  const float s = 2.1f;
  float u = t - 1.0f;
  return 1.0f + (s + 1.0f) * u * u * u + s * u * u;
}

// A moon/sun cell before it is paired up into a particle (build-time only).
// Floats keep this table small: setup() runs on a small 8 KB stack.
struct Cell {
  float x, y, a;
  uint8_t role;
};

Cell mkCell(double x, double y, double a, uint8_t role) {
  Cell c = { (float)x, (float)y, (float)a, role };
  return c;
}

const int MAX_MOON = 100;
const int MAX_SUN  = 64;

// Sort cells by angle around (cx,cy), then by distance - same ordering as the
// web version's byAngle comparator. Insertion sort = stable, like JS Array.sort.
double cellCmp(const Cell& a, const Cell& b, double cx, double cy) {
  double da = atan2(a.y - cy, a.x - cx) - atan2(b.y - cy, b.x - cx);
  if (da != 0.0) return da;
  return hypot(a.x - cx, a.y - cy) - hypot(b.x - cx, b.y - cy);
}

void sortByAngle(int* idx, int n, const Cell* cells, double cx, double cy) {
  for (int i = 1; i < n; i++) {
    int j = i;
    while (j > 0 && cellCmp(cells[idx[j - 1]], cells[idx[j]], cx, cy) > 0.0) {
      int tmp = idx[j - 1]; idx[j - 1] = idx[j]; idx[j] = tmp;
      j--;
    }
  }
}

}  // namespace

// ============================================================================
// Spring
// ============================================================================
void Spring::init(float x0, float stiffness, float zeta) {
  x = x0; v = 0.0f; t = x0;
  k = stiffness;
  c = 2.0f * zeta * sqrtf(stiffness);
}

void Spring::step(float dt) {
  float a = -k * (x - t) - c * v;
  v += a * dt;
  x += v * dt;
}

bool Spring::still() const {
  return fabsf(x - t) < 0.005f && fabsf(v) < 0.01f;
}

void Spring::snap(float x0) {
  x = x0; t = x0; v = 0.0f;
}

// ============================================================================
// Toggle - setup
// ============================================================================
float Toggle::rnd() {
  // xorshift32: tiny, fast, good enough for visual randomness
  uint32_t x = rng_;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rng_ = x;
  return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void Toggle::begin(Theme start, uint32_t seed) {
  rng_ = seed ? seed : 0x9E3779B9u;
  c_.init(REST_DARK, 420.0f, 0.84f);      // knob centre
  w_.init(KNOB,      520.0f, 0.55f);      // knob width (underdamped = squash on arrival)
  p_.init(0.0f,     1400.0f, 1.00f);      // theme progress, snappy
  a_.init(ANGLE_DARK, 110.0f, 0.80f);     // rim angle
  fl_.init(0.0f,     260.0f, 1.00f);      // rim flash
  theme_ = start;
  pressed_ = false;
  acc_ = 0.0f;
  simTime_ = 0.0f;
  epoch_ = 0;
  n_ = 0;
  morphOn_ = false;
  morphMs_ = 0.0f;
  for (int i = 0; i < 4; i++) queue_[i].used = false;
  buildParticles();
  snapAll();
}

void Toggle::buildParticles() {
  static const double P = PITCH;
  Cell moon[MAX_MOON];
  Cell sun[MAX_SUN];
  int nm = 0, ns = 0;

  // ---- moon: a crescent cut from a disc, plus faint "ghost" cells in the cut, plus stars ----
  for (int r = -6; r <= 6; r++) {
    for (int c = -6; c <= 6; c++) {
      double d1 = hypot((double)c, (double)r);
      double dc = hypot(c - 3.2, r + 2.4);
      if (d1 <= 5.4 && dc > 4.7) {
        if (nm < MAX_MOON) { Cell k = mkCell(c * P, r * P, 1.0, ROLE_BRIGHT); moon[nm++] = k; }
      } else if (d1 <= 6.2 && dc <= 5.0 && (c + r) % 2 == 0) {
        double cut = 4.7 - dc; if (cut < 0.0) cut = 0.0;
        double a = 0.06 + 0.42 * exp(-cut / 1.7);
        if (nm < MAX_MOON) { Cell k = mkCell(c * P, r * P, a, ROLE_GHOST); moon[nm++] = k; }
      }
    }
  }
  static const double STARS[7][3] = {
    {6, -6, 1.0}, {5, -6, 0.55}, {7, -6, 0.55}, {6, -7, 0.55}, {6, -5, 0.55}, {3, -7, 0.5}, {7, -3, 0.45}
  };
  for (int i = 0; i < 7; i++) {
    if (nm < MAX_MOON) { Cell k = mkCell(STARS[i][0] * P, STARS[i][1] * P, STARS[i][2], ROLE_STAR); moon[nm++] = k; }
  }

  // ---- sun: a round core of square cells + rays in 8 directions ----
  for (double r = -2.5; r <= 2.5; r += 1.0) {
    for (double c = -2.5; c <= 2.5; c += 1.0) {
      if (hypot(c, r) <= 3.5 && ns < MAX_SUN) { Cell k = mkCell(c * P, r * P, 1.0, ROLE_CORE); sun[ns++] = k; }
    }
  }
  static const double RAY_D[3] = { 5.3, 6.3, 7.3 };
  for (int i = 0; i < 3; i++) {
    double d = RAY_D[i];
    Cell k1 = mkCell(0.0, -d * P, 0.92, ROLE_RAY);
    Cell k2 = mkCell(0.0,  d * P, 0.92, ROLE_RAY);
    Cell k3 = mkCell(-d * P, 0.0, 0.92, ROLE_RAY);
    Cell k4 = mkCell( d * P, 0.0, 0.92, ROLE_RAY);
    if (ns + 4 <= MAX_SUN) { sun[ns++] = k1; sun[ns++] = k2; sun[ns++] = k3; sun[ns++] = k4; }
  }
  static const double DIAG_D[2] = { 3.6, 4.5 };
  for (int i = 0; i < 2; i++) {
    double d = DIAG_D[i];
    for (int sx = -1; sx <= 1; sx += 2) {
      for (int sy = -1; sy <= 1; sy += 2) {
        Cell k = mkCell(sx * d * P, sy * d * P, 0.92, ROLE_RAY);
        if (ns < MAX_SUN) sun[ns++] = k;
      }
    }
  }

  // ---- pair the bright moon cells with sun cells by angle rank, so the swirl is orderly ----
  int brightIdx[MAX_MOON];
  int nb = 0;
  double mcx = 0.0, mcy = 0.0;
  for (int i = 0; i < nm; i++) {
    if (moon[i].role == ROLE_BRIGHT) { brightIdx[nb++] = i; mcx += moon[i].x; mcy += moon[i].y; }
  }
  if (nb > 0) { mcx /= nb; mcy /= nb; }
  sortByAngle(brightIdx, nb, moon, mcx, mcy);

  int sunIdx[MAX_SUN];
  for (int i = 0; i < ns; i++) sunIdx[i] = i;
  sortByAngle(sunIdx, ns, sun, 0.0, 0.0);

  bool moonUsed[MAX_MOON];
  bool sunUsed[MAX_SUN];
  for (int i = 0; i < nm; i++) moonUsed[i] = false;
  for (int i = 0; i < ns; i++) sunUsed[i] = false;

  n_ = 0;
  int pairs = nb < ns ? nb : ns;
  for (int i = 0; i < pairs && n_ < MAX_PARTICLES; i++) {
    int mi = brightIdx[(i * nb) / pairs];
    int si = sunIdx[(i * ns) / pairs];
    moonUsed[mi] = true; sunUsed[si] = true;
    Particle& p = parts_[n_++];
    p.m.x = (float)moon[mi].x; p.m.y = (float)moon[mi].y; p.m.a = (float)moon[mi].a; p.m.role = moon[mi].role; p.m.used = true;
    p.s.x = (float)sun[si].x;  p.s.y = (float)sun[si].y;  p.s.a = (float)sun[si].a;  p.s.role = sun[si].role;  p.s.used = true;
  }
  for (int i = 0; i < nm && n_ < MAX_PARTICLES; i++) {
    if (moonUsed[i]) continue;
    Particle& p = parts_[n_++];
    p.m.x = (float)moon[i].x; p.m.y = (float)moon[i].y; p.m.a = (float)moon[i].a; p.m.role = moon[i].role; p.m.used = true;
    p.s.x = p.s.y = p.s.a = 0.0f; p.s.role = 0; p.s.used = false;
  }
  for (int i = 0; i < ns && n_ < MAX_PARTICLES; i++) {
    if (sunUsed[i]) continue;
    Particle& p = parts_[n_++];
    p.s.x = (float)sun[i].x; p.s.y = (float)sun[i].y; p.s.a = (float)sun[i].a; p.s.role = sun[i].role; p.s.used = true;
    p.m.x = p.m.y = p.m.a = 0.0f; p.m.role = 0; p.m.used = false;
  }

  for (int i = 0; i < n_; i++) {
    Particle& p = parts_[i];
    p.x = p.y = p.a = 0.0f;
    p.cloudX = p.cloudY = p.cloudA = 0.0f;
    p.fromX = p.fromY = p.fromA = 0.0f;
    p.toX = p.toY = p.toA = 0.0f;
    p.d1 = p.d2 = p.curl = 0.0f;
    p.phase = rnd() * 6.28f;
    p.star = p.m.used && p.m.role == ROLE_STAR;
  }
}

// ============================================================================
// Toggle - state changes
// ============================================================================
void Toggle::setRest(Theme t) {
  for (int i = 0; i < n_; i++) {
    Particle& p = parts_[i];
    const Slot& s = (t == THEME_LIGHT) ? p.s : p.m;
    p.x = s.used ? s.x : 0.0f;
    p.y = s.used ? s.y : 0.0f;
    p.a = s.used ? s.a : 0.0f;
  }
}

void Toggle::snapAll() {
  c_.snap(theme_ == THEME_LIGHT ? REST_LIGHT : REST_DARK);
  w_.snap(KNOB);
  p_.snap(theme_ == THEME_LIGHT ? 1.0f : 0.0f);
  a_.snap(theme_ == THEME_LIGHT ? ANGLE_LIGHT : ANGLE_DARK);
  fl_.snap(0.0f);
  morphOn_ = false;
  for (int i = 0; i < 4; i++) queue_[i].used = false;
  setRest(theme_);
}

float Toggle::centerTarget() const {
  float rest = (theme_ == THEME_LIGHT) ? REST_LIGHT : REST_DARK;
  if (!pressed_) return rest;
  return rest + (theme_ == THEME_DARK ? 5.0f : -5.0f);   // lean into the press
}

// Rim always turns counter-clockwise, at least 20 degrees, never reversing mid-flight.
float Toggle::ccwTarget(float cur, float base) const {
  float d = fmodf(cur - base, 360.0f);
  if (d < 0.0f) d += 360.0f;
  if (d >= 360.0f) d -= 360.0f;
  if (d < 20.0f) d += 360.0f;
  return cur - d;
}

void Toggle::later(float ms, uint8_t kind) {
  for (int i = 0; i < 4; i++) {
    if (!queue_[i].used) {
      queue_[i].used = true;
      queue_[i].left = ms / 1000.0f;
      queue_[i].kind = kind;
      queue_[i].tok = epoch_;
      return;
    }
  }
}

void Toggle::setPressed(bool p) {
  if (pressed_ == p) return;
  pressed_ = p;
  c_.t = centerTarget();
}

bool Toggle::setTheme(Theme next, bool instant) {
  if (next == theme_) return false;
  theme_ = next;
  epoch_++;
  // Any delayed event from an earlier tap is stale now: drop them all (their token would not match anyway).
  for (int i = 0; i < 4; i++) queue_[i].used = false;

  if (instant) {
    pressed_ = false;
    snapAll();
    return true;
  }
  c_.t = centerTarget();
  later(T_RIM_MS,  EV_RIM);
  later(T_FLIP_MS, EV_FLIP);
  startMorph(theme_);
  return true;
}

// ---- morph: cur -> cloud -> target, per-particle staggered ----
void Toggle::startMorph(Theme to) {
  for (int i = 0; i < n_; i++) {
    Particle& p = parts_[i];
    const Slot& tgt = (to == THEME_LIGHT) ? p.s : p.m;
    float ang = rnd() * 6.283f;
    float rad = 5.0f + sqrtf(rnd()) * 30.0f;
    p.cloudX = cosf(ang) * rad;
    p.cloudY = 4.0f + sinf(ang) * rad * 0.85f;
    p.fromX = p.x; p.fromY = p.y; p.fromA = p.a;
    if (tgt.used) { p.toX = tgt.x; p.toY = tgt.y; p.toA = tgt.a; }
    else          { p.toX = p.cloudX; p.toY = p.cloudY; p.toA = 0.0f; }
    if (p.fromA < 0.02f) { p.fromX = p.cloudX; p.fromY = p.cloudY; }
    p.cloudA = (p.fromA > 0.02f && p.toA > 0.02f) ? 1.0f : 0.0f;
    bool ray = tgt.used && tgt.role == ROLE_RAY;
    p.d1 = rnd() * 0.05f;
    p.d2 = ray ? 0.09f + rnd() * 0.12f : rnd() * 0.08f;
    p.curl = rnd() * 2.0f - 1.0f;
  }
  morphMs_ = 0.0f;
  morphOn_ = true;
}

namespace {
// Point a fraction s of the way from A to B, bent sideways into a gentle arc.
void curve(float ax, float ay, float bx, float by, float s, float curl, float* ox, float* oy) {
  float dx = bx - ax, dy = by - ay;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1e-6f) len = 1.0f;
  float k = sinf(3.14159265f * clampf(s, 0.0f, 1.0f)) * len * 0.22f * curl;
  *ox = lerpf(ax, bx, s) - dy / len * k;
  *oy = lerpf(ay, by, s) + dx / len * k;
}
}  // namespace

void Toggle::evalMorph(Particle& p, float tn) {
  float x, y;
  if (tn < M1) {
    float s = easeInOut(clampf((tn - p.d1) / (M1 - p.d1), 0.0f, 1.0f));
    curve(p.fromX, p.fromY, p.cloudX, p.cloudY, s, p.curl, &x, &y);
    p.x = x;
    p.y = y + 3.5f * sinf(3.14159265f * tn / M1);       // slight sag as it dissolves
    p.a = lerpf(p.fromA, p.cloudA, s);
  } else if (tn < M2) {
    p.x = p.cloudX + sinf(simTime_ * 41.0f + p.phase) * 1.3f;
    p.y = p.cloudY + cosf(simTime_ * 37.0f + p.phase) * 1.3f;
    p.a = p.cloudA;
  } else {
    float u = clampf((tn - M2 - p.d2) / (1.0f - M2 - p.d2), 0.0f, 1.0f);
    float s = easeOutBack(u);
    curve(p.cloudX, p.cloudY, p.toX, p.toY, s, -p.curl * 0.6f, &x, &y);
    p.x = x; p.y = y;
    p.a = lerpf(p.cloudA, p.toA, clampf(u * 1.6f, 0.0f, 1.0f));
  }
}

// ============================================================================
// Toggle - the clock
// ============================================================================
void Toggle::stepOnce(float h) {
  // the knob stretches with its speed (and swells a little while pressed)
  float sp = fabsf(c_.v) / STRETCH_VREF;
  if (sp > 1.0f) sp = 1.0f;
  w_.t = KNOB + STRETCH_AMP * sp + (pressed_ ? 10.0f : 0.0f);

  c_.step(h); w_.step(h); p_.step(h); a_.step(h); fl_.step(h);
  fl_.t = 0.0f;

  simTime_ += h;
  if (simTime_ >= SIM_WRAP) simTime_ -= SIM_WRAP;

  if (morphOn_) {
    morphMs_ += h * 1000.0f;
    if (morphMs_ >= T_MORPH_MS) { morphOn_ = false; setRest(theme_); }
  }

  for (int i = 0; i < 4; i++) {
    Event& e = queue_[i];
    if (!e.used) continue;
    e.left -= h;
    if (e.left > 0.0f) continue;
    e.used = false;
    if (e.tok != epoch_) continue;                      // an even newer tap replaced this one
    if (e.kind == EV_RIM) {
      a_.t = ccwTarget(a_.x, theme_ == THEME_LIGHT ? ANGLE_LIGHT : ANGLE_DARK);
    } else if (e.kind == EV_FLIP) {
      p_.t = (theme_ == THEME_LIGHT) ? 1.0f : 0.0f;
      fl_.x = 1.0f; fl_.v = 0.0f;                       // white rim flash
    }
  }
}

// The rim angle only ever decreases (always counter-clockwise). After many
// flips the number would grow huge and floats would lose precision (the spring
// would jitter and never come to rest). A spring only cares about the GAP between
// its position and its target, so we can slide both by whole turns of 360 degrees
// at any moment: same picture, same motion, small numbers.
void Toggle::normalizeRimAngle() {
  if (a_.x >= -360.0f && a_.x < 720.0f) return;
  float shift = 360.0f * floorf(a_.x / 360.0f);
  a_.x -= shift;
  a_.t -= shift;
}

namespace {
// Floats cannot represent a spring that is "almost exactly" at rest forever: the last
// few thousandths wobble. Once a spring is closer than these tolerances (far below what
// one screen pixel can show) we put it exactly on its target so the animation can go idle.
void settleSpring(Spring& s, float xTol, float vTol) {
  if (fabsf(s.x - s.t) < xTol && fabsf(s.v) < vTol) { s.x = s.t; s.v = 0.0f; }
}
}  // namespace

void Toggle::update(float dtSeconds) {
  if (dtSeconds <= 0.0f) return;
  if (dtSeconds > 0.05f) dtSeconds = 0.05f;             // after a stall: do not fast-forward
  acc_ += dtSeconds;
  while (acc_ >= SUB) {
    acc_ -= SUB;
    stepOnce(SUB);
  }
  if (morphOn_) {
    float tn = morphMs_ / T_MORPH_MS;
    for (int i = 0; i < n_; i++) evalMorph(parts_[i], tn);
  }
  normalizeRimAngle();
  settleSpring(c_, 0.01f, 0.05f);     // design pixels, px/s
  // The knob width follows the knob's speed. Once the centre is snapped to rest its
  // speed is 0, so refresh the width target BEFORE snapping the width too; otherwise
  // the width could freeze 0.001 px off its final value and the screen would keep it.
  w_.t = KNOB + (pressed_ ? 10.0f : 0.0f) + STRETCH_AMP * fminf(1.0f, fabsf(c_.v) / STRETCH_VREF);
  settleSpring(w_, 0.01f, 0.05f);
  settleSpring(p_, 0.0005f, 0.01f);   // 0..1
  settleSpring(fl_, 0.001f, 0.01f);
  settleSpring(a_, 0.02f, 0.2f);      // degrees, deg/s
}

bool Toggle::busy() const {
  if (morphOn_) return true;
  for (int i = 0; i < 4; i++) if (queue_[i].used) return true;
  return !(c_.still() && w_.still() && p_.still() && a_.still() && fl_.still());
}

float Toggle::knobScaleY() const {
  float stretch = w_.x / KNOB;
  return clampf(1.0f - 0.4f * (stretch - 1.0f), 0.85f, 1.06f);
}

float Toggle::themeProgress() const { return clampf(p_.x, 0.0f, 1.0f); }
float Toggle::flash() const { return clampf(fl_.x, 0.0f, 1.4f); }

// ============================================================================
// Slider (the second screen)
// ============================================================================
namespace {
// Slider values run 0..1 along the whole slider, so the speeds below are "whole sliders per second".
const float SL_STRETCH_VREF = 3.0f;       // speed at which the knob is fully stretched
const float SL_POS_TOL = 0.0006f;         // about a tenth of a pixel on the longest slider
const float SL_V_TOL   = 0.01f;
}

void Slider::begin(float value) {
  value = clampf(value, 0.0f, 1.0f);
  pos_.init(value, 420.0f, 0.84f);        // follows the finger with a soft overshoot
  st_.init(0.0f,   520.0f, 0.55f);        // stretch: underdamped, so the knob wobbles when it arrives
  pr_.init(0.0f,   900.0f, 0.70f);        // press-in
  gl_.init(0.25f,  200.0f, 0.90f);        // rim glow: dim at rest, bright while held
  target_ = value;
  pressed_ = false;
  acc_ = 0.0f;
}

void Slider::setTarget(float value) {
  target_ = clampf(value, 0.0f, 1.0f);
  pos_.t = target_;
}

void Slider::setPressed(bool p) {
  pressed_ = p;
  pr_.t = p ? 1.0f : 0.0f;
  gl_.t = p ? 1.0f : 0.25f;
}

void Slider::update(float dtSeconds) {
  if (dtSeconds <= 0.0f) return;
  if (dtSeconds > 0.05f) dtSeconds = 0.05f;
  acc_ += dtSeconds;
  while (acc_ >= SUB) {
    acc_ -= SUB;
    st_.t = fminf(1.0f, fabsf(pos_.v) / SL_STRETCH_VREF);
    pos_.step(SUB); st_.step(SUB); pr_.step(SUB); gl_.step(SUB);
  }
  if (fabsf(pos_.x - pos_.t) < SL_POS_TOL && fabsf(pos_.v) < SL_V_TOL) { pos_.x = pos_.t; pos_.v = 0.0f; }
  st_.t = fminf(1.0f, fabsf(pos_.v) / SL_STRETCH_VREF);
  if (fabsf(st_.x - st_.t) < 0.002f && fabsf(st_.v) < 0.02f) { st_.x = st_.t; st_.v = 0.0f; }
  if (fabsf(pr_.x - pr_.t) < 0.002f && fabsf(pr_.v) < 0.02f) { pr_.x = pr_.t; pr_.v = 0.0f; }
  if (fabsf(gl_.x - gl_.t) < 0.002f && fabsf(gl_.v) < 0.02f) { gl_.x = gl_.t; gl_.v = 0.0f; }
}

bool Slider::busy() const {
  auto moving = [](const Spring& s, float xTol, float vTol) { return fabsf(s.x - s.t) >= xTol || fabsf(s.v) >= vTol; };
  return moving(pos_, SL_POS_TOL, SL_V_TOL) || moving(st_, 0.002f, 0.02f) ||
         moving(pr_, 0.002f, 0.02f) || moving(gl_, 0.002f, 0.02f);
}

}  // namespace tog
