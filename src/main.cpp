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

// Frame hand-over for TLS handshakes. The ESP32's heap cannot hold the 76.8 KB frame
// and a TLS handshake (~50-60 KB at its peak) at the same time, so for the second
// or two a new HTTPS session takes, the render loop parks and frees the frame (the
// panel keeps showing the last picture). The connection is then kept open, so this
// only happens at start-up, after a dropped link, or for a route lookup.
volatile bool g_frame_req = false;     // network task: please free the frame
volatile bool g_frame_parked = false;  // render loop: frame freed, waiting
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
  if (ui::waterButtonHit(x, y)) {
    openWater();
    return;
  }
  if (ui::radar3d::handleTap(x, y)) {
    g_last_redraw_ms = 0;
  }
}

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

/**
 * Run `fn` (network task) while the render loop has the frame freed. Before handing
 * the frame back, make sure it fits again; if the open TLS connection took its
 * place, close it (the screen matters more than one fetch).
 */
template <typename Fn>
void withFrameReleased(Fn fn) {
  if (ui::radar3d::frame() == nullptr && !g_frame_parked) {
    fn();  // no frame to free (setup screens)
    return;
  }
  g_frame_req = true;
  const unsigned long t0 = millis();
  while (!g_frame_parked && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(5));
  if (!g_frame_parked) {  // render loop busy (e.g. USB update): try without
    g_frame_req = false;
    fn();
    return;
  }
  fn();
  if (heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < ui::radar3d::kFrameBytes + 64) {
    Serial.printf("frame would not fit (largest block %u): closing TLS\n",
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    services::adsb::releaseConnection();
  }
  g_frame_req = false;
  const unsigned long t1 = millis();
  while (g_frame_parked && millis() - t1 < 3000) vTaskDelay(pdMS_TO_TICKS(5));
}

// ADS-B and weather fetches block for ~1-2 s each (HTTPS), so they run on their
// own task on core 0; the render loop keeps dead-reckoning on core 1.
void adsbFetchTask(void*) {
  for (;;) {
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
      if (route_wanted || (adsb_due && services::adsb::needsHandshake())) {
        withFrameReleased([&]() {
          if (route_wanted) services::route::fetchPending();  // only the card shows routes
          fetchAircraft();  // (re)opens the kept TLS connection while the heap is free
        });
      } else if (adsb_due) {
        fetchAircraft();
      }
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

  // Finger on the screen during power-up: (re)calibrate the touch panel.
  lgfx::touch_point_t tp;
  if (tft.getTouch(&tp) > 0) {
    displayRunTouchCalibration();
  }

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
  if (g_frame_req) {
    ui::radar3d::releaseFrame();
    g_frame_parked = true;
    const unsigned long t0 = millis();
    while (g_frame_req && millis() - t0 < 20000) delay(5);
    // The network task checked that the frame fits again before letting go.
    for (int i = 0; i < 20 && !ui::radar3d::reacquireFrame(); ++i) delay(50);
    g_frame_parked = false;
    g_last_redraw_ms = 0;
    if (g_radar_visible) repaintScreen();
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
      if (millis() - g_last_redraw_ms >= config::kRadarRedrawIntervalMs) {
        g_last_redraw_ms = millis();
        updateScreen();
        pollTouch();  // a short tap may start while the frame is being drawn
      }
    }
  }

  delay(5);
}
