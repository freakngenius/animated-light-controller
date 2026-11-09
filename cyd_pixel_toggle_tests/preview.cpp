// preview.cpp - runs the REAL physics + drawing code on a laptop against the mock
// screen, and saves PNG pictures of what the CYD would show.
//
// It also checks two things automatically:
//   1. "dirty rectangle" correctness: after each normal (partial) frame, the same
//      frame is redrawn from scratch; the two screens must be identical.
//   2. band independence: the whole run is repeated with the sprite cut into
//      2 and into 4 strips; every saved frame must match pixel for pixel.
//
// Build + run: ./build_preview.sh   (writes PNGs to the folder given as argv[1])
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include "display_config.h"
#include "draw.h"
#include "physics.h"

size_t lgfx::mock_max_alloc = 0;

// ---------------------------------------------------------------------------
// Minimal PNG writer: uncompressed ("stored") deflate blocks + CRC32 + Adler32.
// ---------------------------------------------------------------------------
static uint32_t crcTable[256];
static void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> b;
  be32(b, (uint32_t)data.size());
  b.insert(b.end(), type, type + 4);
  b.insert(b.end(), data.begin(), data.end());
  uint32_t c = crc(&b[4], b.size() - 4) ^ 0xFFFFFFFFu;
  be32(b, c);
  fwrite(&b[0], 1, b.size(), f);
}
static bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  be32(ihdr, w); be32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
  }
  std::vector<uint8_t> z;
  z.push_back(0x78); z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    size_t n = raw.size() - pos; if (n > 65535) n = 65535;
    bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8);
    z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size(); i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  be32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", std::vector<uint8_t>());
  fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
static std::string g_out;
typedef std::vector<uint16_t> Shot;

static Shot grab(const LGFX& lcd) { return Shot(lcd.mockFrame(), lcd.mockFrame() + 320 * 240); }

static void to888(uint16_t v, uint8_t* o) {
  int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  o[0] = (uint8_t)((r << 3) | (r >> 2)); o[1] = (uint8_t)((g << 2) | (g >> 4)); o[2] = (uint8_t)((b << 3) | (b >> 2));
}
// Save a shot, optionally a crop, scaled up `k` times (nearest neighbour: pixels stay square).
static void savePng(const Shot& s, const std::string& name, int k = 1, int cx = 0, int cy = 0, int cw = 320, int ch = 240) {
  std::vector<uint8_t> rgb((size_t)cw * k * ch * k * 3);
  for (int y = 0; y < ch * k; y++) for (int x = 0; x < cw * k; x++)
    to888(s[(size_t)(cy + y / k) * 320 + (cx + x / k)], &rgb[((size_t)y * cw * k + x) * 3]);
  writePng(g_out + "/" + name + ".png", cw * k, ch * k, rgb);
}
static void sheet(const std::vector<Shot>& shots, const std::string& name, int k) {
  int W = 320 * 3 * k, H = 240 * 3 * k;
  std::vector<uint8_t> rgb((size_t)W * H * 3, 40);
  for (size_t i = 0; i < shots.size() && i < 9; i++) {
    int ox = (int)(i % 3) * 320 * k, oy = (int)(i / 3) * 240 * k;
    for (int y = 0; y < 240 * k; y++) for (int x = 0; x < 320 * k; x++) {
      if ((x < 2 || y < 2) && i) { continue; }                 // thin dark gutter between tiles
      to888(shots[i][(size_t)(y / k) * 320 + x / k], &rgb[((size_t)(oy + y) * W + ox + x) * 3]);
    }
  }
  writePng(g_out + "/" + name + ".png", W, H, rgb);
}

static int diffCount(const Shot& a, const Shot& b) {
  int n = 0;
  for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) n++;
  return n;
}

// ---------------------------------------------------------------------------
// One scripted run. Returns the named shots; checks dirty-rect correctness.
// ---------------------------------------------------------------------------
struct Named { std::string name; Shot shot; };

