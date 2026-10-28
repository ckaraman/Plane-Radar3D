#include "services/weather_service.h"
#include "hardware/console.h"

#include <Arduino.h>
#include <Preferences.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>

// The forecast both systems show. Who fills it: the Wi-Fi fetch
// (src/net/services/weather_fetch.cpp, system 1) or a "PR:W" line from the PC.

namespace services::weather {

namespace {

constexpr char kPrefsNamespace[] = "weather";
constexpr char kKeyOn[] = "on";

bool s_enabled = true;
Forecast s_forecast;
bool s_have = false;
unsigned long s_fetched_ms = 0;
SemaphoreHandle_t s_mutex = nullptr;
char s_error[48] = "";  // why the last attempt failed (shown on the forecast screen)

void lock() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
}

void unlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

bool checkboxChecked(const char* value) {
  return value != nullptr && (value[0] == 'T' || value[0] == 't') && value[1] == '\0';
}

}  // namespace

void init() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    s_enabled = prefs.getBool(kKeyOn, true);
    prefs.end();
  }
}

bool enabled() { return s_enabled; }

void saveFromPortal(const char* enabled_checkbox) {
  s_enabled = checkboxChecked(enabled_checkbox);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kKeyOn, s_enabled);
    prefs.end();
  }
  Console.printf("Weather: %s\n", s_enabled ? "on" : "off");
}

void clear() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  s_enabled = true;
}

bool snapshot(Forecast* out, unsigned long* fetched_ms) {
  lock();
  const bool have = s_have;
  if (have) {
    if (out != nullptr) {
      *out = s_forecast;
    }
    if (fetched_ms != nullptr) {
      *fetched_ms = s_fetched_ms;
    }
  }
  unlock();
  return have;
}

bool lastError(char* out, size_t out_len) {
  lock();
  const bool has = s_error[0] != '\0';
  if (has && out != nullptr && out_len > 0) {
    strncpy(out, s_error, out_len - 1);
    out[out_len - 1] = '\0';
  }
  unlock();
  return has;
}

void publish(const Forecast& f, bool clear_error) {
  lock();
  s_forecast = f;
  s_have = true;
  s_fetched_ms = millis();
  if (s_fetched_ms == 0) s_fetched_ms = 1;
  if (clear_error) s_error[0] = '\0';
  unlock();
}

void setError(const char* text) {
  lock();
  snprintf(s_error, sizeof(s_error), "%s", text);
  unlock();
}

void handleBridge(const char* args) {
  Forecast f;
  if (!parseBridgeLine(args, &f)) {
    Console.println("PR:ERR {\"msg\":\"bad_weather\"}");
    return;
  }
  publish(f, false);
  Console.printf("PR:WOK {\"days\":%u}\n", static_cast<unsigned>(f.day_count));
}

}  // namespace services::weather
