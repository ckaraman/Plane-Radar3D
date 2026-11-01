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
  void beginHiddenFrame() {
    _bus.waitHidden();
    _panel.setTarget(_bus.hidden());
  }
  /** Show it at the next refresh; afterwards drawing goes to the visible buffer. */
  void setPanelPclk(uint32_t hz) { _bus.setPclk(hz); }
  /** The hidden frame buffer itself (800 x 480, byte-swapped RGB565), for fast copies. */
  uint16_t* hiddenBuffer() { return reinterpret_cast<uint16_t*>(_bus.hidden()); }
  /** Which of the three frame buffers hiddenBuffer() is (0..2). */
  int hiddenIndex() const { return _bus.hiddenIndex(); }
  uint32_t vsyncCount() const { return _bus.vsyncCount(); }
  uint32_t bufferWaits() const { return _bus.waitCount(); }
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

#endif  // BOARD_WS5
