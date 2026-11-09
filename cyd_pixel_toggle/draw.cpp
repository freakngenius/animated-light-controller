// draw.cpp - paints the toggle onto the CYD screen.
//
// ---------------------------------------------------------------------------
// The big idea, in plain English
// ---------------------------------------------------------------------------
// * The web version (theme-toggle/index.html) is built from CSS layers: a page
//   background, a soft coloured "halo", the pill-shaped track, the square knob
//   with a glow inside it, a pixel-art moon/sun, and a neon "rim" arc.
// * A browser has a GPU for blur and transparency. The ESP32 does not, so every
//   soft effect is faked:
//     - big soft gradients  -> one solid colour per screen row, or a few
//                              concentric solid "steps" (like contour lines)
//     - transparency        -> we work out the mixed colour ourselves first
//                              ("pre-mixing") and then paint a solid colour
//     - blur / glow         -> a cheap bell-curve lookup table, only on the
//                              few thousand pixels close to the rim/track edge
// * Everything is painted into an off-screen picture in RAM (a "sprite"), then
//   sent to the screen in one go, so you never see half-drawn frames.
//   To save RAM that picture can be cut into 1, 2 or 4 horizontal strips
//   ("bands", SPRITE_BANDS in config.h). Each strip is painted and sent in turn.
// * We paint straight into the sprite's memory with our own tiny drawing
//   routines (instead of the library's fillRect etc.). That gives us smooth
//   (anti-aliased) edges and exact colour control, and it is very fast.
//   The library is only used to create the sprite and send it to the screen.
//
// Units: "design px" = the web version's 392 x 198 space. Screen px = design px
// x SCALE (0.70). The pixel icon uses its own grid (see ICON_PITCH below).
// ---------------------------------------------------------------------------
#include "draw.h"
#include "display_config.h"   // class LGFX (and LovyanGFX itself)
#include "layout.h"           // where things sit on screen two
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  // The Arduino build compiles everything with -Os (smallest code). This file is the hot
  // path of the animation, so build it for speed instead.
  #pragma GCC optimize ("O3")
#else
  #include <chrono>
#endif

namespace gfx {

// ===========================================================================
// 1. Layout constants (screen pixels)
// ===========================================================================
namespace {

const int   SCR_W = 320;
const int   SCR_H = 240;

// Track (the pill) on screen: 274.4 x 138.6 px, centred. We start it on whole
// pixels so the left/top edges are crisp.
const float OX = 23.0f;                       // screen x of design x = 0
const float OY = 51.0f;                       // screen y of design y = 0
const float S  = SCALE;                       // 0.70

// The sprite covers the track plus a ~10 px margin all round (for the glow).
const int   REG_X  = 13;                      // left edge of the sprite area on screen
const int   REG_Y  = 41;                      // top edge
const int   REG_W  = 294;                     // width  (13 .. 306)
const int   REG_H1 = 159;                     // height when it is ONE piece (41 .. 199)

// The pixel icon. The web cells sit 6.5 design px apart (= 4.55 screen px).
// 4.55 is not a whole number, so snapped cells would sit 4 or 5 px apart
// (uneven gaps). Instead we put the icon on an exact 5 px grid with 4 x 4 px
// cells: every cell is a crisp square with a clean 1 px gap. (The icon ends
// up ~10% bigger than a straight 0.70 scale - it still fits the knob easily.)
const int   ICON_PITCH = 5;                   // screen px between cell centres
const int   ICON_CELL  = 4;                   // cell size, screen px
const float ICON_K     = (float)ICON_PITCH / tog::PITCH;   // design px -> icon px

// Status dot (top-right corner, outside the sprite)
const int   DOT_X = SCR_W - 12;
const int   DOT_Y = 12;
const int   DOT_R = 3;

// ===========================================================================
// 2. Small maths + colour helpers
// ===========================================================================
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int   imin(int a, int b) { return a < b ? a : b; }
inline int   imax(int a, int b) { return a > b ? a : b; }

// A colour with 0..255 float channels (floats so mixing is exact; converted to
// the screen's 16-bit format only at the very end).
struct Col { float r, g, b; };

inline Col mixc(const Col& a, const Col& b, float t) {
  Col o = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
  return o;
}
inline Col scalec(const Col& a, float k) {          // brighten (k > 1), clamped
  Col o = { fminf(255.0f, a.r * k), fminf(255.0f, a.g * k), fminf(255.0f, a.b * k) };
  return o;
}

// Our own RGB565 packer: 5 bits red, 6 bits green, 5 bits blue (with rounding).
// This is the "uint16_t colour" LovyanGFX expects.
inline uint16_t rgb565(int r, int g, int b) {
  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  b = b < 0 ? 0 : (b > 255 ? 255 : b);
  return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) | ((b * 31 + 127) / 255));
}
inline uint16_t pack(const Col& c) {
  return rgb565((int)(c.r + 0.5f), (int)(c.g + 0.5f), (int)(c.b + 0.5f));
}
inline Col unpack(uint16_t v) {
  Col c = { (float)((v >> 11) & 31) * (255.0f / 31.0f),
            (float)((v >> 5) & 63) * (255.0f / 63.0f),
            (float)(v & 31) * (255.0f / 31.0f) };
  return c;
}
inline Col hex(uint32_t h) {
  Col c = { (float)((h >> 16) & 255), (float)((h >> 8) & 255), (float)(h & 255) };
  return c;
}

// ---- integer helpers for the hot loops (this chip does floating point slowly) ----
// Square root without a library call: bit-trick first guess + 2 Newton steps (error ~5e-6).
inline float fsqrtf(float x) {
  if (x <= 1e-12f) return 0.0f;
  uint32_t i; memcpy(&i, &x, 4);
  i = 0x5f3759dfu - (i >> 1);
  float y; memcpy(&y, &i, 4);
  float h = 0.5f * x;
  y = y * (1.5f - h * y * y);
  y = y * (1.5f - h * y * y);
  return x * y;
}
// Alpha maths on whole numbers. An "alpha" here is 0..255 (or 0..256 once passed through a256()).
inline int a256(int a) { return a + (a >> 7); }                       // 0..255 -> 0..256
inline int mulA(int v, int a) { return (v * a256(a)) >> 8; }          // v * a / 255
inline int mix8(int c, int t, int a /* 0..256 */) { return c + (((t - c) * a + 128) >> 8); }

// A theme "token" = the dark value and the light value of one CSS variable,
// each with an opacity. color-mix() in the CSS blends them with --p.
struct Tok { uint32_t dark; float da; uint32_t light; float la; };

// Blend a token like CSS color-mix(in srgb, dark (1-p), light) does. For see-through
// colours the browser mixes "pre-multiplied" (colour x opacity), so we do too.
inline void tokMix(const Tok& t, float p, Col* out, float* alpha) {
  Col d = hex(t.dark), l = hex(t.light);
  float wa = t.da * (1.0f - p), wb = t.la * p;
  float a = wa + wb;
  if (a < 1e-5f) { *out = mixc(d, l, p); *alpha = 0.0f; return; }
  Col o = { (d.r * wa + l.r * wb) / a, (d.g * wa + l.g * wb) / a, (d.b * wa + l.b * wb) / a };
  *out = o; *alpha = a;
}
inline Col tokCol(const Tok& t, float p) { Col c; float a; tokMix(t, p, &c, &a); return c; }

// ---- every colour token from the CSS :root block (lines 53-86 of index.html) ----
const Tok T_BG_HI     = { 0x1b1d20, 1.0f,  0xe7e3de, 1.0f };
const Tok T_BG_LO     = { 0x08090b, 1.0f,  0xcbc9c6, 1.0f };
const Tok T_TRACK_TOP = { 0x040407, 1.0f,  0xe3dfdc, 1.0f };
const Tok T_TRACK_BOT = { 0x08090d, 1.0f,  0xe9e5e2, 1.0f };
const Tok T_TRACK_IN  = { 0x000000, 0.95f, 0x60584e, 0.34f };
const Tok T_TRACK_LIFT= { 0xffffff, 0.04f, 0xffffff, 0.95f };
const Tok T_TRACK_EDGE= { 0xffffff, 0.035f,0x968c80, 0.28f };
const Tok T_TRACK_DROP= { 0x000000, 0.65f, 0x6e6456, 0.36f };
const Tok T_K_TOP     = { 0x2a2b2f, 1.0f,  0xf8f6f3, 1.0f };
const Tok T_K_BOT     = { 0x1f2024, 1.0f,  0xebe8e4, 1.0f };
const Tok T_K_GLOW    = { 0x8d99ff, 1.0f,  0xffb02e, 1.0f };
const Tok T_K_HI      = { 0xffffff, 0.16f, 0xffffff, 0.95f };
const Tok T_K_EDGE    = { 0xffffff, 0.07f, 0xbeb2a0, 0.35f };
const Tok T_K_DROP    = { 0x4654ff, 0.30f, 0xffa01e, 0.34f };
const Tok T_WALL_A    = { 0x1a1b1e, 1.0f,  0xdedad4, 1.0f };
const Tok T_WALL_B    = { 0x111214, 1.0f,  0xbdb4a6, 1.0f };
const Tok T_RIM_CORE  = { 0xc4cbff, 1.0f,  0xffb21c, 1.0f };
const Tok T_RIM_HOT   = { 0xffffff, 1.0f,  0xfff1b8, 1.0f };
const Tok T_RIM_GLOW  = { 0x6c7aff, 1.0f,  0xff9a10, 1.0f };
const Tok T_HALO      = { 0x5462ff, 0.44f, 0xffa01e, 0.40f };
// pixel ink (JS: INK.dark / INK.light) and the moon's bloom colour (JS shadowColor)
const uint32_t INK_DARK  = 0xf2ede6;
const uint32_t INK_LIGHT = 0x38322b;
const uint32_t BLOOM_RGB = 0xd7deff;

// ===========================================================================
// 3. Rounded rectangles: the one shape everything here is made of
// ===========================================================================
// Stored as centre, half-width, half-height and corner radius (screen px, floats
// so shapes can sit between pixels and slide smoothly).
struct RBox { float cx, cy, hw, hh, r; };

// Signed distance from a point to the box outline: negative inside, positive
// outside, in pixels. This is what gives us smooth edges and glow falloff.
inline float sdBox(const RBox& b, float px, float py) {
  float qx = fabsf(px - b.cx) - (b.hw - b.r);
  float qy = fabsf(py - b.cy) - (b.hh - b.r);
  float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
  float out = (ox > 0.0f && oy > 0.0f) ? fsqrtf(ox * ox + oy * oy) : (ox + oy);
  float in = qx > qy ? qx : qy;
  if (in > 0.0f) in = 0.0f;
  return out + in - b.r;
}

// Half of the box's width on screen row `py`, after growing the box by `grow` px
// (negative = shrink). Returns -1 when the row misses the box.
inline float halfExtent(const RBox& b, float py, float grow) {
  float hh = b.hh + grow, hw = b.hw + grow, r = b.r + grow;
  if (r < 0.0f) r = 0.0f;
  float dy = fabsf(py - b.cy);
  if (dy >= hh) return -1.0f;
  float c0 = hh - r;
  if (dy <= c0) return hw;
  float t = dy - c0, s = r * r - t * t;
  return hw - r + (s > 0.0f ? sqrtf(s) : 0.0f);
}

// ===========================================================================
// 4. The sprite and our tiny pixel painter
// ===========================================================================
// Two picture buffers take turns: while one is being sent to the screen (by DMA, in the
// background) the next strip is painted into the other. With SPRITE_BANDS 1 there is just one.
lgfx::LGFX_Sprite g_spr[2];
uint16_t* g_bufs[2] = { 0, 0 };  // the pixel memory of each buffer
uint16_t* g_buf    = 0;         // the buffer the current strip is painted into
uint8_t   g_nbuf   = 0;         // 1 or 2 buffers
int       g_lastPushed = -1;    // the buffer sent to the screen last (it may still be on its way)
bool      g_txOpen = false;     // the screen's write session is kept open across frames
uint8_t   g_bands  = 0;         // 1, 2 or 4
int       g_bandH  = 0;         // rows per band
int       g_regH   = 0;         // g_bandH * g_bands (159 or 160)
bool      g_swap   = true;      // LovyanGFX keeps 16-bit sprite pixels byte-swapped; begin() checks
int       g_bandY  = 0;         // screen y of the band currently being painted
int       g_ox     = REG_X;     // screen x of the buffer's first column (screen one: REG_X; screen two moves it)

