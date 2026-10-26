/**
 * Plane Radar 3D — ESP32-WROOM-32U + 4.0" ST7796S (480x320, XPT2046 touch) + SHT30.
 *
 * Same services as the round plane-radar-clock build (USB setup page, adsb.fi,
 * NTP clock, Open-Meteo weather, map layer); the radar itself is a perspective
 * view with touch controls.
 */

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <WiFi.h>

#include <cmath>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/clock_service.h"
#include "services/display_prefs.h"
#include "services/map_store.h"
#include "services/radar_location.h"
#include "services/route_service.h"
#include "services/serial_ota.h"
#include "services/sht30.h"
#include "services/terrain.h"
#include "services/usb_feed.h"
#include "services/weather_service.h"
#include "services/water_store.h"
#include "services/wifi_setup.h"
#include "ui/clock_display.h"
#include "ui/radar3d.h"
#include "ui/radar_range.h"
#include "ui/screen_switch.h"
#include "ui/settings_menu.h"
#include "ui/status_screens.h"
#include "ui/water_screen.h"
#include "ui/weather_display.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_redraw_ms = 0;

enum class Screen { Radar, Clock, Weather, Water };
volatile Screen g_screen = Screen::Radar;  // read by the network task too

unsigned long g_last_aircraft_ms = 0;

// Touch state (polled; the XPT2046 shares the SPI bus with the panel).
bool g_touch_down = false;
bool g_touch_long_fired = false;
unsigned long g_touch_down_ms = 0;
// Resistive panels give a noisy first sample, so a tap uses the average of the
// samples taken while the finger was down (the first one left out).
long g_touch_sum_x = 0;
long g_touch_sum_y = 0;
int g_touch_samples = 0;
int g_touch_first_x = 0;
int g_touch_first_y = 0;

/** Data source available: Wi-Fi, or aircraft arriving from the PC over USB. */
bool online() { return WiFi.status() == WL_CONNECTED || services::usb_feed::active(); }

void showRadarIfConnected() {
  if (!online()) {
    g_radar_visible = false;
    return;
  }
  services::clock::startSync();  // no-op after the first call
  g_screen = Screen::Radar;
  g_last_aircraft_ms = millis();
  ui::radar3d::drawFrame();
  g_radar_visible = true;
}

/** Aircraft inside the selected range (the fetch, and the USB bridge, cover more). */
size_t aircraftInRange() {
  return services::adsb::countWithin(services::location::lat(), services::location::lon(),
                                     ui::radar::rangeCurrent().outer_km);
}

void repaintScreen();

#if defined(BOARD_WS5)
/**
 * One-off speed test at boot, printed to the serial log: PSRAM write / read / copy
 * and a CPU loop, with the panel running normally and again with its pixel clock
 * turned down (no display load on PSRAM). Tells what limits the frame rate.
 */
void benchMemory() {
  constexpr size_t kN = 256 * 1024;
  auto* a = static_cast<uint8_t*>(heap_caps_malloc(kN, MALLOC_CAP_SPIRAM));
  auto* b = static_cast<uint8_t*>(heap_caps_malloc(kN, MALLOC_CAP_SPIRAM));
  auto* sram = static_cast<uint8_t*>(heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!a || !b || !sram) {
    Serial.println("bench: no memory");
  } else {
    for (int pass = 0; pass < 2; ++pass) {
      if (pass == 1) {
        tft.setPanelPclk(2000000);
        delay(100);
      }
      uint32_t t = micros();
      memset(a, pass + 1, kN);
      const uint32_t wr = micros() - t;
      t = micros();
      volatile uint32_t sum = 0;
      const uint32_t* w = reinterpret_cast<const uint32_t*>(b);
      for (size_t i = 0; i < kN / 4; ++i) sum += w[i];
      const uint32_t rd = micros() - t;
      t = micros();
      memcpy(b, a, kN);
      const uint32_t cp = micros() - t;
      t = micros();
      for (int k = 0; k < 16; ++k) memset(sram, k, 16384);
      const uint32_t sw = micros() - t;
      t = micros();
      volatile float f = 1.0f;
      for (int i = 0; i < 200000; ++i) f = f * 1.0000001f + 0.5f;
      const uint32_t cpu = micros() - t;
      Serial.printf("bench (%s): psram write %.1f MB/s, read %.1f MB/s, copy %.1f MB/s; sram write %.1f MB/s; cpu loop %lu us\n",
                    pass == 0 ? "panel on" : "panel slow", kN / (float)wr, kN / (float)rd, kN / (float)cp,
                    16 * 16384 / (float)sw, cpu);
    }
    tft.setPanelPclk(16000000);
  }
  heap_caps_free(a);
  heap_caps_free(b);
  heap_caps_free(sram);
}
#endif

