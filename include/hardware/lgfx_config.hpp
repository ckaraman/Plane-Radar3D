#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

/**
 * LovyanGFX device: 4.0" ST7796S (320x480 native, used in landscape) on VSPI,
 * XPT2046 resistive touch sharing the same bus, PWM backlight.
 * invert / rgb_order are applied at runtime from display_prefs (displayInit).
 */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ST7796 _panel;
  lgfx::Touch_XPT2046 _touch;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = config::kDisplaySpiWriteHz;
      cfg.freq_read = config::kDisplaySpiReadHz;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = config::kSpiPinSclk;
      cfg.pin_mosi = config::kSpiPinMosi;
      cfg.pin_miso = config::kSpiPinMiso;
      cfg.pin_dc = config::kDisplayPinDc;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = config::kDisplayPinCs;
      cfg.pin_rst = config::kDisplayPinRst;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;   // LCD SDO is not wired: MISO belongs to the touch chip
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;  // touch on the same SPI bus
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = config::kDisplayPinBacklight;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      // Rough raw range; the on-screen calibration (settings menu) replaces it.
      cfg.x_min = 200;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3900;
      cfg.pin_int = config::kTouchPinIrq;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = VSPI_HOST;
      cfg.freq = config::kTouchSpiHz;
      cfg.pin_sclk = config::kSpiPinSclk;
      cfg.pin_mosi = config::kSpiPinMosi;
      cfg.pin_miso = config::kSpiPinMiso;
      cfg.pin_cs = config::kTouchPinCs;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }

  /** Apply color inversion / red-blue order chosen in the settings menu. */
  void applyPanelColorOptions(bool invert, bool bgr) {
    auto cfg = _panel.config();
    cfg.invert = invert;
    cfg.rgb_order = bgr;
    _panel.config(cfg);
    _panel.setInvert(invert);
  }
};