// The clip rectangle (screen coords, end-exclusive) for the band being painted:
// band rows intersected with the "dirty" rectangle. Nothing is written outside it.
int cX0 = 0, cY0 = 0, cX1 = 0, cY1 = 0;

inline uint16_t toBuf(uint16_t c)   { return g_swap ? (uint16_t)((c >> 8) | (c << 8)) : c; }
inline uint16_t fromBuf(uint16_t v) { return g_swap ? (uint16_t)((v >> 8) | (v << 8)) : v; }
inline uint16_t* at(int x, int y)   { return g_buf + (y - g_bandY) * REG_W + (x - g_ox); }

// 8-bit channels -> the 16-bit value as it sits in the buffer. Three small tables, OR-ed together
// (already byte-swapped when the library wants that). Built in begin() once g_swap is known.
uint16_t g_pkR[256], g_pkG[256], g_pkB[256];
inline uint16_t pk(int r, int g, int b) { return (uint16_t)(g_pkR[r] | g_pkG[g] | g_pkB[b]); }
void buildPackTables() {
  for (int i = 0; i < 256; i++) {
    g_pkR[i] = toBuf((uint16_t)(((i * 31 + 127) / 255) << 11));
    g_pkG[i] = toBuf((uint16_t)(((i * 63 + 127) / 255) << 5));
    g_pkB[i] = toBuf((uint16_t)((i * 31 + 127) / 255));
  }
}
// A buffer pixel -> 8-bit channels.
inline void ld(uint16_t v, int* r, int* g, int* b) {
  uint32_t s = fromBuf(v);
  int r5 = (int)(s >> 11), g6 = (int)((s >> 5) & 63), b5 = (int)(s & 31);
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

// Paint pixels x0..x1 (inclusive) of row y with one solid colour (clipped).
// Two pixels per store once the row is 32-bit aligned.
typedef uint32_t __attribute__((may_alias)) u32a;
inline void hline(int y, int x0, int x1, uint16_t c) {
  if (y < cY0 || y >= cY1) return;
  if (x0 < cX0) x0 = cX0;
  if (x1 >= cX1) x1 = cX1 - 1;
  if (x0 > x1) return;
  uint16_t s = toBuf(c);
  uint16_t* p = at(x0, y);
  int n = x1 - x0 + 1;
  if (((uintptr_t)p & 2u) != 0u) { *p++ = s; n--; }
  uint32_t s2 = ((uint32_t)s << 16) | s;
  u32a* q = (u32a*)p;
  for (int i = n >> 1; i > 0; i--) *q++ = s2;
  if (n & 1) *(uint16_t*)q = s;
}
// Same, but skip the columns ex0..ex1 (used to avoid painting under the knob).
inline void hlineSkip(int y, int x0, int x1, int ex0, int ex1, uint16_t c) {
  if (ex0 > ex1) { hline(y, x0, x1, c); return; }
  hline(y, x0, imin(x1, ex0 - 1), c);
  hline(y, imax(x0, ex1 + 1), x1, c);
}
inline bool inClip(int x, int y) { return x >= cX0 && x < cX1 && y >= cY0 && y < cY1; }
// Blend one pixel toward colour c by amount a (0..1). Used only for smooth
// edges and the rim glow - a few thousand pixels per frame at most.
inline void blendPx(int x, int y, const Col& c, float a) {
  if (a <= 0.0f || !inClip(x, y)) return;
  uint16_t* p = at(x, y);
  if (a >= 0.999f) { *p = toBuf(pack(c)); return; }
  *p = toBuf(pack(mixc(unpack(fromBuf(*p)), c, a)));
}

// Walk a rounded box row by row (only rows inside the clip).
//   fill(y, x0, x1)  : solid inside part of a row, already clipped to the clip rect
//   edge(x, y, d)    : one pixel on the soft edge, d = signed distance (-0.5..0.5 = partly covered)
// `inner` = how far inside the outline a pixel must be to count as "solid" (-0.5 normally).
template <class FillFn, class EdgeFn>
void rasterBox(const RBox& b, float inner, FillFn fill, EdgeFn edge) {
  int y0 = imax(cY0, (int)floorf(b.cy - b.hh - 1.0f));
  int y1 = imin(cY1 - 1, (int)ceilf(b.cy + b.hh + 1.0f));
  for (int y = y0; y <= y1; y++) {
    float py = y + 0.5f;
    float ho = halfExtent(b, py, 0.5f);
    if (ho < 0.0f) continue;
    float hi = halfExtent(b, py, inner);
    int xo0 = (int)floorf(b.cx - ho - 0.5f), xo1 = (int)ceilf(b.cx + ho - 0.5f);
    int xi0 = 1, xi1 = 0;                                   // empty unless the row has a solid part
    if (hi >= 0.0f) { xi0 = (int)ceilf(b.cx - hi - 0.5f); xi1 = (int)floorf(b.cx + hi - 0.5f); }
    if (xi0 > xi1) { xi0 = xo1 + 1; xi1 = xo1; }            // whole row is "edge"
    for (int x = imax(xo0, cX0); x <= imin(xi0 - 1, cX1 - 1); x++) edge(x, y, sdBox(b, x + 0.5f, py));
    for (int x = imax(xi1 + 1, cX0); x <= imin(xo1, cX1 - 1); x++) edge(x, y, sdBox(b, x + 0.5f, py));
    int f0 = imax(xi0, cX0), f1 = imin(xi1, cX1 - 1);
    if (f0 <= f1) fill(y, f0, f1);
  }
}

// ===========================================================================
// 5. Lookup tables (built once in begin())
// ===========================================================================
uint8_t  g_gauss[64];           // exp(-x^2) for x = 0 .. 4 in steps of 1/16, scaled to 255
uint8_t  g_haloLut[128];        // (1 - sqrt(u)) for u = 0..1: the halo's radial fade
uint16_t g_backRow[SCR_H];      // the page background colour of every screen row
uint8_t  g_backRGB[SCR_H][3];   // the same colour as 8-bit channels (for mixing in the margin)
int      g_fade256[11];         // regionFade() as 0..256, for a border distance of 0..10 px
float    g_colU[REG_W];         // halo distance (squared, scaled) of each pixel column, per frame
uint16_t g_trackRow[144];       // the track's colour on each of its rows (gradient + inner shadow)
const int TRACK_ROW0 = 51;      // screen row of g_trackRow[0]

inline float gauss(float x) {   // bell curve exp(-x^2), x >= 0
  if (x >= 4.0f) return 0.0f;
  return g_gauss[(int)(x * 16.0f)] * (1.0f / 255.0f);
}

// Fast atan2 in degrees (about 0.01 deg error; no library call per pixel).
inline float fastAtan2Deg(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y);
  float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  if (mx < 1e-9f) return 0.0f;
  float a = mn / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0.0f) r = 3.14159265f - r;
  if (y < 0.0f) r = -r;
  return r * 57.2957795f;
}

// ===========================================================================
// 6. Everything that changes per frame, worked out ONCE before painting
// ===========================================================================
struct Frame {
  float p;                      // theme progress 0 (dark) .. 1 (light)
  float bgP;                    // the p the page background was last painted with
  // geometry (screen px)
  RBox  track, trackDrop, knob, wall, rim;
  float kcx, kcy, kw, kh, sy;
  int   iconX, iconY;           // knob centre snapped to whole pixels (icon anchor)
  float angle, sinA, cosA, fl;
  float hx, hy, hR2inv;         // halo centre and 1/R^2
  float gx, gy, gRxInv, gRyInv; // knob inner-glow centre and 1/radii
  float bright;                 // rim brightness boost from the flash
  float rimStart;               // mask start angle (a - 72), degrees
  float rsx[4], rcy[4];         // sin / cos of the 4 mask boundary angles (start, +30, +114, +144 deg)
  float sig[4];                 // rim blur sizes (screen px)
  float rimReach;               // how far from the rim line the glow is worth painting
  // colours
  Col trackEdge, trackDropC, kTop, kBot, kGlow, kHi, kEdge, kDrop, wallA, wallB;
  Col rimCore, rimHot, rimGlow, halo, ink, bloom;
  float aTrackEdge, aTrackDrop, aKHi, aKEdge, aKDrop, aHalo;
  float bloomA;                 // 0 in light mode
  bool  idle;                   // not morphing: stars twinkle
};
Frame F;

// Rim glow strength as a function of the distance from the rim line, rebuilt once per frame:
// 32 entries per pixel of distance, alpha 0..256. (The 4 blurred glows and the hot core only
// depend on that distance, so one lookup replaces 4 bell-curve evaluations per pixel.)
const int RIM_BINS  = 32;
const int RIM_LUT_N = 24 * RIM_BINS + 4;
uint16_t g_rimG[RIM_LUT_N];       // the soft coloured glow (4 blurs combined)
uint16_t g_rimH[RIM_LUT_N];       // the narrow hot-white line (already x 0.6)
bool     g_rimLutOk = false;

// knob inner glow: 12 concentric steps (outer -> inner). The CSS radial gradient
// goes glow 100% at 0, 64% at .34, 20% at .62, 0 at .88 (fraction of the radius).
// The steps are spaced so each one adds the same amount of glow (even-looking
// contours), radii worked out in begin().
const int   GLOW_STEPS = 12;
float GLOW_D[GLOW_STEPS];       // step radius (fraction of the gradient size), outer -> inner
float g_glowA[GLOW_STEPS];      // opacity of each step

float glowRamp(float d) {
  if (d <= 0.0f) return 1.0f;
  if (d < 0.34f) return lerpf(1.0f, 0.64f, d / 0.34f);
  if (d < 0.62f) return lerpf(0.64f, 0.20f, (d - 0.34f) / 0.28f);
  if (d < 0.88f) return lerpf(0.20f, 0.0f, (d - 0.62f) / 0.26f);
  return 0.0f;
}

// Background behind the page, per screen row. The CSS is a radial gradient
// centred at (50%, 14%); we keep only its top-to-bottom change (one colour per row).
void buildBackRows(float bgP) {
  Col hi = tokCol(T_BG_HI, bgP), lo = tokCol(T_BG_LO, bgP);
  for (int y = 0; y < SCR_H; y++) {
    float t = fabsf((y + 0.5f) - 0.14f * SCR_H) / SCR_H;
    Col c = mixc(hi, lo, clamp01(t));
    g_backRow[y] = pack(c);
    g_backRGB[y][0] = (uint8_t)(int)(c.r + 0.5f);
    g_backRGB[y][1] = (uint8_t)(int)(c.g + 0.5f);
    g_backRGB[y][2] = (uint8_t)(int)(c.b + 0.5f);
  }
}

// Track rows: top-to-bottom gradient, darker inner shadow along the top edge
// (CSS: inset 0 5px 14px), light "lift" line along the bottom (inset 0 -2px 3px).
void buildTrackRows(float p) {
  Col top = tokCol(T_TRACK_TOP, p), bot = tokCol(T_TRACK_BOT, p);
  Col inC, liftC; float inA, liftA;
  tokMix(T_TRACK_IN, p, &inC, &inA);
  tokMix(T_TRACK_LIFT, p, &liftC, &liftA);
  const RBox& t = F.track;
  float top0 = t.cy - t.hh, h = 2.0f * t.hh;
  for (int i = 0; i < 144; i++) {
    float py = TRACK_ROW0 + i + 0.5f;
    float u = clamp01((py - top0) / h);
    Col c = mixc(top, bot, u);
    float fromTop = (py - top0) / S;               // design px below the top edge
    float fromBot = (top0 + h - py) / S;           // design px above the bottom edge
    c = mixc(c, inC, inA * clamp01(0.5f - (fromTop - 5.0f) / 17.5f));
    c = mixc(c, liftC, liftA * clamp01(0.5f - (fromBot - 2.0f) / 3.75f));
    g_trackRow[i] = pack(c);
  }
}
inline uint16_t trackRow565(int y) {
  int i = y - TRACK_ROW0;
  if (i < 0) i = 0;
  if (i > 143) i = 143;
  return g_trackRow[i];
}

// How much a soft effect may show near the sprite's outer border: 0 on the
// very last pixel row/column, rising smoothly to 1 at 10 px inwards. This is what makes the glow
// meet the plain backdrop outside the sprite with no visible seam.
inline float regionFade(int x, int y) {
  int d = imin(imin(x - REG_X, REG_X + REG_W - 1 - x), imin(y - REG_Y, REG_Y + g_regH - 1 - y));
  if (d <= 0) return 0.0f;
  if (d >= 10) return 1.0f;
  float t = d * 0.1f;
  return t * t * (3.0f - 2.0f * t);        // smooth S-curve over the whole 10 px margin
}

