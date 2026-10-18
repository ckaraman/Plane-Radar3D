#pragma once

#include <ArduinoJson.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace services::weather {

enum class Condition : uint8_t {
  Clear,
  PartlyCloudy,
  Cloudy,
  Fog,
  Drizzle,
  Rain,
  Showers,
  Snow,
  Thunder,
};

struct Slot {
  bool valid = false;
  int16_t temp_c = 0;  // whole degrees Celsius
  Condition cond = Condition::Clear;
  bool day = true;
};

/** One day of the multi-day forecast. */
struct Day {
  bool valid = false;
  uint8_t wday = 0;    // 0 = Sunday (local calendar day of the forecast location)
  uint8_t mday = 1;    // day of the month
  Condition cond = Condition::Clear;
  int16_t max_c = 0;
  int16_t min_c = 0;
  int16_t pop = -1;    // max precipitation probability %, -1 = unknown
};

constexpr int kMaxDays = 5;

struct Forecast {
  Slot now;
  // Extra "now" details for the forecast screen.
  int16_t feels_c = 0;
  int16_t humidity = -1;   // %, -1 = unknown
  int16_t wind_kmh = -1;
  int16_t wind_dir = -1;   // degrees the wind comes from
  int16_t sunrise_min = -1;  // local minutes after midnight, today
  int16_t sunset_min = -1;
  Day days[kMaxDays];
  uint8_t day_count = 0;
};

/** WMO weather interpretation code (as returned by Open-Meteo) -> icon class. */
inline Condition conditionFromWmo(int code) {
  switch (code) {
    case 0:
    case 1:
      return Condition::Clear;
    case 2:
      return Condition::PartlyCloudy;
    case 3:
      return Condition::Cloudy;
    case 45:
    case 48:
      return Condition::Fog;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
      return Condition::Drizzle;
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
      return Condition::Rain;
    case 80:
    case 81:
    case 82:
      return Condition::Showers;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
      return Condition::Snow;
    case 95:
    case 96:
    case 99:
      return Condition::Thunder;
    default:
      return Condition::Cloudy;
  }
}

/** Short ASCII label (the bitmap font has no Turkish letters). */
inline const char* conditionLabel(Condition c) {
  switch (c) {
    case Condition::Clear: return "ACIK";
    case Condition::PartlyCloudy: return "P.BULUTLU";
    case Condition::Cloudy: return "BULUTLU";
    case Condition::Fog: return "SISLI";
    case Condition::Drizzle: return "CISELEME";
    case Condition::Rain: return "YAGMURLU";
    case Condition::Showers: return "SAGANAK";
    case Condition::Snow: return "KARLI";
    case Condition::Thunder: return "FIRTINA";
  }
  return "";
}

namespace date_util {

inline void localDate(long unix_s, long utc_offset_s, uint8_t* wday, uint8_t* mday) {
  // Days since 1970-01-01 (a Thursday) in the location's local time.
  long t = unix_s + utc_offset_s;
  long days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
  *wday = static_cast<uint8_t>(((days % 7) + 11) % 7);  // 1970-01-01 -> 4 (Thursday)
  // Civil date from days (Howard Hinnant's algorithm).
  days += 719468;
  const long era = (days >= 0 ? days : days - 146096) / 146097;
  const long doe = days - era * 146097;
  const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long mp = (5 * doy + 2) / 153;
  *mday = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
}

inline int16_t localMinutes(long unix_s, long utc_offset_s) {
  long t = (unix_s + utc_offset_s) % 86400;
  if (t < 0) t += 86400;
  return static_cast<int16_t>(t / 60);
}

}  // namespace date_util

/**
 * Open-Meteo request this parser expects (see weather_service.cpp):
 *   current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,
 *           wind_speed_10m,wind_direction_10m
 *   daily=weather_code,temperature_2m_max,temperature_2m_min,
 *         precipitation_probability_max,sunrise,sunset
 *   forecast_days=5, timezone=auto, timeformat=unixtime
 * Timestamps are UTC epoch seconds; utc_offset_seconds gives the local calendar.
 */
