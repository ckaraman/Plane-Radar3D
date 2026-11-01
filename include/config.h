#pragma once

#include <cstdint>

#include <WiFi.h>
#include <driver/gpio.h>

namespace config {

/** Shown on the web setup page; the build copies it into the update manifest.
 *  Bump it for every release you publish. Keep this declaration on a single line. */
#if defined(WITH_DECK)
constexpr char kFirmwareVersion[] = "5in-0.50.2";  // system 2: no Wi-Fi, data from the PC
#else  // system 1: Wi-Fi radar without the stream deck (env:radar)
constexpr char kFirmwareVersion[] = "radar-1.1.0";
#endif

// --- USB setup page address shown on the setup screen ---
// CI writes include/setup_url.h with the GitHub Pages address of the repository
// ("owner.github.io/repo"). Without it (local builds) this repository's page is used.
#if defined(__has_include)
#if __has_include("setup_url.h")
#include "setup_url.h"
#endif
#endif
#ifndef SETUP_PAGE_URL
#define SETUP_PAGE_URL "ckaraman.github.io/Plane-Radar3D"
#endif
#if defined(WITH_DECK)
constexpr char kSetupPageUrl[] = SETUP_PAGE_URL;
#else  // system 1 has its own page
constexpr char kSetupPageUrl[] = SETUP_PAGE_URL "/radar";
#endif

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;
/** Full transmit power. */
constexpr wifi_power_t kWifiTxPower = WIFI_POWER_19_5dBm;

#if defined(BOARD_WS5)
// =====================================================================================
// Waveshare ESP32-S3-Touch-LCD-5 (800x480 RGB panel, GT911 capacitive touch, CH422G
// I/O expander for backlight / resets, 8 MB PSRAM). Pins from the board's schematic as
// used by Waveshare's demos and ESPHome's "ESP32-S3-TOUCH-LCD-4.3" model (same family).
// =====================================================================================
constexpr bool kHasBootButton = false;  // GPIO 0 is an RGB data line on this board
constexpr gpio_num_t kBootPin = GPIO_NUM_NC;
constexpr unsigned long kBootResetHoldMs = 3000UL;
constexpr unsigned long kBootTapMinMs = 40UL;

constexpr int kDisplayWidth = 800;
constexpr int kDisplayHeight = 480;
/** Default backlight (0-255). The CH422G switches it on/off only. */
constexpr uint8_t kDisplayBrightness = 255;

// Shared I2C bus: GT911 touch, CH422G expander and the SHT30 (on the board's I2C header).
constexpr int kI2cPort = 1;  // LovyanGFX I2C port used for all three
constexpr int kI2cPinSda = 8;
constexpr int kI2cPinScl = 9;
constexpr int kTouchPinInt = 4;
constexpr uint8_t kSht30Address = 0x44;
#endif
constexpr unsigned long kSht30IntervalMs = 10000;
/** Added to every temperature reading. Use a negative value if the sensor sits
 *  close to the display or the ESP32 and reads warm. */
constexpr float kSht30TempOffsetC = 0.0f;

// --- Radar center defaults (overridden from the setup page) ---
constexpr double kDefaultRadarLat = 41.0082;   // Istanbul
constexpr double kDefaultRadarLon = 28.9784;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Redraw cadence of the 3D view. Aircraft are dead-reckoned between fetches. */
constexpr unsigned long kRadarFrameIntervalMs = 16;    // 5": radar frame pacing (as fast as it draws)
constexpr unsigned long kRadarRedrawIntervalMs = 200;  // leaves time between frames to read the touch panel
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- Idle clock (shown when no aircraft are in range) ---
/** POSIX TZ string used until changed on the setup page. Istanbul = UTC+3, no DST. */
constexpr char kDefaultTimezone[] = "<+03>-3";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.google.com";
/** Radar must stay empty this long before the clock takes over. */
constexpr unsigned long kClockIdleDelayMs = 15000;
/** true = Turkish day/month names (ASCII), false = English. */
constexpr bool kClockTurkishDate = true;

// --- Weather screen (Open-Meteo) ---
constexpr unsigned long kWeatherRefreshMs = 600000;
constexpr unsigned long kWeatherRetryMs = 60000;

// --- Touch ---
/** Hold anywhere this long to open the settings menu. */
constexpr unsigned long kTouchLongPressMs = 1500;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