void computeFrame(const tog::Toggle& tg, float bgP) {
  Frame& f = F;
  f.p = tg.themeProgress();
  f.bgP = bgP;
  f.idle = !tg.morphing();

  // ---- track ----
  f.track.cx = OX + tog::TRACK_W * S * 0.5f;  f.track.cy = OY + tog::TRACK_H * S * 0.5f;
  f.track.hw = tog::TRACK_W * S * 0.5f;       f.track.hh = tog::TRACK_H * S * 0.5f;
  f.track.r  = 52.0f * S;
  // CSS: 0 22px 40px -6px  -> shrunk 6, moved down 22, blurred (sigma 20 design px)
  f.trackDrop = f.track;
  f.trackDrop.hw -= 6.0f * S; f.trackDrop.hh -= 6.0f * S; f.trackDrop.r -= 6.0f * S;
  f.trackDrop.cy += 22.0f * S;

  // ---- knob (squash & stretch) ----
  f.sy  = tg.knobScaleY();
  f.kcx = OX + tg.centerX() * S;
  f.kcy = OY + tog::KNOB_Y * S;
  f.kw  = tg.knobWidth() * S;
  f.kh  = tog::KNOB * f.sy * S;
  f.knob.cx = f.kcx; f.knob.cy = f.kcy; f.knob.hw = f.kw * 0.5f; f.knob.hh = f.kh * 0.5f;
  f.knob.r  = 50.0f * S * (f.sy < 1.0f ? f.sy : 1.0f);
  f.iconX = (int)floorf(f.kcx + 0.5f);
  f.iconY = (int)floorf(f.kcy + 0.5f);
  // light-mode side wall: the knob shape moved (5, 6) design px
  f.wall = f.knob; f.wall.cx += 5.0f * S; f.wall.cy += 6.0f * S * f.sy;

  // ---- the one light angle that drives rim, inner glow and halo ----
  f.angle = tg.rimAngleDeg();
  float rad = fmodf(f.angle, 360.0f) * 0.0174532925f;
  f.sinA = sinf(rad); f.cosA = cosf(rad);
  f.fl = tg.flash();

  // halo: 360 design px circle, centre pushed 66 px toward the angle (CSS .halo)
  f.hx = f.kcx + f.sinA * 66.0f * S;
  f.hy = f.kcy - f.cosA * 66.0f * S;
  float hR = 180.0f * S + 3.0f * S;               // + a bit for the 6px blur
  f.hR2inv = 1.0f / (hR * hR);

  // knob inner glow: radial gradient "100% 90% at gx gy" (CSS .k-body)
  f.gx = f.kcx + f.sinA * 0.64f * f.kw;
  f.gy = f.kcy - f.cosA * 0.64f * f.kh;
  f.gRxInv = 1.0f / (1.00f * f.kw);
  f.gRyInv = 1.0f / (0.90f * f.kh);

  // rim: a 2.5 px line on a box 6 px outside the knob (CSS .rim-wrap inset -6px)
  float inset = 2.5f * S * 0.5f;                  // we trace the middle of the line
  f.rim.cx = f.kcx; f.rim.cy = f.kcy;
  f.rim.hw = f.kw * 0.5f + 6.0f * S - inset;
  f.rim.hh = f.kh * 0.5f + (6.0f * S - inset) * f.sy;
  f.rim.r  = (56.0f * S - inset) * (f.sy < 1.0f ? f.sy : 1.0f);
  f.rimStart = f.angle - 72.0f;
  {
    static const float OFF[4] = { 0.0f, 30.0f, 114.0f, 144.0f };
    for (int k = 0; k < 4; k++) {
      float a = (f.rimStart + OFF[k]) * 0.0174532925f;
      f.rsx[k] = sinf(a); f.rcy[k] = cosf(a);
    }
  }
  g_rimLutOk = false;
  f.bright = 1.0f + f.fl * 0.9f;
  // the 4 CSS drop-shadows: blur 1+3fl, 4+6fl, 9+8fl, 16+12fl (design px).
  // Std-dev of a CSS blur = half the blur radius.
  static const float BLUR_BASE[4] = { 1.0f, 4.0f, 9.0f, 16.0f };
  static const float BLUR_FLASH[4] = { 3.0f, 6.0f, 8.0f, 12.0f };
  for (int i = 0; i < 4; i++) {
    float s = 0.5f * S * (BLUR_BASE[i] + BLUR_FLASH[i] * f.fl);
    f.sig[i] = s < 0.45f ? 0.45f : s;
  }
  f.rimReach = 2.2f * f.sig[3] + 2.0f;
  if (f.rimReach > 24.0f) f.rimReach = 24.0f;

  // ---- colours (live theme progress) ----
  float p = f.p;
  tokMix(T_TRACK_EDGE, p, &f.trackEdge, &f.aTrackEdge);
  tokMix(T_TRACK_DROP, p, &f.trackDropC, &f.aTrackDrop);
  f.kTop = tokCol(T_K_TOP, p);  f.kBot = tokCol(T_K_BOT, p);
  f.kGlow = tokCol(T_K_GLOW, p);
  tokMix(T_K_HI, p, &f.kHi, &f.aKHi);
  tokMix(T_K_EDGE, p, &f.kEdge, &f.aKEdge);
  tokMix(T_K_DROP, p, &f.kDrop, &f.aKDrop);
  f.wallA = tokCol(T_WALL_A, p); f.wallB = tokCol(T_WALL_B, p);
  f.rimCore = scalec(tokCol(T_RIM_CORE, p), f.bright);
  f.rimHot  = scalec(tokCol(T_RIM_HOT, p), f.bright);
  f.rimGlow = scalec(tokCol(T_RIM_GLOW, p), f.bright);
  tokMix(T_HALO, p, &f.halo, &f.aHalo);
  f.aHalo *= clamp01(0.8f + f.fl * 0.4f);
  f.ink = mixc(hex(INK_DARK), hex(INK_LIGHT), p);
  f.bloom = hex(BLOOM_RGB);
  f.bloomA = 1.0f - p;
}

// ===========================================================================
// 7. The scene, layer by layer (back to front), inside the current clip
// ===========================================================================

// (a) Page backdrop + the parts of the halo, track shadow and track outline
//     that show in the ~10 px margin around the track. Painted per pixel, but
//     only OUTSIDE the track (the track covers the rest).
//
//     The track never moves, so everything about this ring that is purely geometry (how far each
//     pixel is from the region border, from the track's shadow and from its outline) is worked out
//     ONCE (buildMarginCache) and stored as three small numbers per pixel. Each frame only the
//     colours and the moving halo are mixed in, with whole-number maths.
struct MW { uint8_t fade, drop, edge; };           // all 0..255
MW*      g_mw = 0;                                 // every margin pixel, row after row
uint16_t g_mwOff[170];                             // per sprite row: where its first margin pixel is
int16_t  g_mwSx0[170];                             // per row: first x of the track's solid part
int16_t  g_mwRs[170];                              // per row: first x after it

bool buildMarginCache() {
  if (g_mw) { free(g_mw); g_mw = 0; }
  // the same boxes computeFrame() makes for the (fixed) track and its drop shadow
  RBox tr;
  tr.cx = OX + tog::TRACK_W * S * 0.5f;  tr.cy = OY + tog::TRACK_H * S * 0.5f;
  tr.hw = tog::TRACK_W * S * 0.5f;       tr.hh = tog::TRACK_H * S * 0.5f;
  tr.r  = 52.0f * S;
  RBox td = tr;
  td.hw -= 6.0f * S; td.hh -= 6.0f * S; td.r -= 6.0f * S;
  td.cy += 22.0f * S;
  const float sigD = 20.0f * S;
  const int xEnd = REG_X + REG_W;

  int total = 0;
  for (int ry = 0; ry < g_regH; ry++) {
    float py = REG_Y + ry + 0.5f;
    int sx0 = xEnd, rs = xEnd;                     // no solid track part on this row
    float hi = halfExtent(tr, py, -0.5f);
    if (hi >= 0.0f) {
      sx0 = (int)ceilf(tr.cx - hi - 0.5f);
      rs  = (int)floorf(tr.cx + hi - 0.5f) + 1;
      if (sx0 < REG_X) sx0 = REG_X;
      if (rs > xEnd) rs = xEnd;
      if (rs < sx0) rs = sx0;
    }
    g_mwSx0[ry] = (int16_t)sx0; g_mwRs[ry] = (int16_t)rs; g_mwOff[ry] = (uint16_t)total;
    total += (sx0 - REG_X) + (xEnd - rs);
  }
  g_mw = (MW*)malloc((size_t)total * sizeof(MW));
  if (!g_mw) return false;

  for (int ry = 0; ry < g_regH; ry++) {
    int y = REG_Y + ry;
    float py = y + 0.5f;
    MW* o = g_mw + g_mwOff[ry];
    for (int part = 0; part < 2; part++) {
      int xa = part == 0 ? REG_X : g_mwRs[ry];
      int xb = part == 0 ? g_mwSx0[ry] : xEnd;
      for (int x = xa; x < xb; x++, o++) {
        float px = x + 0.5f;
        float fade = regionFade(x, y);
        float aD = clamp01(0.5f - sdBox(td, px, py) / (2.5f * sigD));
        float dT = sdBox(tr, px, py);
        float aE = dT < 1.2f ? clamp01(1.2f - dT) : 0.0f;
        if (fade <= 0.0f) { aD = 0.0f; aE = 0.0f; }          // the border pixel is plain backdrop
        o->fade = (uint8_t)(int)(fade * 255.0f + 0.5f);
        o->drop = (uint8_t)(int)(aD * fade * 255.0f + 0.5f);
        o->edge = (uint8_t)(int)(aE * 255.0f + 0.5f);
      }
    }
  }
  return true;
}

void drawMargin() {
  const Frame& f = F;
  // this frame's colours and strengths, as whole numbers
  const int tdq = (int)(f.aTrackDrop * 255.0f + 0.5f);
  const int teq = (int)(f.aTrackEdge * 255.0f + 0.5f);
  const int hq  = (int)(f.aHalo * 255.0f + 0.5f);
  const int hr = (int)(f.halo.r + 0.5f),       hg = (int)(f.halo.g + 0.5f),       hb = (int)(f.halo.b + 0.5f);
  const int dr = (int)(f.trackDropC.r + 0.5f), dg = (int)(f.trackDropC.g + 0.5f), db = (int)(f.trackDropC.b + 0.5f);
  const int er = (int)(f.trackEdge.r + 0.5f),  eg = (int)(f.trackEdge.g + 0.5f),  eb = (int)(f.trackEdge.b + 0.5f);
  // halo: (distance / radius)^2 x 127, split into a column part (per frame) and a row part
  const float k = f.hR2inv * 127.0f;
  for (int x = cX0; x < cX1; x++) { float dx = (x + 0.5f) - f.hx; g_colU[x - REG_X] = dx * dx * k; }

  for (int y = cY0; y < cY1; y++) {
    const int ry = y - REG_Y;
    const int br = g_backRGB[y][0], bg = g_backRGB[y][1], bb = g_backRGB[y][2];
    const uint16_t base = toBuf(g_backRow[y]);
    const float dyH = (y + 0.5f) - f.hy;
    const float rowU = dyH * dyH * k;
    const int sx0 = g_mwSx0[ry], rs = g_mwRs[ry];
    const MW* mwRow = g_mw + g_mwOff[ry];
    uint16_t* row = g_buf + (y - g_bandY) * REG_W - REG_X;
    for (int part = 0; part < 2; part++) {
      int xa, xb; const MW* mw;
      if (part == 0) { xa = cX0; xb = imin(cX1, sx0); mw = mwRow - REG_X; }                       // left of the track
      else           { xa = imax(cX0, rs); xb = cX1; mw = mwRow + (sx0 - REG_X) - rs; }          // right of the track
      for (int x = xa; x < xb; x++) {
        const MW& w = mw[x];
        const int fa = w.fade;
        if (fa == 0) { row[x] = base; continue; }
        int r = br, g = bg, b = bb;
        // halo (behind the track)
        float uu = g_colU[x - REG_X] + rowU;
        if (uu < 127.0f) {
          int a = mulA(mulA(hq, g_haloLut[(int)uu]), fa);
          if (a) { int a2 = a256(a); r = mix8(r, hr, a2); g = mix8(g, hg, a2); b = mix8(b, hb, a2); }
        }
        // the track's soft drop shadow
        if (w.drop) { int a2 = a256(mulA(tdq, w.drop)); r = mix8(r, dr, a2); g = mix8(g, dg, a2); b = mix8(b, db, a2); }
        // 1 px outline just outside the track (CSS: 0 0 0 1px)
        if (w.edge) { int a2 = a256(mulA(teq, w.edge)); r = mix8(r, er, a2); g = mix8(g, eg, a2); b = mix8(b, eb, a2); }
        row[x] = pk(r, g, b);
      }
    }
  }
}

