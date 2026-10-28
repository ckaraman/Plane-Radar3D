#include "services/adsb_client.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cmath>

// The aircraft list both systems draw from. Who fills it: the Wi-Fi fetch
// (src/net/services/adsb_fetch.cpp, system 1) or the USB feed (usb_feed.cpp).

namespace services::adsb {

namespace {

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
SemaphoreHandle_t s_mutex = nullptr;

/** Publish parsed aircraft to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  for (size_t i = 0; i < count; ++i) {
    s_aircraft[i] = src[i];
  }
  s_aircraft_count = count;
  s_last_update_ms = millis();  // base time for dead-reckoning
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

}  // namespace

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void publishExternal(const Aircraft* src, size_t count) {
  publish(src, count < kMaxAircraft ? count : kMaxAircraft);
}

size_t aircraftCount() { return s_aircraft_count; }

size_t countWithin(double lat0, double lon0, float radius_km) {
  const double kx = 111.0 * cos(lat0 * 0.01745329252), r2 = static_cast<double>(radius_km) * radius_km;
  if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
  size_t in = 0;
  for (size_t i = 0; i < s_aircraft_count; ++i) {
    const double dx = (s_aircraft[i].lon - lon0) * kx, dy = (s_aircraft[i].lat - lat0) * 111.0;
    if (dx * dx + dy * dy <= r2) ++in;
  }
  if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
  return in;
}

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

}  // namespace services::adsb
