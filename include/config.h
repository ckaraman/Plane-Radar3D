#pragma once

#include <cstdint>

#include <WiFi.h>
#include <driver/gpio.h>

namespace config {

/** Shown on the web setup page; the build copies it into the update manifest.
 *  Bump it for every release you publish. Keep this declaration on a single line. */
constexpr char kFirmwareVersion[] = "5in-0.18.2";

// --- USB setup page address shown on the setup screen ---
// CI writes include/setup_url.h with the GitHub Pages address of the repository
// ("owner.github.io/repo"). Without it the address below is used: the existing
// plane-radar-clock setup page speaks the same USB protocol as this firmware.
#if defined(__has_include)
#if __has_include("setup_url.h")
#include "setup_url.h"
#endif
#endif
#ifdef SETUP_PAGE_URL
constexpr char kSetupPageUrl[] = SETUP_PAGE_URL;
#else
constexpr char kSetupPageUrl[] = "ckaraman.github.io/plane-radar-3d";
#endif

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids reconnect storms on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;
/** ESP32-WROOM-32U with an external antenna: full transmit power is fine.
 *  (The C3 Super Mini build had to drop to 8.5 dBm because of its chip antenna.) */
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

#if defined(BOARD_WS5B)  // ESP32-S3-Touch-LCD-5B: 1024x600 panel, otherwise identical
constexpr int kDisplayWidth = 1024;
constexpr int kDisplayHeight = 600;
#else
constexpr int kDisplayWidth = 800;
constexpr int kDisplayHeight = 480;
#endif
/** Default backlight (0-255). The CH422G switches it on/off only. */
constexpr uint8_t kDisplayBrightness = 255;

// Shared I2C bus: GT911 touch, CH422G expander and the SHT30 (on the board's I2C header).
constexpr int kI2cPort = 1;  // LovyanGFX I2C port used for all three
constexpr int kI2cPinSda = 8;
constexpr int kI2cPinScl = 9;
constexpr int kTouchPinInt = 4;
constexpr uint8_t kSht30Address = 0x44;
#else
// --- BOOT button (ESP32 DevKit, GPIO 0, active LOW) ---
constexpr bool kHasBootButton = true;
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

// --- Display: 4.0" ST7796S 480x320 on VSPI, XPT2046 resistive touch on the same bus ---
// Wiring table: README.md. The LCD's own SDO/MISO pin stays unconnected; only
// the touch controller (T_DO) drives MISO.
constexpr int kSpiPinSclk = 18;   // LCD SCK  + touch T_CLK
constexpr int kSpiPinMosi = 23;   // LCD SDI  + touch T_DIN
constexpr int kSpiPinMiso = 19;   // touch T_DO
constexpr int kDisplayPinCs = 5;
constexpr int kDisplayPinDc = 21;
constexpr int kDisplayPinRst = 22;
constexpr int kDisplayPinBacklight = 4;  // module LED pin, PWM dimmed
constexpr int kTouchPinCs = 15;
constexpr int kTouchPinIrq = 34;  // input-only pin, fine for T_IRQ

constexpr int kDisplayWidth = 480;   // landscape
constexpr int kDisplayHeight = 320;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
constexpr uint32_t kDisplaySpiReadHz = 16000000;
constexpr uint32_t kTouchSpiHz = 2500000;
/** Default backlight (0-255). */
constexpr uint8_t kDisplayBrightness = 255;  // full backlight

// --- SHT30 temperature / humidity (I2C) ---
constexpr int kI2cPinSda = 25;
constexpr int kI2cPinScl = 26;
constexpr uint8_t kSht30Address = 0x44;  // 0x45 when the ADDR pin is tied to 3.3 V
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