// (b) The track: one colour per row, smooth (anti-aliased) rounded ends.
void drawTrack() {
  rasterBox(F.track, -0.5f,
    [](int y, int x0, int x1) { hline(y, x0, x1, trackRow565(y)); },
    [](int x, int y, float d) { blendPx(x, y, unpack(trackRow565(y)), clamp01(0.5f - d)); });
}

// Colour of the track at a pixel (used to pre-mix things painted on top of it).
inline Col trackColAt(int y) { return unpack(trackRow565(y)); }

// Solid inside span of the knob on row y (so layers under the knob can skip it).
inline void knobSolidSpan(float py, int* x0, int* x1) {
  float hi = halfExtent(F.knob, py, -0.5f);
  if (hi < 0.0f) { *x0 = 1; *x1 = 0; return; }
  *x0 = (int)ceilf(F.knob.cx - hi - 0.5f);
  *x1 = (int)floorf(F.knob.cx + hi - 0.5f);
}

// (c) The knob's coloured drop shadow (CSS: 0 12px 30px -4px var(--k-drop)),
//     as 5 solid steps. Kept inside the track; colours pre-mixed with the track row.
void drawKnobShadow() {
  const Frame& f = F;
  if (f.aKDrop < 0.01f) return;
  RBox base = f.knob;
  base.cy += 12.0f * S * f.sy;
  base.hw -= 4.0f * S; base.hh -= 4.0f * S; base.r -= 4.0f * S;
  float sig = 15.0f * S;                           // blur 30 -> sigma 15 design px
  static const float GROW[5] = { 12.0f, 8.0f, 4.0f, 0.0f, -5.0f };
  for (int s = 0; s < 5; s++) {
    RBox b = base;
    b.hw += GROW[s]; b.hh += GROW[s]; b.r += GROW[s];
    if (b.r < 0.0f) b.r = 0.0f;
    // shadow strength in the middle of this step's ring
    float mid = (s < 4) ? (GROW[s] + GROW[s + 1]) * 0.5f : GROW[s] - 3.0f;
    float a = f.aKDrop * clamp01(0.5f - mid / (2.5f * sig));
    int y0 = imax(cY0, (int)floorf(b.cy - b.hh)), y1 = imin(cY1 - 1, (int)ceilf(b.cy + b.hh));
    for (int y = y0; y <= y1; y++) {
      float py = y + 0.5f;
      float he = halfExtent(b, py, 0.0f);
      float ht = halfExtent(f.track, py, -0.5f);
      if (he < 0.0f || ht < 0.0f) continue;
      int x0 = imax((int)ceilf(b.cx - he - 0.5f), (int)ceilf(f.track.cx - ht - 0.5f));
      int x1 = imin((int)floorf(b.cx + he - 0.5f), (int)floorf(f.track.cx + ht - 0.5f));
      int k0, k1; knobSolidSpan(py, &k0, &k1);
      hlineSkip(y, x0, x1, k0, k1, pack(mixc(trackColAt(y), f.kDrop, a)));
    }
  }
}

// (d) Light mode only: the knob's grey side wall peeking out bottom-right
//     (CSS .knob::before, translate(5px, 6px), opacity = p).
void drawWall() {
  const Frame& f = F;
  if (f.p < 0.01f) return;
  float top = f.wall.cy - f.wall.hh, h = 2.0f * f.wall.hh;
  rasterBox(f.wall, -0.5f,
    [&](int y, int x0, int x1) {
      Col w = mixc(f.wallA, f.wallB, clamp01((y + 0.5f - top) / h));
      int k0, k1; knobSolidSpan(y + 0.5f, &k0, &k1);
      hlineSkip(y, x0, x1, k0, k1, pack(mixc(trackColAt(y), w, f.p)));
    },
    [&](int x, int y, float d) {
      Col w = mixc(f.wallA, f.wallB, clamp01((y + 0.5f - top) / h));
      blendPx(x, y, w, f.p * clamp01(0.5f - d));
    });
}

// Knob body colour (top->bottom gradient) on a row.
inline Col knobRowCol(float py) {
  const Frame& f = F;
  return mixc(f.kTop, f.kBot, clamp01((py - (f.kcy - f.kh * 0.5f)) / f.kh));
}
// Which inner-glow step a point falls in (-1 = none), and that step's colour.
inline int glowStepAt(float px, float py) {
  float dx = (px - F.gx) * F.gRxInv, dy = (py - F.gy) * F.gRyInv;
  float d2 = dx * dx + dy * dy;
  int k = -1;
  for (int s = 0; s < GLOW_STEPS; s++) { if (d2 < GLOW_D[s] * GLOW_D[s]) k = s; else break; }
  return k;
}
inline Col knobColAt(float px, float py) {
  Col c = knobRowCol(py);
  int k = glowStepAt(px, py);
  if (k >= 0) c = mixc(c, F.kGlow, g_glowA[k]);
  return c;
}

// (e) The knob body: gradient + inner glow as 12 concentric solid steps
//     (pre-mixed with the body colour), a faint 1 px edge and a top highlight.
void drawKnob() {
  const Frame& f = F;
  const RBox& kb = f.knob;
  float hlShift = 1.5f * S;                       // CSS inset 0 1.5px 0 highlight
  rasterBox(kb, -1.3f,
    // solid inside of a row: body colour, then the glow ellipses on top
    [&](int y, int x0, int x1) {
      float py = y + 0.5f;
      Col body = knobRowCol(py);
      hline(y, x0, x1, pack(body));
      float dy = (py - f.gy) * f.gRyInv;
      for (int s = 0; s < GLOW_STEPS; s++) {
        float rr = GLOW_D[s] * GLOW_D[s] - dy * dy;
        if (rr <= 0.0f) break;                    // smaller steps miss this row too
        float half = sqrtf(rr) / f.gRxInv;
        int e0 = imax(x0, (int)ceilf(f.gx - half - 0.5f));
        int e1 = imin(x1, (int)floorf(f.gx + half - 0.5f));
        if (e0 <= e1) hline(y, e0, e1, pack(mixc(body, f.kGlow, g_glowA[s])));
      }
    },
    // edge pixels: work out the colour here exactly, then blend by coverage
    [&](int x, int y, float d) {
      float px = x + 0.5f, py = y + 0.5f;
      float cov = clamp01(0.5f - d);
      if (cov <= 0.0f) return;
      Col c = knobColAt(px, py);
      c = mixc(c, f.kEdge, f.aKEdge * clamp01(1.0f + 0.5f + d));          // 1px inner edge line
      float dh = sdBox(kb, px, py - hlShift);                              // top highlight band
      if (dh > -0.5f) c = mixc(c, f.kHi, f.aKHi * clamp01(0.5f + dh));
      blendPx(x, y, c, cov);
    });
}

// (f) The pixel moon / sun. Cells are crisp 4x4 squares on a 5 px grid.
//     See-through cells are pre-mixed with the knob colour behind them, so each
//     cell is ONE solid colour (no per-pixel blending).
inline void cellRect(int cx, int cy, int size, uint16_t c) {
  int x0 = cx - size / 2, y0 = cy - size / 2;
  for (int y = y0; y < y0 + size; y++) hline(y, x0, x0 + size - 1, c);
}
// Design px offset -> whole icon pixels. Round half UP, always (so -12.5 -> -12 and
// 12.5 -> 13: the sun's half-cell grid keeps even gaps), with a hair of bias so
// float noise like 26.4999 vs 26.5001 cannot make two "equal" cells round apart.
inline int snapIcon(float v) { return (int)floorf(v * ICON_K + 0.5f + 0.001f); }
inline bool particleCell(const tog::Particle& q, float simT, int* cx, int* cy, float* a) {
  float al = q.a;
  if (al < 0.02f) return false;
  if (F.idle && q.star) al *= 0.78f + 0.22f * sinf(simT * 2.4f + q.phase);
  *cx = F.iconX + snapIcon(q.x);
  *cy = F.iconY + snapIcon(q.y);
  *a = al > 1.0f ? 1.0f : al;
  return true;
}
void drawPixels(const tog::Toggle& tg) {
  const Frame& f = F;
  int n = tg.particleCount();
  float simT = tg.simTime();
  int x0 = cX0 - 6, x1 = cX1 + 6, y0 = cY0 - 6, y1 = cY1 + 6;     // early reject window
  // moon bloom (dark mode): two dim squares behind each bright cell, standing in for the
  // web version's canvas shadowBlur. Outer 8x8 first for every cell, then inner 6x6.
  if (f.bloomA > 0.05f) {
    for (int pass = 0; pass < 2; pass++) {
      int size = pass == 0 ? 8 : 6;
      float a = 0.55f * f.bloomA * (pass == 0 ? 0.20f : 0.42f);
      for (int i = 0; i < n; i++) {
        const tog::Particle& q = tg.particle(i);
        if (q.a <= 0.5f) continue;
        int cx, cy; float al;
        if (!particleCell(q, simT, &cx, &cy, &al)) continue;
        if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
        Col bg = knobColAt(cx, cy);
        if (pass == 1) bg = mixc(bg, f.bloom, 0.55f * f.bloomA * 0.20f);
        cellRect(cx, cy, size, pack(mixc(bg, f.bloom, a)));
      }
    }
  }
  for (int i = 0; i < n; i++) {
    int cx, cy; float al;
    if (!particleCell(tg.particle(i), simT, &cx, &cy, &al)) continue;
    if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
    Col bg = knobColAt(cx, cy);
    cellRect(cx, cy, ICON_CELL, pack(mixc(bg, f.ink, al)));
  }
}

// (g) The neon rim arc + its glow. For each pixel near the rim line we know
//     (1) how far it is from the line and (2) its angle around the knob. The
//     angle gives the CSS conic mask (fades in/out over 30 deg each side), the
//     distance gives the line (2.5 px) and 4 bell-curve glows (the 4 CSS
//     drop-shadows). Only pixels where the mask is > 0 are touched.
//
//     Speed notes: the CSS conic mask is a wedge of 144 degrees with 30-degree ramps at both
//     ends. Which part of the wedge a pixel is in is decided with cross products against the 4
//     boundary directions (no angle needed); only the two 30-degree ramp zones need one
//     arctangent. The glow strength comes from a per-frame lookup table indexed by distance,
//     and the colour mixing is done on whole numbers.
void buildRimLut() {
  const Frame& f = F;
  // bell-curve widths: x = d / (sigma * sqrt 2)
  float k0 = 1.0f / (f.sig[0] * 1.41421356f), k1 = 1.0f / (f.sig[1] * 1.41421356f);
  float k2 = 1.0f / (f.sig[2] * 1.41421356f), k3 = 1.0f / (f.sig[3] * 1.41421356f);
  // glow strengths. A thin line blurred wide gets fainter, so wider = weaker;
  // the flash makes them all stronger (CSS brightness()).
  float boost = 1.0f + 0.5f * f.fl;
  float w0 = 0.95f, w1 = 0.70f * boost, w2 = 0.42f * boost, w3 = 0.26f * boost;
  int n = (int)(f.rimReach * RIM_BINS) + 2;
  if (n > RIM_LUT_N) n = RIM_LUT_N;
  for (int i = 0; i < n; i++) {
    float d = (i + 0.5f) * (1.0f / RIM_BINS);
    float g = 1.0f - (1.0f - w1 * gauss(d * k1)) * (1.0f - w2 * gauss(d * k2)) * (1.0f - w3 * gauss(d * k3));
    float hot = w0 * gauss(d * k0);
    g_rimG[i] = (uint16_t)(clamp01(g) * 256.0f + 0.5f);
    g_rimH[i] = (uint16_t)(clamp01(hot * 0.6f) * 256.0f + 0.5f);
  }
  g_rimLutOk = true;
}

