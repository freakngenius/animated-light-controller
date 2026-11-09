// mock/LovyanGFX.hpp - a tiny HOST-SIDE stand-in for the LovyanGFX library.
//
// It exists only so draw.cpp (and display_config.h) compile and run on a laptop,
// drawing into a 320 x 240 picture in memory that the preview tool saves as PNG.
// It copies the real library's function NAMES and argument order for the small
// subset we use, and its colour rules:
//   * a uint16_t (or int) colour is RGB565,  a uint32_t colour is RGB888;
//   * a 16-bit sprite keeps its pixels BYTE-SWAPPED in memory (like the real one,
//     which stores them in the order the screen wants them on the wire);
//   * drawing is clipped to the screen / sprite and to setClipRect().
// It is NOT the real library: nothing here talks to hardware.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SPI2_HOST 1
#define SPI3_HOST 2
#define SPI_DMA_CH_AUTO 3

namespace lgfx {

// Set this > 0 in a test to make createSprite() fail above that many bytes
// (simulates a board that is short of RAM).
extern size_t mock_max_alloc;

// ---- colour conversion: the "typed colour" rule ----
inline uint16_t mock_to565(uint16_t c) { return c; }
inline uint16_t mock_to565(int c) { return (uint16_t)c; }
inline uint16_t mock_to565(uint32_t c) {
  return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

// ---- configuration structs (permissive: every field display_config.h sets) ----
struct Bus_SPI {
  struct config_t {
    int spi_host = 0, spi_mode = 0; uint32_t freq_write = 0, freq_read = 0;
    bool spi_3wire = false, use_lock = true; int dma_channel = 0;
    int pin_sclk = -1, pin_mosi = -1, pin_miso = -1, pin_dc = -1;
  };
  config_t config() const { return cfg_; }
  void config(const config_t& c) { cfg_ = c; }
  config_t cfg_;
};
struct Light_PWM {
  struct config_t { int pin_bl = -1; bool invert = false; uint32_t freq = 0; int pwm_channel = 0; };
  config_t config() const { return cfg_; }
  void config(const config_t& c) { cfg_ = c; }
  config_t cfg_;
};
struct Touch_XPT2046 {
  struct config_t {
    int x_min = 0, x_max = 0, y_min = 0, y_max = 0, pin_int = -1; bool bus_shared = false;
    int offset_rotation = 0, spi_host = 0; uint32_t freq = 0;
    int pin_sclk = -1, pin_mosi = -1, pin_miso = -1, pin_cs = -1;
  };
  config_t config() const { return cfg_; }
  void config(const config_t& c) { cfg_ = c; }
  config_t cfg_;
};
struct Panel_Device {
  struct config_t {
    int pin_cs = -1, pin_rst = -1, pin_busy = -1;
    int panel_width = 240, panel_height = 320, offset_x = 0, offset_y = 0, offset_rotation = 0;
    int dummy_read_pixel = 8, dummy_read_bits = 1;
    bool readable = false, invert = false, rgb_order = false, dlen_16bit = false, bus_shared = false;
  };
  config_t config() const { return cfg_; }
  void config(const config_t& c) { cfg_ = c; }
  void setBus(Bus_SPI*) {}
  void setLight(Light_PWM*) {}
  void setTouch(Touch_XPT2046*) {}
  config_t cfg_;
};
struct Panel_ILI9341 : public Panel_Device {};
struct Panel_ST7789 : public Panel_Device {};

// ---- the drawing base class (the real one is also called LovyanGFX) ----
class LovyanGFX {
 public:
  virtual ~LovyanGFX() {}
  int32_t width() const { return w_; }
  int32_t height() const { return h_; }

  void setClipRect(int32_t x, int32_t y, int32_t w, int32_t h) { cl_ = x; ct_ = y; cr_ = x + w - 1; cb_ = y + h - 1; }
  void clearClipRect() { cl_ = 0; ct_ = 0; cr_ = w_ - 1; cb_ = h_ - 1; }

  void startWrite() { writeDepth_++; }
  void endWrite() { if (writeDepth_ > 0) writeDepth_--; }
  void waitDMA() {}

  template <typename T> void drawPixel(int32_t x, int32_t y, const T& c) { put(x, y, mock_to565(c)); }
  template <typename T> void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, const T& c) {
    uint16_t v = mock_to565(c);
    for (int32_t j = y; j < y + h; j++) for (int32_t i = x; i < x + w; i++) put(i, j, v);
    pixelsSent_ += (uint64_t)(w > 0 ? w : 0) * (uint64_t)(h > 0 ? h : 0);
  }
  template <typename T> void fillScreen(const T& c) { fillRect(0, 0, w_, h_, c); }
  template <typename T> void drawFastHLine(int32_t x, int32_t y, int32_t w, const T& c) { fillRect(x, y, w, 1, c); }
  template <typename T> void drawFastVLine(int32_t x, int32_t y, int32_t h, const T& c) { fillRect(x, y, 1, h, c); }
  template <typename T> void fillCircle(int32_t x, int32_t y, int32_t r, const T& c) {
    uint16_t v = mock_to565(c);
    for (int32_t j = -r; j <= r; j++) for (int32_t i = -r; i <= r; i++)
      if (i * i + j * j <= r * r + r) put(x + i, y + j, v);     // same "r*r + r" rule LovyanGFX uses (roughly)
  }
  template <typename T> void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const T& c) {
    uint16_t v = mock_to565(c);
    for (int32_t j = 0; j < h; j++) for (int32_t i = 0; i < w; i++) {
      int32_t dx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
      int32_t dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
      if (dx * dx + dy * dy <= r * r) put(x + i, y + j, v);
    }
  }
  uint16_t readPixel(int32_t x, int32_t y) const { return get(x, y); }

