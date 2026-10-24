#pragma once

#if defined(BOARD_WS5)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

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
 *
 * Two frame buffers: full-screen frames are drawn into the hidden one and shown at the
 * next frame boundary (present()), so a moving picture never shows half old, half new
 * (tearing). Direct drawing always goes to the visible buffer.
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
    pc.num_fbs = 2;
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
    // LovyanGFX keeps RGB565 pixels byte-swapped in memory and its Bus_RGB routes pin i
    // to LCD data signal i ^ 8 to undo that. Do the same here, or colours come out wrong.
    for (int i = 0; i < 16; ++i) pc.data_gpio_nums[i ^ 8] = pins[i];
    pc.flags.fb_in_psram = 1;

    // The driver refills the bounce buffers from an interrupt on the core that creates
    // the panel; that copying (the whole frame, ~39 times a second) would take a large
    // share of core 1, where the radar is drawn. Create it from a task on core 0
    // (Wi-Fi / network) instead.
    struct Ctx {
      esp_lcd_rgb_panel_config_t* pc;
      esp_lcd_panel_handle_t* out;
      esp_err_t err;
      SemaphoreHandle_t done;
    } ctx{&pc, &_handle, ESP_FAIL, xSemaphoreCreateBinary()};
    auto task = [](void* arg) {
      auto* c = static_cast<Ctx*>(arg);
      c->err = esp_lcd_new_rgb_panel(c->pc, c->out);
      if (c->err == ESP_OK) {
        esp_lcd_panel_reset(*c->out);
        esp_lcd_panel_init(*c->out);
      }
      xSemaphoreGive(c->done);
      vTaskDelete(nullptr);
    };
    if (xTaskCreatePinnedToCore(task, "lcdinit", 4096, &ctx, 5, nullptr, 0) != pdPASS) {
      ctx.err = esp_lcd_new_rgb_panel(&pc, &_handle);  // fall back to this core
      if (ctx.err == ESP_OK) {
        esp_lcd_panel_reset(_handle);
        esp_lcd_panel_init(_handle);
      }
    } else {
      xSemaphoreTake(ctx.done, portMAX_DELAY);
    }
    vSemaphoreDelete(ctx.done);
    if (ctx.err != ESP_OK) return false;
    void* fb0 = nullptr;
    void* fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(_handle, 2, &fb0, &fb1) != ESP_OK || fb0 == nullptr ||
        fb1 == nullptr) {
      return false;
    }
    _fbs[0] = static_cast<uint8_t*>(fb0);
    _fbs[1] = static_cast<uint8_t*>(fb1);
    _fb = _fbs[0];
    _fb_bytes = static_cast<size_t>(w) * h * 2;
    memset(_fbs[1], 0, _fb_bytes);
    _swapped = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_frame_buf_complete = onFrameDone;
    esp_lcd_rgb_panel_register_event_callbacks(_handle, &cbs, this);
    return true;
  }

  /** Pixel clock change (benchmark: measure memory speed without the panel's load). */
  void setPclk(uint32_t hz) { if (_handle) esp_lcd_rgb_panel_set_pclk(_handle, hz); }

  /** The buffer not on screen. */
  uint8_t* hidden() const { return _fb == _fbs[0] ? _fbs[1] : _fbs[0]; }
  uint8_t* visible() const { return _fb; }
  /**
   * Show the hidden buffer from the next frame on. Returns at once; the next
   * waitHidden() blocks until the panel has really switched (the driver switches
   * when it starts reading a new frame, right where it reports the previous one
   * complete), so the next frame is rendered while the panel catches up.
   */
  void present() {
    uint8_t* next = hidden();
    xSemaphoreTake(_swapped, 0);  // drop a stale signal
    esp_lcd_panel_draw_bitmap(_handle, 0, 0, _cfg.panel->config().panel_width,
                              _cfg.panel->config().panel_height, next);
    _fb = next;
    _pending = true;
  }
  /** Before drawing into hidden(): make sure the panel no longer reads it. */
  void waitHidden() {
    if (!_pending) return;
    xSemaphoreTake(_swapped, pdMS_TO_TICKS(60));
    _pending = false;
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
  static bool IRAM_ATTR onFrameDone(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*,
                                    void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<Bus_RGB_Bounce*>(ctx)->_swapped, &woken);
    return woken == pdTRUE;
  }

  config_t _cfg;
  esp_lcd_panel_handle_t _handle = nullptr;
  uint8_t* _fb = nullptr;
  uint8_t* _fbs[2] = {nullptr, nullptr};
  size_t _fb_bytes = 0;
  SemaphoreHandle_t _swapped = nullptr;
  volatile bool _pending = false;
};

/** Panel_RGB whose drawing target can be moved between the two frame buffers. */
class Panel_RGB_DB : public lgfx::Panel_RGB {
 public:
  void setTarget(uint8_t* fb) {
    if (_lines_buffer == nullptr || fb == nullptr) return;
    const int w = (_cfg.panel_width + 3) & ~3;
    const size_t stride = static_cast<size_t>(w) * 2;
    for (int i = 0; i < _cfg.panel_height; ++i) _lines_buffer[i] = fb + stride * i;
  }
};

#endif  // BOARD_WS5