void drawRim() {
  const Frame& f = F;
  const RBox& rb = f.rim;
  if (!g_rimLutOk) buildRimLut();
  const float D = f.rimReach;
  const float lineHalf = 2.5f * S * 0.5f;
  const float Rx = rb.hw - rb.r, Ry = rb.hh - rb.r;
  const float rcore = lineHalf + 0.5f;
  // the three colours as whole numbers
  const int gr = (int)(f.rimGlow.r + 0.5f), gg = (int)(f.rimGlow.g + 0.5f), gb = (int)(f.rimGlow.b + 0.5f);
  const int hr = (int)(f.rimHot.r + 0.5f),  hg = (int)(f.rimHot.g + 0.5f),  hb = (int)(f.rimHot.b + 0.5f);
  const int kr = (int)(f.rimCore.r + 0.5f), kg = (int)(f.rimCore.g + 0.5f), kb = (int)(f.rimCore.b + 0.5f);
  const float ramp = 256.0f / 30.0f;
  int y0 = imax(cY0, (int)floorf(rb.cy - rb.hh - D)), y1 = imin(cY1 - 1, (int)ceilf(rb.cy + rb.hh + D));
  for (int y = y0; y <= y1; y++) {
    float py = y + 0.5f;
    float ho = halfExtent(rb, py, D);
    if (ho < 0.0f) continue;
    float hi = halfExtent(rb, py, -D);
    int xo0 = imax(cX0, (int)floorf(rb.cx - ho)), xo1 = imin(cX1 - 1, (int)ceilf(rb.cx + ho));
    int xi0 = 1, xi1 = 0;
    if (hi >= 0.0f) { xi0 = (int)ceilf(rb.cx - hi); xi1 = (int)floorf(rb.cx + hi) - 1; }
    const float dyr = py - rb.cy;
    const float vyu = -(dyr / f.sy);              // "up" on the un-squashed knob (angles are measured there)
    const float qy = fabsf(dyr) - Ry;
    const float oy = qy > 0.0f ? qy : 0.0f;
    // cross-product term of each boundary that does not depend on x
    const float c0y = -f.rsx[0] * vyu, c1y = -f.rsx[1] * vyu, c2y = -f.rsx[2] * vyu, c3y = -f.rsx[3] * vyu;
    const int dyb = imin(y - REG_Y, REG_Y + g_regH - 1 - y);        // distance of this row to the sprite border
    uint16_t* row = g_buf + (y - g_bandY) * REG_W - REG_X;
    for (int x = xo0; x <= xo1; x++) {
      if (x >= xi0 && x <= xi1) { x = xi1; continue; }
      const float vx = (x + 0.5f) - rb.cx;
      // ---- where in the 144-degree wedge is this pixel? (mq = mask strength 0..256) ----
      int mq;
      const float C1 = f.rcy[1] * vx + c1y, C2 = f.rcy[2] * vx + c2y;
      if (C1 >= 0.0f && C2 < 0.0f) {
        mq = 256;                                                   // flat part of the wedge
      } else {
        const float C0 = f.rcy[0] * vx + c0y, C3 = f.rcy[3] * vx + c3y;
        float ph;
        if (C0 >= 0.0f && C1 < 0.0f)      ph = fastAtan2Deg(C0, f.rsx[0] * vx + f.rcy[0] * vyu);     // fading in
        else if (C2 >= 0.0f && C3 < 0.0f) ph = fastAtan2Deg(-C3, f.rsx[3] * vx + f.rcy[3] * vyu);    // fading out
        else continue;                                                                               // outside the wedge
        mq = (int)(ph * ramp);
        if (mq <= 0) continue;
        if (mq > 256) mq = 256;
      }
      // ---- distance to the rim line ----
      const float qx = fabsf(vx) - Rx;
      const float ox = qx > 0.0f ? qx : 0.0f;
      float sd = (ox > 0.0f && oy > 0.0f) ? fsqrtf(ox * ox + oy * oy) : (ox + oy);
      float inn = qx > qy ? qx : qy;
      if (inn > 0.0f) inn = 0.0f;
      const float d = fabsf(sd + inn - rb.r);
      if (d > D) continue;
      // ---- how strongly each of the three layers shows here ----
      const int di = (int)(d * (float)RIM_BINS);
      const int dxb = imin(x - REG_X, REG_X + REG_W - 1 - x);
      const int db = imin(dxb, dyb);
      const int fq = db >= 10 ? 256 : g_fade256[db < 0 ? 0 : db];
      int ag = (g_rimG[di] * mq) >> 8;
      if (fq != 256) ag = (ag * fq) >> 8;
      const int ah = (g_rimH[di] * mq) >> 8;
      const float cv = rcore - d;
      const int ak = cv <= 0.0f ? 0 : (((cv >= 1.0f ? 256 : (int)(cv * 256.0f)) * mq) >> 8);
      if ((ag | ah | ak) == 0) continue;
      uint16_t* pp = row + x;
      int r, g, b; ld(*pp, &r, &g, &b);
      if (ag) { r = mix8(r, gr, ag); g = mix8(g, gg, ag); b = mix8(b, gb, ag); }
      if (ah) { r = mix8(r, hr, ah); g = mix8(g, hg, ah); b = mix8(b, hb, ah); }
      if (ak) { r = mix8(r, kr, ak); g = mix8(g, kg, ak); b = mix8(b, kb, ak); }
      *pp = pk(r, g, b);
    }
  }
}

// Timing per layer (only when CYD_BENCH is 1): microseconds summed over the frames painted.
//   0 margin, 1 track, 2 knob shadow, 3 wall, 4 knob, 5 pixel icon, 6 rim glow, 7 frame set-up
uint32_t g_layerUs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
uint32_t g_layerFrames = 0;
#if CYD_BENCH
bool     g_benchRec = false;      // true only for frames painted while the toggle is moving
#endif

inline uint32_t nowUs() {
#ifdef ARDUINO
  return micros();
#else
  using namespace std::chrono;
  static const steady_clock::time_point t0 = steady_clock::now();
  return (uint32_t)duration_cast<microseconds>(steady_clock::now() - t0).count();
#endif
}

#if CYD_BENCH
  #define LAYER(i, call) { uint32_t lt_ = nowUs(); call; if (g_benchRec) g_layerUs[i] += nowUs() - lt_; }
#else
  #define LAYER(i, call) call
#endif

void drawScene(const tog::Toggle& tg) {
  LAYER(0, drawMargin());
  LAYER(1, drawTrack());
  LAYER(2, drawKnobShadow());
  LAYER(3, drawWall());
  LAYER(4, drawKnob());
  LAYER(5, drawPixels(tg));
  LAYER(6, drawRim());
}

// ===========================================================================
// 8. Dirty rectangles: only send what changed
// ===========================================================================
struct IRect { int x0, y0, x1, y1; };             // end-exclusive; empty when x0 >= x1
inline bool rEmpty(const IRect& r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }
inline IRect rUnion(const IRect& a, const IRect& b) {
  if (rEmpty(a)) return b;
  if (rEmpty(b)) return a;
  IRect r = { imin(a.x0, b.x0), imin(a.y0, b.y0), imax(a.x1, b.x1), imax(a.y1, b.y1) };
  return r;
}
inline IRect rClip(const IRect& a, const IRect& b) {
  IRect r = { imax(a.x0, b.x0), imax(a.y0, b.y0), imin(a.x1, b.x1), imin(a.y1, b.y1) };
  return r;
}
inline IRect regionRect() { IRect r = { REG_X, REG_Y, REG_X + REG_W, REG_Y + g_regH }; return r; }

// Box around the knob + everything that hangs off it (rim glow, shadow, wall).
IRect knobBox() {
  const Frame& f = F;
  int m = (int)ceilf(f.rimReach) + 6;
  IRect r = { (int)floorf(f.kcx - f.kw * 0.5f) - m, (int)floorf(f.kcy - f.kh * 0.5f) - m,
              (int)ceilf(f.kcx + f.kw * 0.5f) + m, (int)ceilf(f.kcy + f.kh * 0.5f) + m + 10 };
  return rClip(r, regionRect());
}
// Box around the halo (it only shows in the margin, but it is big).
IRect haloBox() {
  const Frame& f = F;
  float R = 183.0f * S;
  IRect r = { (int)floorf(f.hx - R), (int)floorf(f.hy - R), (int)ceilf(f.hx + R), (int)ceilf(f.hy + R) };
  return rClip(r, regionRect());
}
// Box around the twinkling stars (dark mode, idle).
IRect starsBox(const tog::Toggle& tg) {
  IRect r = { 0, 0, 0, 0 };
  for (int i = 0; i < tg.particleCount(); i++) {
    const tog::Particle& q = tg.particle(i);
    if (!q.star || q.a < 0.02f) continue;
    int cx = F.iconX + snapIcon(q.x);
    int cy = F.iconY + snapIcon(q.y);
    IRect c = { cx - 5, cy - 5, cx + 5, cy + 5 };  // cell + bloom
    r = rUnion(r, c);
  }
  return rClip(r, regionRect());
}

// ===========================================================================
// 9. State kept between frames
// ===========================================================================
bool     g_invalid = true;
float    g_bgP = -1.0f;              // p used for the page background currently on screen
float    g_lastP = -1.0f;
IRect    g_lastDyn = { 0, 0, 0, 0 };
IRect    g_lastHaloBox = { 0, 0, 0, 0 };
float    g_lastHx = -1e9f, g_lastHy = -1e9f, g_lastHa = -1.0f;
bool     g_wasBusy = true;
uint32_t g_lastTwinkleMs = 0;
int32_t  g_dotColor = -1;            // what the status dot shows (-1 = nothing)
uint32_t g_drawUs = 0, g_pushUs = 0;

// Paint the page background on the screen everywhere OUTSIDE the sprite area.
// Rows of equal colour are merged into one rectangle each.
void paintBackdropOutside(LGFX& lcd) {
  int ry0 = REG_Y, ry1 = REG_Y + g_regH;
  int y = 0;
  while (y < SCR_H) {
    int y2 = y + 1;
    while (y2 < SCR_H && g_backRow[y2] == g_backRow[y] && (y2 < ry0) == (y < ry0) && (y2 < ry1) == (y < ry1)) y2++;
    uint16_t c = g_backRow[y];
    int h = y2 - y;
    if (y >= ry0 && y < ry1) {                    // beside the sprite: left and right strips
      lcd.fillRect(0, y, REG_X, h, c);
      lcd.fillRect(REG_X + REG_W, y, SCR_W - REG_X - REG_W, h, c);
    } else {                                      // above / below the sprite: full width
      lcd.fillRect(0, y, SCR_W, h, c);
    }
    y = y2;
  }
}

// The quiet status dot (top-right). Only touches the screen when its colour changes.
void updateDot(LGFX& lcd, NetStatus net, uint32_t nowMs, bool force, const uint16_t* rows) {
  int32_t want = -1;
  if (net != NetStatus::Online) {
    Col base = net == NetStatus::WifiDown ? hex(0xd2483c) : hex(0xe0a030);   // dim red / dim amber
    float ph = (float)(nowMs % 2200u) / 2200.0f;
    float b = 0.775f + 0.225f * sinf(ph * 6.2831853f);                       // 55% .. 100%
    Col bg = unpack(rows[DOT_Y]);
    want = pack(mixc(bg, base, 0.80f * b));
  }
  if (want == g_dotColor && !force) return;
  if (want < 0) {
    if (g_dotColor >= 0 || force) {               // erase: put the backdrop rows back
      for (int y = DOT_Y - DOT_R - 1; y <= DOT_Y + DOT_R + 1; y++)
        lcd.fillRect(DOT_X - DOT_R - 1, y, 2 * DOT_R + 3, 1, rows[y]);
    }
  } else {
    lcd.fillCircle(DOT_X, DOT_Y, DOT_R, (uint16_t)want);
  }
  g_dotColor = want;
}


// ===========================================================================
// 9b. Screen two: the hue and brightness sliders, in 8-bit pixel style
// ===========================================================================
// Screen two is drawn with the same tools as screen one (our own pixel painters into the
// sprite strip, then a DMA push), but everything on it is chunky on purpose:
//   * stair-step corners instead of smooth curves,
//   * cells with a 1 px gap, a light top edge and a dark bottom edge (a 3-tone bevel),
//   * knobs made of a dark metal body, a bright pixel window showing the chosen colour, and a
//     neon outline whose glow grows while a finger holds the knob.
// The page behind it is always dark, whatever theme screen one is in.
using namespace layout;

