#include "services/weather_service.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/adsb_client.h"

namespace services::weather {

namespace {

constexpr char kPrefsNamespace[] = "weather";
constexpr char kKeyOn[] = "on";

constexpr int kConnectTimeoutMs = 6000;
constexpr unsigned long kRequestTimeoutMs = 8000;

bool s_enabled = true;
Forecast s_forecast;
bool s_have = false;
unsigned long s_fetched_ms = 0;
unsigned long s_last_try_ms = 0;
bool s_tried = false;
double s_lat = 1000.0;  // location of the data we hold (impossible until first fetch)
double s_lon = 1000.0;
SemaphoreHandle_t s_mutex = nullptr;

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

char s_error[48] = "";  // why the last attempt failed (shown on the forecast screen)

void setError(const char* fmt, int value) {
  lock();
  snprintf(s_error, sizeof(s_error), fmt, value);
  unlock();
  Serial.printf("weather: %s (free heap %u, largest block %u)\n", s_error,
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
  WiFiClientSecure tls;
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
    setError("bağlantı kurulamadı", 0);
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      setError(secure ? "HTTPS hata %d" : "HTTP hata %d", code);
    } else {
      setError("sunucu yanıtı %d", code);
    }
    http.end();
    return false;
  }
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    setError(err == DeserializationError::NoMemory ? "bellek yetmedi" : "veri okunamadı (%d)",
             static_cast<int>(err.code()));
    return false;
  }
  if (!parseForecast(doc, out)) {
    if (doc["reason"].is<const char*>()) Serial.printf("weather: %s\n", doc["reason"].as<const char*>());
    setError("beklenmeyen yanıt", 0);
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
    setError("istek adresi çok uzun", 0);
    return false;
  }
  // "https://..." -> "http://..." by dropping the 's'.
  char plain_url[512];
  snprintf(plain_url, sizeof(plain_url), "http%s", url + 5);

  Forecast f;
  if (!fetchOnce(plain_url, false, &f) && !fetchOnce(url, true, &f)) {
    return false;
  }
  lock();
  s_forecast = f;
  s_have = true;
  s_fetched_ms = millis();
  if (s_fetched_ms == 0) s_fetched_ms = 1;
  s_lat = lat;
  s_lon = lon;
  s_error[0] = '\0';
  unlock();
  Serial.printf("weather: now %d C (%s), %u days\n", f.now.temp_c,
                conditionLabel(f.now.cond), static_cast<unsigned>(f.day_count));
  return true;
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
  Serial.printf("Weather: %s\n", s_enabled ? "on" : "off");
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

void handleBridge(const char* args) {
  Forecast f;
  if (!parseBridgeLine(args, &f)) {
    Serial.println("PR:ERR {\"msg\":\"bad_weather\"}");
    return;
  }
  lock();
  s_forecast = f;
  s_have = true;
  s_fetched_ms = millis();
  if (s_fetched_ms == 0) s_fetched_ms = 1;
  unlock();
  Serial.printf("PR:WOK {\"days\":%u}\n", static_cast<unsigned>(f.day_count));
}

void fetchIfDue(double lat, double lon) {
  if (!s_enabled || WiFi.status() != WL_CONNECTED) {
    return;
  }
  const unsigned long now = millis();
  lock();
  const bool moved = std::fabs(lat - s_lat) > 0.01 || std::fabs(lon - s_lon) > 0.01;
  const bool stale = !s_have || now - s_fetched_ms >= config::kWeatherRefreshMs;
  const bool retry_ok = !s_tried || now - s_last_try_ms >= config::kWeatherRetryMs;
  unlock();
  if (!(moved || stale) || !retry_ok) {
    return;
  }
  s_tried = true;
  s_last_try_ms = now;
  fetchNow(lat, lon);
}

}  // namespace services::weather
