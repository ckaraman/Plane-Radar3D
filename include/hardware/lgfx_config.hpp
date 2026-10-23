#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

#if defined(BOARD_WS5)
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

#include "hardware/bus_rgb_bounce.hpp"
#include "hardware/ch422g.h"

/** Backlight on the CH422G expander: on / off only. */
class Light_CH422G : public lgfx::ILight {
 public:
  bool init(uint8_t brightness) override {
    setBrightness(brightness);
    return true;
  }
  void setBrightness(uint8_t brightness) override {
    hw::ch422g::write(hw::ch422g::kBacklight, brightness > 0);
  }
};

/**
 * LovyanGFX device: Waveshare ESP32-S3-Touch-LCD-5, 800x480 RGB panel (frame buffer
 * in PSRAM), GT911 capacitive touch on the shared I2C bus, backlight via the CH422G.
 * Panel timing: Waveshare's values for this board (16 MHz pixel clock, inverted clock).
 */
class LGFX : public lgfx::LGFX_Device {
  Bus_RGB_Bounce _bus;  // esp_lcd with bounce buffers (see bus_rgb_bounce.hpp)
  Panel_RGB_DB _panel;
  lgfx::Touch_GT911 _touch;
  Light_CH422G _light;

 public:
  /** Full-screen frame: draw into the hidden buffer between these two calls. */
  void beginHiddenFrame() { _panel.setTarget(_bus.hidden()); }
  /** Show it at the next refresh; afterwards drawing goes to the visible buffer. */
  void presentHiddenFrame() {
    _bus.present();
    _panel.setTarget(_bus.visible());
  }

  LGFX() {
    {
      auto cfg = _panel.config();
      cfg.memory_width = config::kDisplayWidth;
      cfg.memory_height = config::kDisplayHeight;
      cfg.panel_width = config::kDisplayWidth;
      cfg.panel_height = config::kDisplayHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _panel.config_detail();
      cfg.use_psram = 1;
      _panel.config_detail(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      // B0..B4, G0..G5, R0..R4
      cfg.pin_d0 = 14;
      cfg.pin_d1 = 38;
      cfg.pin_d2 = 18;
      cfg.pin_d3 = 17;
      cfg.pin_d4 = 10;
      cfg.pin_d5 = 39;
      cfg.pin_d6 = 0;
      cfg.pin_d7 = 45;
      cfg.pin_d8 = 48;
      cfg.pin_d9 = 47;
      cfg.pin_d10 = 21;
      cfg.pin_d11 = 1;
      cfg.pin_d12 = 2;
      cfg.pin_d13 = 42;
      cfg.pin_d14 = 41;
      cfg.pin_d15 = 40;
      cfg.pin_henable = 5;
      cfg.pin_vsync = 3;
      cfg.pin_hsync = 46;
      cfg.pin_pclk = 7;
#if defined(BOARD_WS5B)
      // 1024x600 "5B" panel: timing proven with LovyanGFX on this board
      // (VaAndCob/Waveshare-ESP32-S3-Touch-5). 14 MHz keeps the PSRAM frame buffer
      // read within budget (LovyanGFX has no bounce buffers).
      cfg.freq_write = 14000000;
      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 40;
      cfg.hsync_pulse_width = 20;
      cfg.hsync_back_porch = 40;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 40;
      cfg.vsync_pulse_width = 20;
      cfg.vsync_back_porch = 40;
      cfg.pclk_active_neg = 0;
#else
      // 800x480 panel: porches from Waveshare's own values for ESP32-S3-Touch-LCD-5
      // (ESP32_Display_Panel, BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_5). Their 16 MHz relies
      // on bounce buffers that LovyanGFX does not have: the panel reads the frame buffer
      // straight from PSRAM, so frames are copied into it in bands to leave the panel
      // enough bandwidth (hardware/frame_push.h). 12 MHz was too slow for this panel
      // (white screen).
      cfg.freq_write = 16000000;
      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch = 8;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch = 8;
      cfg.pclk_active_neg = 1;
#endif
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    _panel.light(&_light);
    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = config::kDisplayWidth - 1;
      cfg.y_min = 0;
      cfg.y_max = config::kDisplayHeight - 1;
      cfg.pin_int = config::kTouchPinInt;
      cfg.pin_rst = -1;  // on the CH422G: done in displayInit()
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = config::kI2cPort;
      cfg.pin_sda = config::kI2cPinSda;
      cfg.pin_scl = config::kI2cPinScl;
      cfg.freq = 400000;
      cfg.i2c_addr = 0x5D;  // the driver also tries 0x14
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }

  /** IPS RGB panel: no inversion / colour-order options. */
  void applyPanelColorOptions(bool, bool) {}
};

#else

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

#endif  // BOARD_WS5