uint16_t g_back2[SCR_H];                       // the dark page background, one colour per row
uint8_t  g_back2RGB[SCR_H][3];
uint16_t g_hueCell[HUE_CELLS][4];              // each hue cell: top highlight, body, shade, dark edge
bool     g_invalid2 = true;                    // the next screen-two frame repaints everything

void buildBack2() {
  Col hi = tokCol(T_BG_HI, 0.0f), lo = tokCol(T_BG_LO, 0.0f);
  for (int y = 0; y < SCR_H; y++) {
    float t = fabsf((y + 0.5f) - 0.14f * SCR_H) / SCR_H;
    Col c = mixc(hi, lo, clamp01(t));
    g_back2[y] = pack(c);
    g_back2RGB[y][0] = (uint8_t)(int)(c.r + 0.5f);
    g_back2RGB[y][1] = (uint8_t)(int)(c.g + 0.5f);
    g_back2RGB[y][2] = (uint8_t)(int)(c.b + 0.5f);
  }
}

// Hue 0..1 (red -> yellow -> green -> cyan -> blue -> magenta -> red again) at full colour.
void hueRGB(float v, int* r, int* g, int* b) {
  v = clamp01(v);
  float h = v * 6.0f;
  int i = (int)h;
  if (i >= 6) { i = 0; h = 0.0f; }
  int up = (int)((h - (float)i) * 255.0f + 0.5f), dn = 255 - up;
  switch (i) {
    case 0:  *r = 255; *g = up;  *b = 0;   break;
    case 1:  *r = dn;  *g = 255; *b = 0;   break;
    case 2:  *r = 0;   *g = 255; *b = up;  break;
    case 3:  *r = 0;   *g = dn;  *b = 255; break;
    case 4:  *r = up;  *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;   *b = dn;  break;
  }
}

// Three shades of one colour for a bevelled cell: lighter, as is, darker.
inline uint16_t shade565(int r, int g, int b, int k256) { return rgb565((r * k256) >> 8, (g * k256) >> 8, (b * k256) >> 8); }
inline uint16_t tint565(int r, int g, int b, int a256) { return rgb565(mix8(r, 255, a256), mix8(g, 255, a256), mix8(b, 255, a256)); }

void buildScreen2Tables() {
  buildBack2();
  for (int i = 0; i < HUE_CELLS; i++) {
    float v = hueValueFromX(HUE_CELL_X0 + i * HUE_CELL_PITCH + 3);       // the hue under the middle of the cell
    int r, g, b; hueRGB(v, &r, &g, &b);
    g_hueCell[i][0] = tint565(r, g, b, 90);
    g_hueCell[i][1] = rgb565(r, g, b);
    g_hueCell[i][2] = shade565(r, g, b, 170);
    g_hueCell[i][3] = shade565(r, g, b, 100);
  }
}

// ---- fixed colours of the pixel art ----
const uint16_t C2_OUTLINE   = rgb565(7, 8, 11);
const uint16_t C2_WELL      = rgb565(5, 6, 8);
const uint16_t C2_FR_SHADE  = rgb565(16, 17, 21);
const uint16_t C2_FR_BODY   = rgb565(40, 42, 49);
const uint16_t C2_FR_LIGHT  = rgb565(78, 82, 94);
const uint16_t C2_OFF_CELL  = rgb565(24, 26, 31);
const uint16_t C2_OFF_EDGE  = rgb565(14, 15, 18);
const uint16_t C2_DIVIDER   = rgb565(30, 32, 38);

// ---- tiny shape painters (all clipped to the clip rectangle) ----
inline void rectFill(int x, int y, int w, int h, uint16_t c) {
  int y0 = imax(y, cY0), y1 = imin(y + h, cY1);
  for (int yy = y0; yy < y1; yy++) hline(yy, x, x + w - 1, c);
}

// A rectangle whose corners are cut in steps, the 8-bit way. cut = 1..4 steps.
const uint8_t CORNER_INSET[5][4] = { {0, 0, 0, 0}, {1, 0, 0, 0}, {2, 1, 0, 0}, {3, 1, 1, 0}, {4, 2, 1, 1} };
inline void notchFill(int x, int y, int w, int h, int cut, uint16_t c) {
  int y0 = imax(y, cY0), y1 = imin(y + h, cY1);
  for (int yy = y0; yy < y1; yy++) {
    int k = imin(yy - y, h - 1 - (yy - y));
    int inset = k < cut ? CORNER_INSET[cut][k] : 0;
    hline(yy, x + inset, x + w - 1 - inset, c);
  }
}

// Blend one pixel toward (r,g,b) by a (0..256), whole-number maths.
inline void blendInt(int x, int y, int r, int g, int b, int a) {
  if (a <= 0 || !inClip(x, y)) return;
  uint16_t* p = at(x, y);
  int r0, g0, b0; ld(*p, &r0, &g0, &b0);
  *p = pk(mix8(r0, r, a), mix8(g0, g, a), mix8(b0, b, a));
}
void blendRect(int x0, int y0, int x1, int y1, int r, int g, int b, int a) {   // x1, y1 exclusive
  int ya = imax(y0, cY0), yb = imin(y1, cY1), xa = imax(x0, cX0), xb = imin(x1, cX1);
  if (a <= 0) return;
  for (int y = ya; y < yb; y++) {
    uint16_t* p = at(xa, y);
    for (int x = xa; x < xb; x++, p++) {
      int r0, g0, b0; ld(*p, &r0, &g0, &b0);
      *p = pk(mix8(r0, r, a), mix8(g0, g, a), mix8(b0, b, a));
    }
  }
}
// A one pixel wide ring `d` pixels outside the rectangle (corner pixels left out: softer, pixel-like).
void blendRing(int x0, int y0, int x1, int y1, int d, int r, int g, int b, int a) {
  if (a <= 0) return;
  int ax = x0 - d, ay = y0 - d, bx = x1 + d - 1, by = y1 + d - 1;          // the ring's outer box, inclusive
  for (int x = ax + 1; x < bx; x++) { blendInt(x, ay, r, g, b, a); blendInt(x, by, r, g, b, a); }
  for (int y = ay + 1; y < by; y++) { blendInt(ax, y, r, g, b, a); blendInt(bx, y, r, g, b, a); }
}

// The dark metal frame around a slider (outline, bevel, body).
void metalFrame(int x, int y, int w, int h) {
  notchFill(x, y, w, h, 4, C2_FR_SHADE);
  notchFill(x, y, w - 1, h - 1, 4, C2_FR_LIGHT);
  notchFill(x + 1, y + 1, w - 2, h - 2, 3, C2_FR_BODY);
}

// A bevelled coloured cell: light top, body, shade, dark edge, one white glint pixel.
inline void bevelCell(int x, int y, int w, int h, uint16_t hi, uint16_t body, uint16_t shade, uint16_t dark) {
  int hiH = h >= 20 ? 3 : 2, darkH = h >= 20 ? 2 : 1, shadeH = h >= 20 ? 4 : 2;
  rectFill(x, y, w, hiH, hi);
  rectFill(x, y + hiH, w, h - hiH - shadeH - darkH, body);
  rectFill(x, y + h - shadeH - darkH, w, shadeH, shade);
  rectFill(x, y + h - darkH, w, darkH, dark);
  rectFill(x + 1, y + 1, 1, 1, 0xFFFF);
}

// ---- everything that changes per frame on screen two, worked out once ----
struct S2 {
  int hx, hy, hw, hh;          // hue knob rectangle (screen px)
  int bx, by, bw, bh;          // brightness knob rectangle
  int ar, ag, ab;              // the chosen colour at full strength
  int glowH, glowB;            // rim glow strength of each knob, 0..256
  int litCy;                   // cells whose middle is at or below this y are lit
  int sunLevel;                // 0..15, brightness of the sun / moon icons
};
S2 g_s2;

void computeS2(const tog::Slider& hue, const tog::Slider& bri) {
  S2& s = g_s2;
  float hs = clamp01(hue.stretch()), hp = clamp01(hue.press());
  s.hw = (int)(30.0f + 16.0f * hs + 6.0f * hp + 0.5f);
  s.hh = (int)(62.0f - 10.0f * hs + 0.5f);
  s.hx = (int)floorf(hueKnobX(hue.pos()) - s.hw * 0.5f + 0.5f);
  s.hx = imax(2, imin(SPLIT_X - 3 - s.hw, s.hx));
  s.hy = HUE_CY - s.hh / 2;

  float bs = clamp01(bri.stretch()), bp = clamp01(bri.press());
  s.bw = (int)(52.0f - 6.0f * bs + 4.0f * bp + 0.5f);
  s.bh = (int)(20.0f + 12.0f * bs + 0.5f);
  float cy = briKnobY(bri.pos());
  s.by = (int)floorf(cy - s.bh * 0.5f + 0.5f);
  s.by = imax(4, imin(SCR_H - 4 - s.bh, s.by));
  s.bx = BRI_CX - s.bw / 2;
  s.litCy = s.by + s.bh / 2;

  hueRGB(hue.pos(), &s.ar, &s.ag, &s.ab);
  s.glowH = (int)(clamp01(hue.glow()) * 256.0f);
  s.glowB = (int)(clamp01(bri.glow()) * 256.0f);
  s.sunLevel = (int)(clamp01(bri.pos()) * 15.0f + 0.5f);
}

// A knob: soft glow rings and drop shadow behind, dark metal body, a bright window with the colour.
void drawKnob2(int x0, int y0, int w, int h, int winInX, int winInY, int ar, int ag, int ab, int glow256) {
  if (x0 - 9 >= cX1 || x0 + w + 9 <= cX0 || y0 - 9 >= cY1 || y0 + h + 9 <= cY0) return;
  // neon glow: four rings fading outwards, stronger while the finger holds the knob
  static const uint8_t RING_A[4] = { 210, 100, 52, 22 };
  int k = 90 + ((glow256 * 166) >> 8);                       // 90..256: even a resting knob glows a little
  for (int d = 1; d <= 4; d++) blendRing(x0, y0, x0 + w, y0 + h, d, ar, ag, ab, (RING_A[d - 1] * k) >> 8);
  // drop shadow (down and to the right)
  blendRect(x0 + 3, y0 + 4, x0 + w + 3, y0 + h + 4, 0, 0, 0, 150);
  // metal body: outline, shade, top-to-bottom gradient, highlight lines
  notchFill(x0, y0, w, h, 3, C2_OUTLINE);
  notchFill(x0 + 1, y0 + 1, w - 2, h - 2, 3, C2_FR_SHADE);
  int bodyTop = y0 + 2, bodyH = h - 5;
  for (int y = imax(bodyTop, cY0); y < imin(bodyTop + bodyH, cY1); y++) {
    int t = bodyH > 1 ? ((y - bodyTop) * 256) / (bodyH - 1) : 0;
    hline(y, x0 + 2, x0 + w - 4, rgb565(mix8(58, 36, t), mix8(61, 38, t), mix8(69, 44, t)));
  }
  rectFill(x0 + 3, y0 + 1, w - 7, 1, C2_FR_LIGHT);            // top highlight
  rectFill(x0 + 1, y0 + 3, 1, h - 7, C2_FR_LIGHT);            // left highlight
  // the window that shows the chosen colour
  int wx = x0 + winInX, wy = y0 + winInY, ww = w - 2 * winInX, wh = h - 2 * winInY;
  if (ww > 4 && wh > 4) {
    rectFill(wx, wy, ww, wh, C2_WELL);
    bevelCell(wx + 1, wy + 1, ww - 2, wh - 2, tint565(ar, ag, ab, 90), rgb565(ar, ag, ab),
              shade565(ar, ag, ab, 170), shade565(ar, ag, ab, 100));
  }
  // bright 1 px neon line right around the outline
  blendRing(x0, y0, x0 + w, y0 + h, 0, ar, ag, ab, 140 + ((glow256 * 100) >> 8));
}

// A little 8 x 8 pixel picture drawn at 2x.
void bitmap8(int x, int y, const uint8_t rows[8], uint16_t c) {
  if (x >= cX1 || x + 16 <= cX0 || y >= cY1 || y + 16 <= cY0) return;
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 8; b++)
      if (rows[r] & (0x80 >> b)) rectFill(x + b * 2, y + r * 2, 2, 2, c);
}
const uint8_t BMP_SUN[8]  = { 0x18, 0x5A, 0x3C, 0xFF, 0xFF, 0x3C, 0x5A, 0x18 };
const uint8_t BMP_MOON[8] = { 0x38, 0x70, 0xE0, 0xE0, 0xE0, 0xF0, 0x7C, 0x38 };