  // test helpers (not in the real library)
  uint64_t mockPixelsSent() const { return pixelsSent_; }
  void mockResetCounters() { pixelsSent_ = 0; }

 protected:
  friend class LGFX_Sprite;
  virtual void putRaw(int32_t x, int32_t y, uint16_t c565) = 0;
  virtual uint16_t getRaw(int32_t x, int32_t y) const = 0;
  void put(int32_t x, int32_t y, uint16_t c) {
    if (x < cl_ || x > cr_ || y < ct_ || y > cb_ || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    putRaw(x, y, c);
  }
  uint16_t get(int32_t x, int32_t y) const {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
    return getRaw(x, y);
  }
  int32_t w_ = 0, h_ = 0;
  int32_t cl_ = 0, ct_ = 0, cr_ = -1, cb_ = -1;
  int writeDepth_ = 0;
  uint64_t pixelsSent_ = 0;
};

// ---- the screen: 320 x 240 (landscape) RGB565 framebuffer ----
class LGFX_Device : public LovyanGFX {
 public:
  LGFX_Device() { w_ = 320; h_ = 240; memset(fb_, 0, sizeof(fb_)); clearClipRect(); }
  void setPanel(Panel_Device*) {}
  bool init() { return true; }
  bool begin() { return true; }
  void setRotation(uint8_t) {}
  void setBrightness(uint8_t) {}
  const uint16_t* mockFrame() const { return fb_; }
 protected:
  void putRaw(int32_t x, int32_t y, uint16_t c) { fb_[y * 320 + x] = c; }
  uint16_t getRaw(int32_t x, int32_t y) const { return fb_[y * 320 + x]; }
  uint16_t fb_[320 * 240];
};

// ---- an off-screen picture in RAM ----
class LGFX_Sprite : public LovyanGFX {
 public:
  LGFX_Sprite() {}
  explicit LGFX_Sprite(LovyanGFX*) {}
  ~LGFX_Sprite() { deleteSprite(); }
  void* setColorDepth(uint8_t bpp) { bpp_ = bpp; return buf_; }
  void* createSprite(int32_t w, int32_t h) {
    deleteSprite();
    size_t bytes = (size_t)w * (size_t)h * 2u;
    if (bpp_ != 16 || w <= 0 || h <= 0) return 0;
    if (mock_max_alloc && bytes > mock_max_alloc) return 0;
    buf_ = (uint16_t*)calloc((size_t)w * (size_t)h, 2);
    if (!buf_) return 0;
    w_ = w; h_ = h; clearClipRect();
    return buf_;
  }
  void deleteSprite() { free(buf_); buf_ = 0; w_ = h_ = 0; clearClipRect(); }
  void* getBuffer() const { return buf_; }
  // Copy the whole sprite onto dst at (x, y); dst's clip rectangle applies.
  void pushSprite(LovyanGFX* dst, int32_t x, int32_t y) {
    if (!buf_ || !dst) return;
    for (int32_t j = 0; j < h_; j++) for (int32_t i = 0; i < w_; i++) {
      int32_t dx = x + i, dy = y + j;
      if (dx < dst->cl_ || dx > dst->cr_ || dy < dst->ct_ || dy > dst->cb_) continue;
      if (dx < 0 || dy < 0 || dx >= dst->w_ || dy >= dst->h_) continue;
      dst->putRaw(dx, dy, getRaw(i, j));
      dst->pixelsSent_++;
    }
  }
 protected:
  void putRaw(int32_t x, int32_t y, uint16_t c) { buf_[y * w_ + x] = (uint16_t)((c >> 8) | (c << 8)); }
  uint16_t getRaw(int32_t x, int32_t y) const { uint16_t v = buf_[y * w_ + x]; return (uint16_t)((v >> 8) | (v << 8)); }
  uint16_t* buf_ = 0;
  uint8_t bpp_ = 16;
};

}  // namespace lgfx

// The real library also exposes these at global scope.
using lgfx::LovyanGFX;
using lgfx::LGFX_Sprite;