static int runScript(LGFX& lcd, bool save, std::vector<Named>* out, int* dirtyErrors, bool verbose) {
  if (!gfx::begin(lcd)) { fprintf(stderr, "gfx::begin failed\n"); return -1; }
  printf("  bands=%u bufferBytes=%u\n", (unsigned)gfx::bandCount(), (unsigned)gfx::bufferBytes());
  tog::Toggle t;
  t.begin(tog::THEME_DARK, 12345u);
  NetStatus net = NetStatus::Online;
  const float dt = 1.0f / 60.0f;
  uint32_t nowMs = 0;
  *dirtyErrors = 0;
  int slideIdx = 0;
  uint64_t sentTotal = 0; int framesWithPush = 0; uint64_t sentMax = 0;
  for (int fr = 0; fr <= 330; fr++) {
    // ---- script ----
    if (fr == 40)  t.setPressed(true);
    if (fr == 46)  { t.flip(); t.setPressed(false); }
    if (fr == 160) net = NetStatus::SearchingBulb;
    if (fr == 185) net = NetStatus::WifiDown;
    if (fr == 205) net = NetStatus::Online;
    if (fr == 210) { t.setPressed(true); }
    if (fr == 214) { t.flip(); t.setPressed(false); }

    if (fr > 0) t.update(dt);
    nowMs = (uint32_t)(fr * 1000 / 60);
    lcd.mockResetCounters();
    gfx::render(lcd, t, net, nowMs);
    uint64_t sent = lcd.mockPixelsSent();
    if (sent) { sentTotal += sent; framesWithPush++; if (sent > sentMax) sentMax = sent; }
    if (verbose && (fr % 10 == 0 || (fr >= 44 && fr <= 90 && sent)))
      printf("  fr %3d busy=%d p=%.3f cx=%.1f w=%.1f fl=%.2f ang=%.1f morph=%d sent=%6llu px  draw=%uus push=%uus\n",
             fr, (int)t.busy(), t.themeProgress(), t.centerX(), t.knobWidth(), t.flash(), t.rimAngleDeg(),
             (int)t.morphing(), (unsigned long long)sent, gfx::lastDrawUs(), gfx::lastPushUs());

    // ---- dirty-rect check every 3rd frame: redraw from scratch, compare ----
    Shot cur = grab(lcd);
    // Two differences are BY DESIGN and skipped here:
    //  * dark + idle: the stars only refresh every 66 ms, so between refreshes the
    //    screen is (intentionally) a little behind the twinkle clock;
    //  * mid colour flip: the page background is only repainted every 1/64 of the flip.
    float pr = t.themeProgress();
    bool idleDark = !t.busy() && t.theme() == tog::THEME_DARK && sent == 0;
    bool midFlip = pr > 0.0f && pr < 1.0f;
    if (fr % 3 == 0 && !idleDark && !midFlip) {
      gfx::invalidate();
      gfx::render(lcd, t, net, nowMs);
      Shot full = grab(lcd);
      int d = diffCount(cur, full);
      if (d) { (*dirtyErrors)++; printf("  !! frame %d: partial redraw differs from full redraw in %d px\n", fr, d); }
    }

    // ---- shots ----
    std::string name;
    if (fr == 30) name = "01_dark_rest";
    if (fr >= 47 && fr <= 86) {                       // every frame after the tap
      char b[32]; snprintf(b, sizeof b, "tap_f%02d", fr - 46); name = b;
    }
    if (fr == 150) name = "02_light_rest";
    if (fr == 175) name = "03_status_searching";
    if (fr == 200) name = "04_status_wifidown";
    if (fr == 330) name = "05_dark_again";
    if (fr >= 215 && fr <= 240 && (fr % 2 == 1)) { char b[32]; snprintf(b, sizeof b, "back_f%02d", fr - 214); name = b; }
    (void)slideIdx;
    if (!name.empty()) {
      Named n; n.name = name; n.shot = cur;
      out->push_back(n);
      if (save) savePng(cur, name);
    }
  }
  printf("  frames that pushed: %d, avg %llu px, max %llu px (full sprite = %d px)\n", framesWithPush,
         framesWithPush ? (unsigned long long)(sentTotal / framesWithPush) : 0ULL, (unsigned long long)sentMax, 294 * 159);
  return 0;
}