void drawScene2() {
  const S2& s = g_s2;
  // ---- page background + a dotted divider between the two thirds ----
  for (int y = cY0; y < cY1; y++) {
    hline(y, cX0, cX1 - 1, g_back2[y]);
    if ((y & 3) == 0) hline(y, SPLIT_X - 1, SPLIT_X - 1, C2_DIVIDER);
  }

  // ---- hue slider: frame, well, 26 rainbow cells ----
  if (HUE_FRAME_X < cX1 && HUE_FRAME_X + HUE_FRAME_W > cX0 && HUE_FRAME_Y < cY1 && HUE_FRAME_Y + HUE_FRAME_H > cY0) {
    metalFrame(HUE_FRAME_X, HUE_FRAME_Y, HUE_FRAME_W, HUE_FRAME_H);
    notchFill(HUE_FRAME_X + 4, HUE_FRAME_Y + 4, HUE_FRAME_W - 8, HUE_FRAME_H - 8, 3, C2_WELL);
    for (int i = 0; i < HUE_CELLS; i++) {
      int cx = HUE_CELL_X0 + i * HUE_CELL_PITCH;
      if (cx + 6 <= cX0 || cx >= cX1) continue;
      bevelCell(cx, HUE_CELL_Y, 6, HUE_CELL_H, g_hueCell[i][0], g_hueCell[i][1], g_hueCell[i][2], g_hueCell[i][3]);
    }
  }

  // ---- brightness slider: frame, well, 15 meter cells that light from the bottom up ----
  if (BRI_FRAME_X < cX1 && BRI_FRAME_X + BRI_FRAME_W > cX0 && BRI_FRAME_Y < cY1 && BRI_FRAME_Y + BRI_FRAME_H > cY0) {
    metalFrame(BRI_FRAME_X, BRI_FRAME_Y, BRI_FRAME_W, BRI_FRAME_H);
    notchFill(BRI_FRAME_X + 4, BRI_FRAME_Y + 4, BRI_FRAME_W - 8, BRI_FRAME_H - 8, 3, C2_WELL);
    for (int k = 0; k < BRI_CELLS; k++) {
      int cy = BRI_CELL_Y0 + (BRI_CELLS - 1 - k) * BRI_CELL_PITCH;
      if (cy + 10 <= cY0 || cy >= cY1) continue;
      if (cy + 5 >= s.litCy) {                          // lit: the chosen colour, stronger towards the top
        int lv = 70 + (k * 186) / (BRI_CELLS - 1);       // 70..256
        int r = (s.ar * lv) >> 8, g = (s.ag * lv) >> 8, b = (s.ab * lv) >> 8;
        bevelCell(BRI_CELL_X0, cy, BRI_CELL_W, 10, tint565(r, g, b, 80), rgb565(r, g, b), shade565(r, g, b, 170), shade565(r, g, b, 100));
      } else {                                           // dark: an empty slot
        rectFill(BRI_CELL_X0, cy, BRI_CELL_W, 10, C2_OFF_CELL);
        rectFill(BRI_CELL_X0, cy, BRI_CELL_W, 1, C2_OFF_EDGE);
      }
    }
  }

  // ---- sun above, moon below the brightness frame: the sun warms up and the moon dims as the slider goes up.
  // (They sit OUTSIDE the frame, so they get their own "is this rectangle touching me?" test.)
  {
    int lv = s.sunLevel;                                // 0..15
    if (BRI_CX - 8 < cX1 && BRI_CX + 8 > cX0 && 4 < cY1 && 20 > cY0)
      bitmap8(BRI_CX - 8, 4, BMP_SUN, rgb565(60 + lv * 13, 50 + lv * 10, 28 + lv * 2));
    if (BRI_CX - 8 < cX1 && BRI_CX + 8 > cX0 && 220 < cY1 && 236 > cY0)
      bitmap8(BRI_CX - 8, 220, BMP_MOON, rgb565(40 + (15 - lv) * 11, 44 + (15 - lv) * 11, 66 + (15 - lv) * 12));
  }

  // ---- knobs on top ----
  drawKnob2(s.hx, s.hy, s.hw, s.hh, 7, 11, s.ar, s.ag, s.ab, s.glowH);
  drawKnob2(s.bx, s.by, s.bw, s.bh, 7, 4, s.ar, s.ag, s.ab, s.glowB);
}

// Paint one screen-two rectangle (at most 294 x 80) and send it.
int g_tileFlip = 0;
void paintTile2(LGFX& lcd, const IRect& t) {
  if (rEmpty(t)) return;
  int bi = g_nbuf == 2 ? (g_tileFlip ^= 1) : 0;
  if (g_lastPushed == bi) lcd.waitDMA();
  g_buf = g_bufs[bi];
  // keep the sprite entirely on the screen, so the library never has to clip it
  g_ox = imax(0, imin(t.x0, SCR_W - REG_W));
  g_bandY = imin(t.y0, SCR_H - g_bandH);
  cX0 = t.x0; cY0 = t.y0; cX1 = t.x1; cY1 = t.y1;
  drawScene2();
  lcd.setClipRect(t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0);
  g_spr[bi].pushSprite(&lcd, g_ox, g_bandY);
  lcd.clearClipRect();
  g_lastPushed = bi;
  g_ox = REG_X;
}

inline IRect screenRect() { IRect r = { 0, 0, SCR_W, SCR_H }; return r; }

// Paint any screen-two rectangle by cutting it into tiles.
void paintRect2(LGFX& lcd, const IRect& r) {
  IRect c = rClip(r, screenRect());
  for (int y = c.y0; y < c.y1; y += g_bandH) {
    for (int x = c.x0; x < c.x1; x += REG_W) {
      IRect t = { x, y, imin(x + REG_W, c.x1), imin(y + g_bandH, c.y1) };
      paintTile2(lcd, t);
    }
  }
}

// What screen two showed in its last frame (to work out what changed).
IRect    g_prevHueBox = { 0, 0, 0, 0 }, g_prevBriBox = { 0, 0, 0, 0 };

IRect knobBox2(int x, int y, int w, int h) {
  IRect r = { x - 6, y - 5, x + w + 8, y + h + 9 };            // glow rings + shadow
  return rClip(r, screenRect());
}

// ---------------------------------------------------------------------------
// 9c. The screen change: a pixel-block wipe
// ---------------------------------------------------------------------------
// The screen is cut into 16 x 16 pixel blocks. The new screen takes over block by block in a wave
// that sweeps up (going to screen two) or down (going back). The edge of the wave is dithered with
// a 4 x 4 pattern, so it looks like old-school pixel dissolving rather than a smooth wipe.
// Each block is painted straight from the NEW screen's drawing code, so nothing has to remember
// what the old screen looked like.
const int BLK = 16, BCOLS = 20, BROWS = 15;
const int WAVE_STEPS = (BROWS - 1) * 4 + 16;                   // 72 steps from "nothing changed" to "everything changed"
const uint8_t BAYER4[16] = { 0, 8, 2, 10,  12, 4, 14, 6,  3, 11, 1, 9,  15, 7, 13, 5 };
int g_tSteps = 0;

inline int blockStep(int bx, int by, bool wavesUp) {
  int row = wavesUp ? (BROWS - 1 - by) : by;
  return row * 4 + BAYER4[(by & 3) * 4 + (bx & 3)];
}

// Paint screen one's backdrop OUTSIDE the sprite area for the rows of r (rows of equal colour merge).
void fillBackdropRows(LGFX& lcd, const IRect& r) {
  int ry0 = REG_Y, ry1 = REG_Y + g_regH;
  int y = r.y0;
  while (y < r.y1) {
    bool inBand = y >= ry0 && y < ry1;
    int y2 = y + 1;
    while (y2 < r.y1 && g_backRow[y2] == g_backRow[y] && (y2 >= ry0 && y2 < ry1) == inBand) y2++;
    uint16_t c = g_backRow[y];
    if (inBand) {
      if (r.x0 < REG_X) lcd.fillRect(r.x0, y, imin(r.x1, REG_X) - r.x0, y2 - y, c);
      if (r.x1 > REG_X + REG_W) lcd.fillRect(imax(r.x0, REG_X + REG_W), y, r.x1 - imax(r.x0, REG_X + REG_W), y2 - y, c);
    } else {
      lcd.fillRect(r.x0, y, r.x1 - r.x0, y2 - y, c);
    }
    y = y2;
  }
}

// Paint any rectangle of screen one (the part inside the sprite area from the scene, the rest as backdrop).
void paintToggleRect(LGFX& lcd, const tog::Toggle& tg, const IRect& r) {
  fillBackdropRows(lcd, r);
  IRect in = rClip(r, regionRect());
  if (rEmpty(in)) return;
  for (int y = in.y0; y < in.y1; y += g_bandH) {
    IRect t = { in.x0, y, in.x1, imin(y + g_bandH, in.y1) };
    int bi = g_nbuf == 2 ? (g_tileFlip ^= 1) : 0;
    if (g_lastPushed == bi) lcd.waitDMA();
    g_buf = g_bufs[bi];
    g_ox = REG_X;
    g_bandY = imin(t.y0, SCR_H - g_bandH);
    cX0 = t.x0; cY0 = t.y0; cX1 = t.x1; cY1 = t.y1;
    drawScene(tg);
    lcd.setClipRect(t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0);
    g_spr[bi].pushSprite(&lcd, REG_X, g_bandY);
    lcd.clearClipRect();
    g_lastPushed = bi;
  }
}

}  // namespace

// ===========================================================================
// 10. Public API (draw.h)
// ===========================================================================
Rect trackRect() {
  Rect r = { (int16_t)OX, (int16_t)OY, (int16_t)(tog::TRACK_W * S + 0.5f), (int16_t)(tog::TRACK_H * S + 0.5f) };
  return r;   // {23, 51, 274, 139}
}

bool begin(LGFX& lcd) {
  // lookup tables
  for (int i = 0; i < 64; i++) { float x = i / 16.0f; g_gauss[i] = (uint8_t)(expf(-x * x) * 255.0f + 0.5f); }
  for (int i = 0; i < 128; i++) { float u = (i + 0.5f) / 128.0f; g_haloLut[i] = (uint8_t)((1.0f - sqrtf(u)) * 255.0f + 0.5f); }
  for (int s = 0; s < GLOW_STEPS; s++) {
    // step s starts where the ramp reaches opacity s/N and is painted with (s+0.5)/N
    float target = (float)s / GLOW_STEPS, d = 0.88f;
    while (d > 0.0f && glowRamp(d) <= target) d -= 0.0025f;
    GLOW_D[s] = d + 0.0025f;
    g_glowA[s] = (s + 0.5f) / GLOW_STEPS;
  }

  // off-screen buffers: try SPRITE_BANDS, then thinner strips if RAM is short.
  // With 2 or more strips we keep TWO buffers so one can be sent while the other is painted.
  for (int k = 0; k < 2; k++) { g_spr[k].deleteSprite(); g_bufs[k] = 0; }
  g_buf = 0; g_nbuf = 0; g_lastPushed = -1;
  static const uint8_t TRY[3] = { 1, 2, 4 };
  int first = SPRITE_BANDS >= 4 ? 2 : (SPRITE_BANDS >= 2 ? 1 : 0);
  for (int i = first; i < 3 && !g_buf; i++) {
    uint8_t bands = TRY[i];
    int bh = (REG_H1 + bands - 1) / bands;
    int want = bands == 1 ? 1 : 2;
    int got = 0;
    for (int k = 0; k < want; k++) {
      g_spr[k].setColorDepth(16);
      g_spr[k].createSprite(REG_W, bh);
      if (!g_spr[k].getBuffer()) { g_spr[k].deleteSprite(); break; }
      g_bufs[k] = (uint16_t*)g_spr[k].getBuffer();
      got++;
    }
    if (got == want) {
      g_buf = g_bufs[0];
      g_nbuf = (uint8_t)want; g_bands = bands; g_bandH = bh; g_regH = bh * bands;
    } else {
      for (int k = 0; k < 2; k++) { g_spr[k].deleteSprite(); g_bufs[k] = 0; }
    }
  }
#if defined(ARDUINO) && DEBUG_LOG
  if (g_buf) Serial.printf("[draw] sprite %d x %d, %u band(s), %u buffer(s), %u bytes\n", REG_W, g_bandH, (unsigned)g_bands, (unsigned)g_nbuf, (unsigned)bufferBytes());
  else       Serial.println("[draw] could not allocate even the smallest sprite strip!");
#endif
  if (!g_buf) return false;

  // Which byte order does the sprite memory use? Paint pure red (RGB565 0xF800)
  // with the library and look at what landed in memory.
  g_spr[0].drawPixel(0, 0, (uint16_t)0xF800);
  uint16_t raw = g_bufs[0][0];
  g_swap = (raw != 0xF800);                       // 0x00F8 = swapped (the normal LovyanGFX case)
#if defined(ARDUINO) && DEBUG_LOG
  if (raw != 0xF800 && raw != 0x00F8) Serial.printf("[draw] unexpected sprite pixel format 0x%04x\n", raw);
#endif
  buildPackTables();
  buildScreen2Tables();
  g_invalid2 = true;
  g_tSteps = 0;
  for (int d = 0; d <= 10; d++) {                 // regionFade() as a table
    float t = d * 0.1f;
    g_fade256[d] = d <= 0 ? 0 : (d >= 10 ? 256 : (int)(t * t * (3.0f - 2.0f * t) * 256.0f + 0.5f));
  }
  if (!buildMarginCache()) {
#if defined(ARDUINO) && DEBUG_LOG
    Serial.println("[draw] could not allocate the margin cache!");
#endif
    return false;
  }

  // first backdrop (dark until the first render knows better)
  buildBackRows(0.0f);
  lcd.startWrite();
  lcd.fillRect(0, 0, SCR_W, SCR_H, g_backRow[SCR_H / 2]);
  paintBackdropOutside(lcd);
  lcd.endWrite();
  g_bgP = 0.0f;
  g_invalid = true;
  g_dotColor = -1;
  g_lastP = -1.0f;
  g_wasBusy = true;
  g_lastTwinkleMs = 0;
  IRect none = { 0, 0, 0, 0 };
  g_lastDyn = none;
  g_lastHaloBox = none;
  g_lastHx = g_lastHy = -1e9f; g_lastHa = -1.0f;
  return true;
}

