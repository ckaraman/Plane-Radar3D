/**
 * Plane Radar 3D — ESP32-WROOM-32U + 4.0" ST7796S (480x320, XPT2046 touch) + SHT30.
 *
 * Same services as the round plane-radar-clock build (USB setup page, adsb.fi,
 * NTP clock, Open-Meteo weather, map layer); the radar itself is a perspective
 * view with touch controls.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/clock_service.h"
#include "services/display_prefs.h"
#include "services/map_store.h"
#include "services/radar_location.h"
#include "services/route_service.h"
#include "services/sht30.h"
#include "services/usb_feed.h"
#include "services/weather_service.h"
#include "services/wifi_setup.h"
#include "ui/clock_display.h"
#include "ui/radar3d.h"
#include "ui/radar_range.h"
#include "ui/settings_menu.h"
#include "ui/status_screens.h"
#include "ui/weather_display.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_redraw_ms = 0;

enum class Screen { Radar, Clock, Weather };
Screen g_screen = Screen::Radar;
unsigned long g_last_aircraft_ms = 0;
unsigned long g_main_since_ms = 0;
unsigned long g_weather_started_ms = 0;

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
  g_main_since_ms = millis();
  ui::radar3d::drawFrame();
  g_radar_visible = true;
}

size_t aircraftInRange() {
  static services::adsb::Aircraft buf[services::adsb::kMaxAircraft];
  unsigned long fetched = 0;
  const size_t n = services::adsb::snapshotAircraft(buf, services::adsb::kMaxAircraft, &fetched);
  return n;  // fetch radius is already the view radius (+15 %)
}

// Runs at the redraw cadence: decide radar / clock / weather, then draw.
void updateScreen() {
  const unsigned long now = millis();
  const size_t aircraft = aircraftInRange();
  const bool have_data = services::adsb::lastUpdateMs() != 0;
  if (aircraft > 0 || !have_data) {
    g_last_aircraft_ms = now;
  }

  if (g_screen == Screen::Weather) {
    if (now - g_weather_started_ms < config::kWeatherShowMs) {
      ui::weatherDisplayTick();
      return;
    }
    g_main_since_ms = now;
  } else if (services::weather::enabled() && now - g_main_since_ms >= config::kWeatherEveryMs &&
             services::weather::snapshot(nullptr, nullptr)) {
    g_screen = Screen::Weather;
    g_weather_started_ms = now;
    ui::weatherDisplayEnter();
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

/** Clock / weather: any tap (or BOOT tap) brings the radar back for a while. */
void wakeRadar() {
  g_last_aircraft_ms = millis();
  if (g_screen == Screen::Weather) {
    g_weather_started_ms = millis() - config::kWeatherShowMs;
  }
  g_main_since_ms = millis();
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

void onTouchTap(int x, int y) {
  if (g_screen != Screen::Radar) {
    wakeRadar();
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
      if (g_screen == Screen::Clock) ui::clockDisplayEnter();
      if (g_screen == Screen::Weather) ui::weatherDisplayEnter();
      g_last_redraw_ms = 0;
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

// ADS-B and weather fetches block for ~1-2 s each (HTTPS), so they run on their
// own task on core 0; the render loop keeps dead-reckoning on core 1.
void adsbFetchTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      services::adsb::fetchUpdate(services::location::lat(), services::location::lon(),
                                  ui::radar::fetchRadiusKm());
      services::route::fetchPending();
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
  services::clock::init();
  services::weather::init();
  ui::radar::rangeInit();
  services::adsb::init();
  services::route::init();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  xTaskCreatePinnedToCore(adsbFetchTask, "adsb", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
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