// Runs at the redraw cadence: decide radar / clock / weather, then draw.
void updateScreen() {
  const unsigned long now = millis();
  const size_t aircraft = aircraftInRange();
  const bool have_data = services::adsb::lastUpdateMs() != 0;
  if (aircraft > 0 || !have_data) {
    g_last_aircraft_ms = now;
  }

  // Water reminder / amount picker / goal setting on top of any screen.
  if (ui::water::overlayActive()) {
    if (!ui::water::overlayTick()) repaintScreen();
    return;
  }
  if (ui::water::reminderDue()) {
    ui::water::openReminder();
    return;
  }
  // Forecast / water screens stay until the switch button (or BOOT) moves on.
  if (g_screen == Screen::Weather) {
    ui::weatherDisplayTick();
    return;
  }
  if (g_screen == Screen::Water) {
    ui::water::tick();
    return;
  }

  const bool idle = services::clock::enabled() && services::clock::synced() &&
                    now - g_last_aircraft_ms >= config::kClockIdleDelayMs;
  if (idle) {
    if (g_screen != Screen::Clock) {
      g_screen = Screen::Clock;
      ui::clockDisplayEnter();
    }
    ui::clockDisplayTick();
    return;
  }

  g_screen = Screen::Radar;
  ui::radar3d::drawFrame();
}

/** Redraw the current screen after an overlay closed. */
void repaintScreen() {
  switch (g_screen) {
    case Screen::Clock: ui::clockDisplayEnter(); break;
    case Screen::Weather: ui::weatherDisplayEnter(); break;
    case Screen::Water: ui::water::enter(); break;
    case Screen::Radar: break;
  }
  g_last_redraw_ms = 0;
}

/** Back to the radar (from the clock or the forecast screen). */
void wakeRadar() {
  g_last_aircraft_ms = millis();
  g_screen = Screen::Radar;
  g_last_redraw_ms = 0;  // redraw right away
}

void onBootTap() {
  if (g_screen != Screen::Radar) {
    wakeRadar();
    return;
  }
  ui::radar::rangeNext();
  char label[12];
  ui::radar::formatCurrentRing3Label(label, sizeof(label));
  Serial.printf("Range: %s\n", label);
  g_last_redraw_ms = 0;
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onBootTap();
  }
}

void openWeather() {
  g_screen = Screen::Weather;
  ui::weatherDisplayEnter();
}

void openWater() {
  g_screen = Screen::Water;
  ui::water::enter();
}

