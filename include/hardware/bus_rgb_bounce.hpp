#pragma once

#if defined(BOARD_WS5)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

/**
 * RGB panel bus for LovyanGFX's Panel_RGB that uses ESP-IDF 5's esp_lcd RGB driver
 * with bounce buffers.
 *
 * LovyanGFX's own Bus_RGB lets the LCD DMA read the frame buffer straight from PSRAM.
 * Whenever the CPU also works in PSRAM (drawing a frame), the DMA misses data and the
 * picture jumps sideways or drops out for a frame. With bounce buffers the DMA reads
 * from two small internal-RAM buffers which an interrupt refills from the PSRAM frame
 * buffer, which is what Waveshare's own demos for this board do.
 *
 * Takes the same config as lgfx::Bus_RGB, so the board setup code stays the same.
 */
class Bus_RGB_Bounce : public lgfx::IBus {
 public:
  using config_t = lgfx::Bus_RGB::config_t;
  static constexpr int kBounceLines = 10;

  config_t config() const { return _cfg; }
  void config(const config_t& cfg) { _cfg = cfg; }

  lgfx::bus_type_t busType() const override { return lgfx::bus_type_t::bus_unknown; }

  bool init() override {
    if (_fb != nullptr) return true;
    auto* panel = _cfg.panel;
    const int w = panel->config().panel_width;
    const int h = panel->config().panel_height;

    esp_lcd_rgb_panel_config_t pc = {};
    pc.clk_src = LCD_CLK_SRC_DEFAULT;
    pc.timings.pclk_hz = _cfg.freq_write;
    pc.timings.h_res = w;
    pc.timings.v_res = h;
    pc.timings.hsync_pulse_width = _cfg.hsync_pulse_width;
    pc.timings.hsync_back_porch = _cfg.hsync_back_porch;
    pc.timings.hsync_front_porch = _cfg.hsync_front_porch;
    pc.timings.vsync_pulse_width = _cfg.vsync_pulse_width;
    pc.timings.vsync_back_porch = _cfg.vsync_back_porch;
    pc.timings.vsync_front_porch = _cfg.vsync_front_porch;
    pc.timings.flags.hsync_idle_low = _cfg.hsync_polarity ? 1 : 0;
    pc.timings.flags.vsync_idle_low = _cfg.vsync_polarity ? 1 : 0;
    pc.timings.flags.de_idle_high = _cfg.de_idle_high;
    pc.timings.flags.pclk_active_neg = _cfg.pclk_active_neg;
    pc.timings.flags.pclk_idle_high = _cfg.pclk_idle_high;
    pc.data_width = 16;
    pc.bits_per_pixel = 16;
    pc.num_fbs = 1;
    pc.bounce_buffer_size_px = static_cast<size_t>(w) * kBounceLines;
    pc.dma_burst_size = 64;
    pc.hsync_gpio_num = _cfg.pin_hsync;
    pc.vsync_gpio_num = _cfg.pin_vsync;
    pc.de_gpio_num = _cfg.pin_henable;
    pc.pclk_gpio_num = _cfg.pin_pclk;
    pc.disp_gpio_num = -1;
    const int8_t pins[16] = {_cfg.pin_d0,  _cfg.pin_d1,  _cfg.pin_d2,  _cfg.pin_d3,
                             _cfg.pin_d4,  _cfg.pin_d5,  _cfg.pin_d6,  _cfg.pin_d7,
                             _cfg.pin_d8,  _cfg.pin_d9,  _cfg.pin_d10, _cfg.pin_d11,
                             _cfg.pin_d12, _cfg.pin_d13, _cfg.pin_d14, _cfg.pin_d15};
    for (int i = 0; i < 16; ++i) pc.data_gpio_nums[i] = pins[i];
    pc.flags.fb_in_psram = 1;

    if (esp_lcd_new_rgb_panel(&pc, &_handle) != ESP_OK) return false;
    esp_lcd_panel_reset(_handle);
    esp_lcd_panel_init(_handle);
    void* fb = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(_handle, 1, &fb) != ESP_OK || fb == nullptr) {
      return false;
    }
    _fb = static_cast<uint8_t*>(fb);
    return true;
  }
  void release() override {}

  void beginTransaction() override {}
  void endTransaction() override {}
  void wait() override {}
  bool busy() const override { return false; }
  void initDMA() override {}
  void addDMAQueue(const uint8_t*, uint32_t) override {}
  void execDMAQueue() override {}
  uint8_t* getDMABuffer(uint32_t) override { return _fb; }
  void flush() override {}
  bool writeCommand(uint32_t, uint_fast8_t) override { return true; }
  void writeData(uint32_t, uint_fast8_t) override {}
  void writeDataRepeat(uint32_t, uint_fast8_t, uint32_t) override {}
  void writePixels(lgfx::pixelcopy_t*, uint32_t) override {}
  void writeBytes(const uint8_t*, uint32_t, bool, bool) override {}
  void beginRead() override {}
  void endRead() override {}
  uint32_t readData(uint_fast8_t) override { return 0; }
  bool readBytes(uint8_t*, uint32_t, bool) override { return false; }
  void readPixels(void*, lgfx::pixelcopy_t*, uint32_t) override {}

 private:
  config_t _cfg;
  esp_lcd_panel_handle_t _handle = nullptr;
  uint8_t* _fb = nullptr;
};

#endif  // BOARD_WS5
