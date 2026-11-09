// display_config.h - tells the LovyanGFX library how the CYD's screen and touch chip are wired.
//
// The pin numbers below are the fixed wiring of the ESP32-2432S028R board
// (checked against github.com/witnessmenow/ESP32-Cheap-Yellow-Display):
//   screen  : SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, backlight 21   (HSPI bus)
//   touch   : CLK 25, MOSI 32, MISO 39, CS 33                        (its OWN bus)
// You should not need to edit this file. To fix colours, change CYD_VARIANT in config.h.
#pragma once
#include "config.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#if (CYD_VARIANT == 3) || (CYD_VARIANT == 4)
  #define CYD_IS_ST7789 1
  typedef lgfx::Panel_ST7789 CydPanel;
#else
  #define CYD_IS_ST7789 0
  typedef lgfx::Panel_ILI9341 CydPanel;
#endif

#if (CYD_VARIANT == 2) || (CYD_VARIANT == 4)
  #define CYD_INVERT_COLORS 1
#else
  #define CYD_INVERT_COLORS 0
#endif

class LGFX : public lgfx::LGFX_Device {
  CydPanel          _panel_instance;
  lgfx::Bus_SPI     _bus_instance;
  lgfx::Light_PWM   _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

 public:
  LGFX(void) {
    {  // screen bus
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;           // HSPI
      cfg.spi_mode    = 0;
      cfg.freq_write  = CYD_SPI_MHZ * 1000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {  // screen panel
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 15;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;
      cfg.panel_width  = 240;                // the panel is built portrait: 240 wide, 320 tall
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable   = false;                // we never read pixels back from the screen
      cfg.invert     = (CYD_INVERT_COLORS != 0);
      cfg.rgb_order  = (CYD_SWAP_RED_BLUE != 0);
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }
    {  // backlight
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {  // touch chip: XPT2046 on its own SPI pins (VSPI host)
      auto cfg = _touch_instance.config();
      cfg.x_min = 300;  cfg.x_max = 3900;    // rough defaults; real values come from calibration
      cfg.y_min = 3700; cfg.y_max = 200;
      cfg.pin_int    = -1;                   // the IRQ pin is not used
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI3_HOST;              // VSPI
      cfg.freq     = 1000000;
      cfg.pin_sclk = 25;
      cfg.pin_mosi = 32;
      cfg.pin_miso = 39;
      cfg.pin_cs   = 33;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};