void onTouchTap(int x, int y) {
  if (ui::water::overlayActive()) {
    ui::water::overlayTap(x, y);
    if (!ui::water::overlayActive()) repaintScreen();
    return;
  }
  if (g_screen == Screen::Weather) {
    if (ui::switchButtonHit(x, y)) {
      wakeRadar();
    } else if (ui::waterButtonHit(x, y)) {
      openWater();
    } else {
      ui::weatherDisplayTap(x, y);
    }
    return;
  }
  if (g_screen == Screen::Clock) {
    const int hit = ui::clockDisplayHit(x, y);
    if (hit == 1) {
      openWater();
    } else if (hit == 2) {
      openWeather();
    } else {
      wakeRadar();
    }
    return;
  }
  if (g_screen == Screen::Water) {
    if (ui::switchButtonHit(x, y)) {
      wakeRadar();
    } else {
      ui::water::tap(x, y);
    }
    return;
  }
  if (g_screen != Screen::Radar) {
    wakeRadar();
    return;
  }
  if (ui::switchButtonHit(x, y)) {
    openWeather();
    return;
  }
#if defined(BOARD_WS5)
  if (ui::radarWaterButtonHit(x, y)) {
#else
  if (ui::waterButtonHit(x, y)) {
#endif
    openWater();
    return;
  }
  if (ui::radar3d::handleTap(x, y)) {
    g_last_redraw_ms = 0;
  }
}

#if defined(BOARD_WS5)
// 5" (capacitive, multi-touch): a short touch is a tap; a finger that moves more than
// a few pixels turns (sideways) and tilts (up/down) the map; two fingers pinching
// change the range. Holding still for 1.5 s opens the settings.
void pollTouch() {
  constexpr int kSlop = 12;  // px a tap may wander before it counts as a drag
  static bool gesture = false, pinch = false;
  static int last_x = 0, last_y = 0;
  static unsigned long last_ms = 0;
  static float vel_x = 0.0f, pinch_base = 0.0f;
  lgfx::touch_point_t tps[2];
  const int n = tft.getTouch(tps, 2);
  const unsigned long now = millis();
  const bool on_radar = g_screen == Screen::Radar && !ui::water::overlayActive();
  if (n > 0) {
    if (!g_touch_down) {
      g_touch_down = true;
      g_touch_long_fired = false;
      g_touch_down_ms = now;
      g_touch_first_x = tps[0].x;
      g_touch_first_y = tps[0].y;
      g_touch_sum_x = 0;
      g_touch_sum_y = 0;
      g_touch_samples = 0;
      gesture = pinch = false;
      last_x = tps[0].x;
      last_y = tps[0].y;
      last_ms = now;
      vel_x = 0.0f;
    }
    if (on_radar && n >= 2) {  // pinch: range one step per clear change of the finger gap
      const float d = hypotf(static_cast<float>(tps[0].x - tps[1].x), static_cast<float>(tps[0].y - tps[1].y));
      if (!pinch) {
        if (gesture) ui::radar3d::dragEnd(0.0f);
        pinch = gesture = true;
        pinch_base = d;
      } else if (pinch_base > 20.0f) {
        if (d > pinch_base * 1.4f) {  // fingers apart: closer in
          ui::radar3d::pinchStep(-1);
          pinch_base = d;
        } else if (d < pinch_base * 0.7f) {  // together: wider
          ui::radar3d::pinchStep(1);
          pinch_base = d;
        }
      }
      return;
    }
    if (pinch) return;  // after a pinch the last finger does nothing until all lift
    const int x = tps[0].x, y = tps[0].y;
    if (!gesture && on_radar &&
        (abs(x - g_touch_first_x) > kSlop || abs(y - g_touch_first_y) > kSlop)) {
      gesture = true;
      ui::radar3d::dragBegin();
    }
    if (gesture) {
      ui::radar3d::dragBy(x - last_x, y - last_y);
      const unsigned long dt = now - last_ms;
      if (dt > 0) vel_x = 0.5f * vel_x + 0.5f * (x - last_x) * 1000.0f / dt;
      last_x = x;
      last_y = y;
      last_ms = now;
      return;
    }
    g_touch_sum_x += x;
    g_touch_sum_y += y;
    ++g_touch_samples;
    if (!g_touch_long_fired && now - g_touch_down_ms >= config::kTouchLongPressMs) {
      g_touch_long_fired = true;
      ui::settingsMenuRun();
      // Back from the menu: repaint whatever screen is active.
      repaintScreen();
      g_touch_down = false;
    }
    return;
  }
  if (g_touch_down) {
    g_touch_down = false;
    if (gesture) {
      // A flick glides on; a finger that stopped before lifting just stops.
      if (!pinch) ui::radar3d::dragEnd(now - last_ms < 100 ? vel_x : 0.0f);
      return;
    }
    if (!g_touch_long_fired && now - g_touch_down_ms < 800) {
      if (g_touch_samples > 0) {
        onTouchTap(static_cast<int>(g_touch_sum_x / g_touch_samples),
                   static_cast<int>(g_touch_sum_y / g_touch_samples));
      } else {
        onTouchTap(g_touch_first_x, g_touch_first_y);
      }
    }
  }
}
#else
void pollTouch() {
  lgfx::touch_point_t tp;
  const bool down = tft.getTouch(&tp) > 0;
  const unsigned long now = millis();
  if (down) {
    if (!g_touch_down) {
      g_touch_down = true;
      g_touch_long_fired = false;
      g_touch_down_ms = now;
      g_touch_first_x = tp.x;
      g_touch_first_y = tp.y;
      g_touch_sum_x = 0;
      g_touch_sum_y = 0;
      g_touch_samples = 0;
    } else {
      g_touch_sum_x += tp.x;
      g_touch_sum_y += tp.y;
      ++g_touch_samples;
    }
    if (!g_touch_long_fired && now - g_touch_down_ms >= config::kTouchLongPressMs) {
      g_touch_long_fired = true;
      ui::settingsMenuRun();
      // Back from the menu: repaint whatever screen is active.
      repaintScreen();
      g_touch_down = false;
    }
    return;
  }
  if (g_touch_down) {
    g_touch_down = false;
    if (!g_touch_long_fired && now - g_touch_down_ms < 800) {
      if (g_touch_samples > 0) {
        onTouchTap(static_cast<int>(g_touch_sum_x / g_touch_samples),
                   static_cast<int>(g_touch_sum_y / g_touch_samples));
      } else {
        onTouchTap(g_touch_first_x, g_touch_first_y);
      }
    }
  }
}
#endif

// ADS-B and weather fetches block for ~1-2 s each (HTTPS), so they run on their
// own task on core 0; the render loop keeps dead-reckoning on core 1.
void adsbFetchTask(void*) {
  unsigned long last_mem_log = 0;
  for (;;) {
    if (millis() - last_mem_log >= 60000) {
      last_mem_log = millis();
      Serial.printf("mem: free %u, largest %u, lowest %u, net stack left %u\n",
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()),
                    static_cast<unsigned>(ESP.getMinFreeHeap()),
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    // While a USB update runs, leave the network alone: the update needs the CPU
    // and the heap, and TLS handshakes take ~40 KB at a time.
    // Aircraft are polled every cycle while the radar or the clock (which needs to
    // know whether anything is in range) is up; behind the forecast or water screen
    // every 30 s is enough, which spares the network and the heap.
    static unsigned long last_adsb = 0;
    const bool aircraft_visible = g_screen == Screen::Radar || g_screen == Screen::Clock;
    const bool adsb_due = aircraft_visible || last_adsb == 0 || millis() - last_adsb >= 30000;
    if (services::serial_ota::active()) services::adsb::releaseConnection();
    if (WiFi.status() == WL_CONNECTED && !services::serial_ota::active()) {
      const bool route_wanted = g_screen == Screen::Radar && services::route::hasPending();
      auto fetchAircraft = [&]() {
        last_adsb = millis();
        services::adsb::fetchUpdate(services::location::lat(), services::location::lon(),
                                    ui::radar::fetchRadiusKm());
      };
      if (route_wanted) services::route::fetchPending();  // only the card shows routes
      if (adsb_due) fetchAircraft();
      services::weather::fetchIfDue(services::location::lat(), services::location::lon());
    }
    vTaskDelay(pdMS_TO_TICKS(config::kAdsbFetchIntervalMs));
  }
}

}  // namespace

void setup() {
  Serial.setRxBufferSize(4096);  // setup page and USB feed send lines in bursts
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("Plane Radar 3D v%s\n", config::kFirmwareVersion);

  services::display_prefs::init();
  bootButtonInit();
  displayInit();
  // The 76.8 KB frame sprite needs one contiguous block: take it before Wi-Fi.
  if (!ui::radar3d::init()) {
    Serial.println("Not enough RAM for the radar frame buffer");
  }
  services::sht30::init();
#if defined(BOARD_WS5)
  benchMemory();
#endif

#if !defined(BOARD_WS5)  // capacitive panel on the 5" board needs no calibration
  // Finger on the screen during power-up: (re)calibrate the touch panel.
  lgfx::touch_point_t tp;
  if (tft.getTouch(&tp) > 0) {
    displayRunTouchCalibration();
  }
#endif

  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  services::map::init();
  services::terrain::init();
  services::clock::init();
  services::weather::init();
  services::water::init();
  ui::radar::rangeInit();
  services::adsb::init();
  services::route::init();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  xTaskCreatePinnedToCore(adsbFetchTask, "adsb", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  if (services::serial_ota::active()) {
    // Firmware update over USB: only read the cable, as fast as possible.
    wifiLoop();
    services::serial_ota::tick();
    return;
  }
  handleBootButton();
  wifiLoop();
  services::sht30::tick();

  if (!online()) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }
    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }
    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else {
      pollTouch();
      // 5": the radar redraws as fast as it can (camera moves smoothly); the other
      // screens keep the slow cadence.
      const unsigned long interval =
          g_screen == Screen::Radar ? config::kRadarFrameIntervalMs : config::kRadarRedrawIntervalMs;
      if (millis() - g_last_redraw_ms >= interval) {
        g_last_redraw_ms = millis();
        updateScreen();
        pollTouch();  // a short tap may start while the frame is being drawn
      }
    }
  }

  delay(5);
}
