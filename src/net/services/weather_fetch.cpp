// Wi-Fi fetch of the forecast (system 1): Open-Meteo over HTTP (HTTPS as the
// fallback), published to the store in src/core/services/weather_service.cpp.
#include "services/weather_service.h"
#include "hardware/console.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include "services/tls_client.h"

#include <cmath>
#include <cstdio>

#include "config.h"
#include "services/adsb_client.h"

namespace services::weather {

namespace {

constexpr int kConnectTimeoutMs = 6000;
constexpr unsigned long kRequestTimeoutMs = 8000;

unsigned long s_last_try_ms = 0;
bool s_tried = false;
double s_lat = 1000.0;  // location of the data we hold (impossible until first fetch)
double s_lon = 1000.0;

void fail(const char* fmt, int value) {
  char text[48];
  snprintf(text, sizeof(text), fmt, value);
  setError(text);
  Console.printf("weather: %s (free heap %u, largest block %u)\n", text,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

/** One GET + parse. `secure` picks HTTPS (needs ~40 KB of heap for TLS) or plain HTTP. */
bool fetchOnce(const char* url, bool secure, Forecast* out) {
  JsonDocument filter;
  filter["utc_offset_seconds"] = true;
  filter["current"] = true;
  filter["daily"] = true;
  filter["reason"] = true;  // Open-Meteo's error text

  if (secure) services::adsb::releaseConnection();  // one TLS session at a time
  WiFiClient plain;
  services::tls::Client tls;
  if (secure) {
    tls.setInsecure();
    tls.setHandshakeTimeout(8);
  }
  HTTPClient http;
  http.useHTTP10(true);  // plain body, no chunked framing
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  http.setUserAgent("plane-radar-3d");
  const bool begun = secure ? http.begin(tls, url) : http.begin(plain, url);
  if (!begun) {
    fail("bağlantı kurulamadı", 0);
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      fail(secure ? "HTTPS hata %d" : "HTTP hata %d", code);
    } else {
      fail("sunucu yanıtı %d", code);
    }
    http.end();
    return false;
  }
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    fail(err == DeserializationError::NoMemory ? "bellek yetmedi" : "veri okunamadı (%d)",
         static_cast<int>(err.code()));
    return false;
  }
  if (!parseForecast(doc, out)) {
    if (doc["reason"].is<const char*>()) Console.printf("weather: %s\n", doc["reason"].as<const char*>());
    fail("beklenmeyen yanıt", 0);
    return false;
  }
  return true;
}

bool fetchNow(double lat, double lon) {
  // Plain HTTP first: the forecast is public, and skipping TLS leaves the ~40 KB
  // it needs to the aircraft feed (HTTPS on the same task, every 5 s). HTTPS is
  // the fallback for networks that block port 80.
  char url[512];  // the full request is ~420 characters
  const int len = snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
           "is_day,wind_speed_10m,wind_direction_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,sunrise,sunset,"
           "wind_speed_10m_max,wind_direction_10m_dominant,relative_humidity_2m_mean"
           "&forecast_days=%d&timezone=auto&timeformat=unixtime",
           lat, lon, kMaxDays);
  if (len <= 0 || len >= static_cast<int>(sizeof(url))) {
    fail("istek adresi çok uzun", 0);
    return false;
  }
  // "https://..." -> "http://..." by dropping the 's'.
  char plain_url[512];
  snprintf(plain_url, sizeof(plain_url), "http%s", url + 5);

  Forecast f;
  if (!fetchOnce(plain_url, false, &f) && !fetchOnce(url, true, &f)) {
    return false;
  }
  publish(f, true);
  s_lat = lat;
  s_lon = lon;
  Console.printf("weather: now %d C (%s), %u days\n", f.now.temp_c,
                conditionLabel(f.now.cond), static_cast<unsigned>(f.day_count));
  return true;
}

}  // namespace

void fetchIfDue(double lat, double lon) {
  if (!enabled() || WiFi.status() != WL_CONNECTED) {
    return;
  }
  const unsigned long now = millis();
  unsigned long fetched_ms = 0;
  const bool have = snapshot(nullptr, &fetched_ms);
  const bool moved = std::fabs(lat - s_lat) > 0.01 || std::fabs(lon - s_lon) > 0.01;
  const bool stale = !have || now - fetched_ms >= config::kWeatherRefreshMs;
  const bool retry_ok = !s_tried || now - s_last_try_ms >= config::kWeatherRetryMs;
  if (!(moved || stale) || !retry_ok) {
    return;
  }
  s_tried = true;
  s_last_try_ms = now;
  fetchNow(lat, lon);
}

}  // namespace services::weather