void invalidate() { g_invalid = true; g_invalid2 = true; }

void render(LGFX& lcd, const tog::Toggle& toggle, NetStatus net, uint32_t nowMs) {
  if (!g_buf) return;
  uint32_t drawUs = 0, pushUs = 0;

  // Keep the screen's write session open for good: a strip then goes out by DMA in the
  // background while the CPU paints the next one (closing the session would wait for the DMA).
  if (!g_txOpen) { lcd.startWrite(); g_txOpen = true; }

  // ---- 1. does the page background need repainting? (only on a real colour change) ----
  float p = toggle.themeProgress();
  bool bgRepaint = g_invalid || fabsf(p - g_bgP) >= (1.0f / 64.0f) ||
                   (p != g_bgP && (p == 0.0f || p == 1.0f));      // land exactly on the final colours
  if (bgRepaint) {
    uint32_t t0 = nowUs();
    buildBackRows(p);
    lcd.startWrite();
    paintBackdropOutside(lcd);
    lcd.endWrite();
    g_bgP = p;
    pushUs += nowUs() - t0;
  }

  // ---- 2. status dot (painted straight on the screen, outside the sprite) ----
  lcd.startWrite();
  updateDot(lcd, net, nowMs, bgRepaint, g_backRow);
  lcd.endWrite();

  // ---- 3. what part of the toggle needs repainting? ----
  bool busy = toggle.busy();
  bool settle = g_wasBusy && !busy;               // one last frame after the motion stops
  g_wasBusy = busy;
  bool colours = g_invalid || bgRepaint || p != g_lastP;
  bool twinkle = false;
  if (!colours && !busy && !settle) {
    // Idle. Light mode (or no stars): nothing to do at all.
    if (toggle.theme() != tog::THEME_DARK || p > 0.0f) return;
    if ((uint32_t)(nowMs - g_lastTwinkleMs) < 66u) return;
    twinkle = true;
  }

  uint32_t t0 = nowUs();
  computeFrame(toggle, g_bgP);
  if (colours || g_lastP != p) buildTrackRows(p);
#if CYD_BENCH
  g_benchRec = busy;
  if (busy) { g_layerUs[7] += nowUs() - t0; g_layerFrames++; }
#endif
  IRect dirty;
  if (colours) {
    dirty = regionRect();
  } else if (twinkle) {
    dirty = starsBox(toggle);
    g_lastTwinkleMs = nowMs;
  } else {
    IRect dyn = knobBox();
    float ha = F.aHalo;
    // The halo is repainted when it has moved or faded, and BOTH its old and new
    // boxes are cleaned. The "last painted" numbers only advance when we really
    // repaint, so tiny moves add up instead of being lost.
    if (fabsf(F.hx - g_lastHx) > 0.001f || fabsf(F.hy - g_lastHy) > 0.001f || fabsf(ha - g_lastHa) > 0.0005f) {
      IRect hb = haloBox();
      dyn = rUnion(dyn, rUnion(hb, g_lastHaloBox));
      g_lastHaloBox = hb;
      g_lastHx = F.hx; g_lastHy = F.hy; g_lastHa = ha;
    }
    dirty = rUnion(dyn, g_lastDyn);
  }
  if (!twinkle) {
    g_lastDyn = knobBox();
    if (colours) { g_lastHx = F.hx; g_lastHy = F.hy; g_lastHa = F.aHalo; g_lastHaloBox = haloBox(); }
  }
  g_lastP = p;
  g_invalid = false;
  drawUs += nowUs() - t0;
  if (rEmpty(dirty)) { g_drawUs = drawUs; g_pushUs = pushUs; return; }

  // ---- 4. paint + send each band that overlaps the dirty rectangle ----
  g_ox = REG_X;
  for (int b = 0; b < g_bands; b++) {
    g_bandY = REG_Y + b * g_bandH;
    IRect band = { REG_X, g_bandY, REG_X + REG_W, g_bandY + g_bandH };
    IRect c = rClip(band, dirty);
    if (rEmpty(c)) continue;
    cX0 = c.x0; cY0 = c.y0; cX1 = c.x1; cY1 = c.y1;

    // Which buffer? With two, strips take turns, so the buffer we paint now was sent a whole
    // strip ago and is free. (If it is the one sent last, its DMA may still be running: wait.)
    int bi = g_nbuf == 2 ? (b & 1) : 0;
    if (g_lastPushed == bi) lcd.waitDMA();
    g_buf = g_bufs[bi];

    uint32_t t1 = nowUs();
    drawScene(toggle);
    uint32_t t2 = nowUs();
    lcd.setClipRect(c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0);   // only the dirty part goes over the wire
    g_spr[bi].pushSprite(&lcd, REG_X, g_bandY);              // starts the DMA and returns at once
    lcd.clearClipRect();
    g_lastPushed = bi;
    uint32_t t3 = nowUs();
    drawUs += t2 - t1;
    pushUs += t3 - t2;
  }
  g_drawUs = drawUs;
  g_pushUs = pushUs;
}

// ---------------------------------------------------------------------------
// Screen two and the screen change (public)
// ---------------------------------------------------------------------------
void renderSliders(LGFX& lcd, const tog::Slider& hue, const tog::Slider& bri, NetStatus net, uint32_t nowMs) {
  if (!g_buf) return;
  if (!g_txOpen) { lcd.startWrite(); g_txOpen = true; }
  uint32_t t0 = nowUs();
  bool force = g_invalid2;

  computeS2(hue, bri);
  const S2& s = g_s2;
  static S2 prev;
  IRect hueBox = knobBox2(s.hx, s.hy, s.hw, s.hh), briBox = knobBox2(s.bx, s.by, s.bw, s.bh);

  IRect dirty[6]; int nd = 0;
  if (force) {
    dirty[nd++] = screenRect();
  } else {
    bool accent = s.ar != prev.ar || s.ag != prev.ag || s.ab != prev.ab;
    bool hueMoved = accent || s.hx != prev.hx || s.hy != prev.hy || s.hw != prev.hw || s.hh != prev.hh || s.glowH != prev.glowH;
    bool briMoved = accent || s.bx != prev.bx || s.by != prev.by || s.bw != prev.bw || s.bh != prev.bh || s.glowB != prev.glowB;
    if (hueMoved) dirty[nd++] = rUnion(g_prevHueBox, hueBox);
    if (briMoved) {
      IRect d = rUnion(g_prevBriBox, briBox);
      if (accent) { IRect m = { BRI_FRAME_X, BRI_FRAME_Y, BRI_FRAME_X + BRI_FRAME_W, BRI_FRAME_Y + BRI_FRAME_H }; d = rUnion(d, m); }
      dirty[nd++] = d;
    }
    if (s.sunLevel != prev.sunLevel) {
      IRect sun = { BRI_CX - 8, 4, BRI_CX + 8, 20 }, moon = { BRI_CX - 8, 220, BRI_CX + 8, 236 };
      dirty[nd++] = sun;
      dirty[nd++] = moon;
    }
  }
  prev = s;
  g_prevHueBox = hueBox;
  g_prevBriBox = briBox;
  g_invalid2 = false;
  g_drawUs = nowUs() - t0;

  uint32_t t1 = nowUs();
  for (int i = 0; i < nd; i++) paintRect2(lcd, dirty[i]);
  g_pushUs = nowUs() - t1;

  // the quiet status dot (drawn straight on the screen, AFTER the paint so a full repaint cannot cover it)
  lcd.startWrite();
  updateDot(lcd, net, nowMs, force, g_back2);
  lcd.endWrite();
}

void beginTransition(bool /*toSliders*/) { g_tSteps = 0; }

void renderTransition(LGFX& lcd, bool toSliders, float progress, const tog::Toggle& toggle,
                      const tog::Slider& hue, const tog::Slider& bri, NetStatus net, uint32_t nowMs) {
  if (!g_buf) return;
  if (!g_txOpen) { lcd.startWrite(); g_txOpen = true; }
  int steps = (int)(progress * (float)WAVE_STEPS + 0.5f);
  steps = steps < 0 ? 0 : (steps > WAVE_STEPS ? WAVE_STEPS : steps);
  if (steps <= g_tSteps) return;

  // this frame's numbers for the screen we are heading to
  if (toSliders) {
    computeS2(hue, bri);
  } else {
    float p = toggle.themeProgress();
    buildBackRows(p);
    g_bgP = p;
    computeFrame(toggle, p);
    buildTrackRows(p);
  }

  uint32_t t0 = nowUs();
  for (int by = 0; by < BROWS; by++) {
    int bx = 0;
    while (bx < BCOLS) {
      int st = blockStep(bx, by, toSliders);
      if (!(st >= g_tSteps && st < steps)) { bx++; continue; }
      int bx2 = bx + 1;
      while (bx2 < BCOLS) {
        int s2 = blockStep(bx2, by, toSliders);
        if (!(s2 >= g_tSteps && s2 < steps)) break;
        bx2++;
      }
      IRect r = { bx * BLK, by * BLK, bx2 * BLK, by * BLK + BLK };
      if (toSliders) paintRect2(lcd, r); else paintToggleRect(lcd, toggle, r);
      bx = bx2;
    }
  }
  g_tSteps = steps;
  g_drawUs = nowUs() - t0;

  // The status dot sits inside the top-right block. Once that block has changed over, the dot is
  // drawn on the new screen again, so it only ever shows on whichever screen owns that block.
  if (blockStep(DOT_X / BLK, DOT_Y / BLK, toSliders) < steps) {
    lcd.startWrite();
    updateDot(lcd, net, nowMs, true, toSliders ? g_back2 : g_backRow);
    lcd.endWrite();
  }
}

void endTransition(bool toSliders) {
  g_tSteps = 0;
  g_dotColor = -1;                               // the wipe painted over the status dot
  if (toSliders) {
    g_invalid2 = true;
  } else {
    g_invalid = true;
    g_wasBusy = true;
  }
}

void layerStats(uint32_t out[8], uint32_t* frames) {
  for (int i = 0; i < 8; i++) { out[i] = g_layerUs[i]; g_layerUs[i] = 0; }
  *frames = g_layerFrames;
  g_layerFrames = 0;
}

size_t   bufferBytes() { return g_buf ? (size_t)REG_W * (size_t)g_bandH * 2u * (size_t)g_nbuf : 0u; }
uint8_t  bandCount()   { return g_bands; }
uint32_t lastDrawUs()  { return g_drawUs; }
uint32_t lastPushUs()  { return g_pushUs; }

}  // namespace gfx