inline bool parseForecast(const JsonDocument& doc, Forecast* out) {
  JsonVariantConst cur = doc["current"];
  if (!cur["temperature_2m"].is<float>() || !cur["weather_code"].is<int>()) {
    return false;
  }
  const long off = doc["utc_offset_seconds"] | 0L;
  Forecast f;
  f.now.valid = true;
  f.now.temp_c = static_cast<int16_t>(lroundf(cur["temperature_2m"].as<float>()));
  f.now.cond = conditionFromWmo(cur["weather_code"].as<int>());
  f.now.day = (cur["is_day"] | 1) != 0;
  f.feels_c = static_cast<int16_t>(
      lroundf(cur["apparent_temperature"] | cur["temperature_2m"].as<float>()));
  if (cur["relative_humidity_2m"].is<float>())
    f.humidity = static_cast<int16_t>(lroundf(cur["relative_humidity_2m"].as<float>()));
  if (cur["wind_speed_10m"].is<float>())
    f.wind_kmh = static_cast<int16_t>(lroundf(cur["wind_speed_10m"].as<float>()));
  if (cur["wind_direction_10m"].is<float>())
    f.wind_dir = static_cast<int16_t>(lroundf(cur["wind_direction_10m"].as<float>()));

  JsonVariantConst d = doc["daily"];
  JsonArrayConst times = d["time"];
  JsonArrayConst codes = d["weather_code"];
  JsonArrayConst tmax = d["temperature_2m_max"];
  JsonArrayConst tmin = d["temperature_2m_min"];
  JsonArrayConst pop = d["precipitation_probability_max"];
  JsonArrayConst rise = d["sunrise"];
  JsonArrayConst set = d["sunset"];
  const size_t n = times.size();
  for (size_t i = 0; i < n && f.day_count < kMaxDays; ++i) {
    if (codes[i].isNull() || tmax[i].isNull() || tmin[i].isNull()) continue;
    Day& day = f.days[f.day_count++];
    day.valid = true;
    // Daily times are local midnight; +12 h keeps the date safe from DST shifts.
    date_util::localDate(times[i].as<long>() + 43200, off, &day.wday, &day.mday);
    day.cond = conditionFromWmo(codes[i].as<int>());
    day.max_c = static_cast<int16_t>(lroundf(tmax[i].as<float>()));
    day.min_c = static_cast<int16_t>(lroundf(tmin[i].as<float>()));
    day.pop = pop[i].isNull() ? -1 : static_cast<int16_t>(pop[i].as<int>());
  }
  if (rise.size() > 0 && !rise[0].isNull()) f.sunrise_min = date_util::localMinutes(rise[0].as<long>(), off);
  if (set.size() > 0 && !set[0].isNull()) f.sunset_min = date_util::localMinutes(set[0].as<long>(), off);
  *out = f;
  return true;
}

/**
 * Same data from the PC bridge (USB mode), one line, space separated:
 *   <utc_off> <temp> <rh> <feels> <code> <is_day> <wind_kmh> <wind_dir> <sunrise> <sunset> <n>
 *   then n groups "<unix>,<code>,<max>,<min>,<pop>"   (pop -1 = unknown)
 * sunrise/sunset are unix seconds (0 = unknown). Returns false on malformed input.
 */
inline bool parseBridgeLine(const char* s, Forecast* out) {
  char* p = const_cast<char*>(s);
  auto num = [&](double* v) {
    while (*p == ' ') ++p;
    char* e = nullptr;
    *v = strtod(p, &e);
    if (e == p) return false;
    p = e;
    return true;
  };
  double off, t, rh, feels, code, isday, ws, wd, rise, set, n;
  if (!num(&off) || !num(&t) || !num(&rh) || !num(&feels) || !num(&code) || !num(&isday) ||
      !num(&ws) || !num(&wd) || !num(&rise) || !num(&set) || !num(&n)) {
    return false;
  }
  const long o = static_cast<long>(off);
  Forecast f;
  f.now.valid = true;
  f.now.temp_c = static_cast<int16_t>(lround(t));
  f.now.cond = conditionFromWmo(static_cast<int>(code));
  f.now.day = isday != 0;
  f.feels_c = static_cast<int16_t>(lround(feels));
  f.humidity = rh < 0 ? -1 : static_cast<int16_t>(lround(rh));
  f.wind_kmh = ws < 0 ? -1 : static_cast<int16_t>(lround(ws));
  f.wind_dir = wd < 0 ? -1 : static_cast<int16_t>(lround(wd));
  if (rise > 0) f.sunrise_min = date_util::localMinutes(static_cast<long>(rise), o);
  if (set > 0) f.sunset_min = date_util::localMinutes(static_cast<long>(set), o);
  for (int i = 0; i < static_cast<int>(n) && f.day_count < kMaxDays; ++i) {
    double v[5];
    for (int k = 0; k < 5; ++k) {
      while (*p == ',' || *p == ' ') ++p;
      if (!num(&v[k])) return false;
    }
    Day& day = f.days[f.day_count++];
    day.valid = true;
    date_util::localDate(static_cast<long>(v[0]) + 43200, o, &day.wday, &day.mday);
    day.cond = conditionFromWmo(static_cast<int>(v[1]));
    day.max_c = static_cast<int16_t>(lround(v[2]));
    day.min_c = static_cast<int16_t>(lround(v[3]));
    day.pop = static_cast<int16_t>(lround(v[4]));
  }
  *out = f;
  return true;
}

}  // namespace services::weather