static const Shot* find(const std::vector<Named>& v, const char* n) {
  for (size_t i = 0; i < v.size(); i++) if (v[i].name == n) return &v[i].shot;
  return 0;
}

int main(int argc, char** argv) {
  crcInit();
  g_out = argc > 1 ? argv[1] : ".";
  LGFX lcd;
  int errs = 0;

  printf("Run A (SPRITE_BANDS from config.h):\n");
  std::vector<Named> A;
  int dirtyA = 0;
  runScript(lcd, true, &A, &dirtyA, true);
  // Raw dumps (RGB565, 320 x 240) so two builds of draw.cpp can be compared pixel by pixel.
  for (size_t i = 0; i < A.size(); i++) {
    std::string rp = g_out + "/raw_" + A[i].name + ".bin";
    FILE* rf = fopen(rp.c_str(), "wb");
    if (rf) { fwrite(&A[i].shot[0], 2, A[i].shot.size(), rf); fclose(rf); }
  }

  printf("Run B (RAM limited to 30000 bytes -> forces the 4-band fallback):\n");
  lgfx::mock_max_alloc = 30000;
  std::vector<Named> B;
  int dirtyB = 0;
  runScript(lcd, false, &B, &dirtyB, false);
  lgfx::mock_max_alloc = 0;

  int bandDiffs = 0;
  for (size_t i = 0; i < A.size() && i < B.size(); i++) {
    int d = diffCount(A[i].shot, B[i].shot);
    if (d) { bandDiffs++; printf("  !! %s differs between band counts in %d px\n", A[i].name.c_str(), d); }
  }
  printf("dirty-rect mismatches: run A %d, run B %d; band mismatches: %d\n", dirtyA, dirtyB, bandDiffs);
  errs = dirtyA + dirtyB + bandDiffs;

  // contact sheet: dark rest, slide, dissolve, cloud, flash, re-assemble, light rest, status dots
  const char* pick[9] = { "01_dark_rest", "tap_f02", "tap_f05", "tap_f08", "tap_f11",
                          "tap_f16", "tap_f24", "02_light_rest", "04_status_wifidown" };
  std::vector<Shot> sh;
  for (int i = 0; i < 9; i++) { const Shot* s = find(A, pick[i]); if (s) sh.push_back(*s); }
  sheet(sh, "contact_sheet_2x", 2);
  // zoomed crops of the knob for close inspection
  if (const Shot* s = find(A, "01_dark_rest")) savePng(*s, "zoom_dark_knob_4x", 4, 20, 45, 150, 150);
  if (const Shot* s = find(A, "02_light_rest")) savePng(*s, "zoom_light_knob_4x", 4, 150, 45, 160, 150);
  if (const Shot* s = find(A, "tap_f05")) savePng(*s, "zoom_flash_4x", 4, 60, 40, 200, 165);
  if (const Shot* s = find(A, "tap_f11")) savePng(*s, "zoom_sprite_edge_right_4x", 4, 240, 25, 80, 190);
  if (const Shot* s = find(A, "02_light_rest")) savePng(*s, "zoom_sprite_edge_bottom_4x", 4, 0, 170, 160, 45);
  if (const Shot* s = find(A, "01_dark_rest")) savePng(*s, "zoom_dark_bottom_4x", 4, 0, 160, 200, 60);
  if (const Shot* s = find(A, "03_status_searching")) savePng(*s, "zoom_dot_amber_8x", 8, 296, 2, 22, 20);
  if (const Shot* s = find(A, "04_status_wifidown")) savePng(*s, "zoom_dot_red_8x", 8, 296, 2, 22, 20);
  printf("%s\n", errs ? "CHECKS FAILED" : "all checks passed");
  return errs ? 1 : 0;
}
