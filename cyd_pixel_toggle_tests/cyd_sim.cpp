// cyd_sim.cpp - runs the REAL app code (gesture recogniser, screen controller, physics, drawing)
// on your laptop against a pretend screen, driven by a little touch script.
//
//   sh cyd_sim.sh SCRIPT [OUTDIR]
//
// SCRIPT is a text file, one command per line, `#` starts a comment. Coordinates are screen
// pixels, landscape: x 0..319, y 0..239. Time is simulated: the loop runs every 8 ms, like the
// touch chip is read on the board.
//
//   wait MS                    run the loop for MS milliseconds
//   down X Y                   put a finger on the glass at X Y (no time passes)
//   move X Y                   move the finger that is down to X Y (no time passes)
//   up                         lift the finger (no time passes)
//   tap X Y                    down, 40 ms, up, 40 ms
//   swipe X1 Y1 X2 Y2 MS       down at X1 Y1, glide to X2 Y2 in a straight line over MS ms, lift
//   drag X1 Y1 X2 Y2 MS        the same, but the finger stays down (use `up` yourself)
//   settle                     run until nothing moves any more (at most 5 seconds)
//   net online|searching|wifidown   what the Wi-Fi/bulb link looks like
//   shot NAME                  save the screen as OUTDIR/NAME.png
//   state                      print one line describing the app (see below)
//
// The `state` line:
//   screen=1|2 transition=0|1 dark=0|1 hue=DEGREES brightness=PERCENT
//   bulb_power=on|off|unset bulb_hue=DEGREES|unset bulb_brightness=PERCENT|unset bulb_packets=COUNT
// `screen` is the screen the finger is on (during a change it is the one being left).
// `dark` is 1 when the toggle on screen one is in dark mode.
// The bulb fields show what the app asked the (pretend) bulb to do.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <string>
#include <vector>
#include "display_config.h"
#include "app.h"
#include "draw.h"
#include "gesture.h"
#include "layout.h"
#include "lifx.h"
#include "png_writer.h"

size_t lgfx::mock_max_alloc = 0;

// ---------------------------------------------------------------------------
// A pretend bulb: it only remembers what the app asked for.
// ---------------------------------------------------------------------------
namespace {
struct FakeBulb {
  bool powerSet = false, power = false;
  bool colourSet = false; uint16_t hue = 0, bri = 0;
  uint32_t packets = 0;                 // every request the app made to the bulb
  NetStatus net = NetStatus::Online;
} g_bulb;
}

namespace lifxnet {
void begin() {}
void setDesired(bool on) { g_bulb.powerSet = true; g_bulb.power = on; g_bulb.packets++; }
void setDesiredColor(uint16_t hue, uint16_t brightness) {
  if (g_bulb.colourSet && g_bulb.hue == hue && g_bulb.bri == brightness) return;   // same as before: nothing happens
  g_bulb.colourSet = true; g_bulb.hue = hue; g_bulb.bri = brightness; g_bulb.packets++;
}
bool colorPending() { return false; }
bool writePending() { return false; }
NetStatus status() { return g_bulb.net; }
Reading reading() { Reading r = { false, false, 0, 0 }; return r; }
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
static LGFX g_lcd;
static gesture::Recognizer g_rec;
static uint32_t g_now = 0;
static bool g_down = false;
static int g_fx = 0, g_fy = 0;
static std::string g_out = ".";
static const uint32_t STEP_MS = 8;

static void step() {
  g_now += STEP_MS;
  gesture::Event ev = g_rec.feed(g_now, g_down, g_fx, g_fy);
  app::frame(g_lcd, ev, g_now, STEP_MS * 0.001f);
}
static void waitMs(int ms) { for (int t = 0; t < ms; t += STEP_MS) step(); }

static bool idle() {
  return !app::controller().busy() && !app::toggle().busy();
}

static void state() {
  const ui::Controller& c = app::controller();
  char hue[16] = "unset", bri[16] = "unset";
  if (g_bulb.colourSet) {
    snprintf(hue, sizeof hue, "%.0f", g_bulb.hue * 360.0 / 65535.0);
    snprintf(bri, sizeof bri, "%.0f", g_bulb.bri * 100.0 / 65535.0);
  }
  printf("screen=%d transition=%d dark=%d hue=%.0f brightness=%.0f bulb_power=%s bulb_hue=%s bulb_brightness=%s bulb_packets=%u\n",
         c.screen() == ui::Screen::Toggle ? 1 : 2, c.transitioning() ? 1 : 0,
         app::toggle().theme() == tog::THEME_DARK ? 1 : 0,
         layout::hueDegrees(c.hueValue()), layout::brightnessFraction(c.brightnessValue()) * 100.0f,
         g_bulb.powerSet ? (g_bulb.power ? "on" : "off") : "unset", hue, bri, (unsigned)g_bulb.packets);
}

static void shot(const std::string& name) {
  const uint16_t* fb = g_lcd.mockFrame();
  std::vector<uint8_t> rgb(320 * 240 * 3);
  for (int i = 0; i < 320 * 240; i++) {
    uint16_t v = fb[i];
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
  }
  std::string path = g_out + "/" + name + ".png";
  if (!writePng(path, 320, 240, rgb)) { fprintf(stderr, "cannot write %s\n", path.c_str()); exit(2); }
  printf("shot %s\n", path.c_str());
}

static void glide(int x1, int y1, int x2, int y2, int ms) {
  int n = ms / (int)STEP_MS; if (n < 1) n = 1;
  g_down = true; g_fx = x1; g_fy = y1;
  step();
  for (int i = 1; i <= n; i++) {
    g_fx = x1 + (x2 - x1) * i / n;
    g_fy = y1 + (y2 - y1) * i / n;
    step();
  }
}

int main(int argc, char** argv) {
  crcInit();
  if (argc < 2) { fprintf(stderr, "usage: cyd_sim SCRIPT [OUTDIR]\n"); return 2; }
  if (argc > 2) g_out = argv[2];
  FILE* f = fopen(argv[1], "r");
  if (!f) { fprintf(stderr, "cannot open script %s\n", argv[1]); return 2; }

  if (!gfx::begin(g_lcd)) { fprintf(stderr, "gfx::begin failed\n"); return 2; }
  app::begin(12345u);
  waitMs(200);                           // let the first frames paint

  char line[256];
  int lineNo = 0;
  while (fgets(line, sizeof line, f)) {
    lineNo++;
    char* hash = strchr(line, '#'); if (hash) *hash = 0;
    char cmd[32] = ""; int a = 0, b = 0, c = 0, d = 0, e = 0; char word[64] = "";
    int n = sscanf(line, "%31s", cmd);
    if (n != 1) continue;
    if (!strcmp(cmd, "wait") && sscanf(line, "%*s %d", &a) == 1) { waitMs(a); }
    else if (!strcmp(cmd, "down") && sscanf(line, "%*s %d %d", &a, &b) == 2) { g_down = true; g_fx = a; g_fy = b; }
    else if (!strcmp(cmd, "move") && sscanf(line, "%*s %d %d", &a, &b) == 2) { g_fx = a; g_fy = b; }
    else if (!strcmp(cmd, "up")) { g_down = false; }
    else if (!strcmp(cmd, "tap") && sscanf(line, "%*s %d %d", &a, &b) == 2) { g_down = true; g_fx = a; g_fy = b; waitMs(40); g_down = false; waitMs(40); }
    else if (!strcmp(cmd, "swipe") && sscanf(line, "%*s %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) { glide(a, b, c, d, e); g_down = false; waitMs(40); }
    else if (!strcmp(cmd, "drag") && sscanf(line, "%*s %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) { glide(a, b, c, d, e); }
    else if (!strcmp(cmd, "settle")) { int t = 0; while (t < 5000 && !(idle() && !g_down)) { step(); t += STEP_MS; } waitMs(40); }
    else if (!strcmp(cmd, "net") && sscanf(line, "%*s %63s", word) == 1) {
      if (!strcmp(word, "online")) g_bulb.net = NetStatus::Online;
      else if (!strcmp(word, "searching")) g_bulb.net = NetStatus::SearchingBulb;
      else if (!strcmp(word, "wifidown")) g_bulb.net = NetStatus::WifiDown;
      else { fprintf(stderr, "line %d: net wants online, searching or wifidown\n", lineNo); return 2; }
    }
    else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %63s", word) == 1) { shot(word); }
    else if (!strcmp(cmd, "state")) { state(); }
    else { fprintf(stderr, "line %d: cannot understand: %s", lineNo, line); fclose(f); return 2; }
  }
  fclose(f);
  return 0;
}
