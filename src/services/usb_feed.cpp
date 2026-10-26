#include "services/usb_feed.h"
#include "hardware/console.h"

#include <Arduino.h>
#include <sys/time.h>

#include <cstdlib>
#include <cstring>

#include "services/adsb_client.h"
#include "services/clock_service.h"
#include "services/route_service.h"

namespace services::usb_feed {

namespace {

constexpr unsigned long kActiveMs = 20000;

// Allocated on first use: in Wi-Fi mode the USB feed is never used and the RAM is
// needed by the HTTPS client.
services::adsb::Aircraft* s_staged = nullptr;
size_t s_count = 0;
unsigned long s_last_batch_ms = 0;
bool s_have_batch = false;

/** Next comma-separated field; returns nullptr at the end. */
char* nextField(char** cursor) {
  if (*cursor == nullptr) return nullptr;
  char* start = *cursor;
  char* comma = strchr(start, ',');
  if (comma != nullptr) {
    *comma = '\0';
    *cursor = comma + 1;
  } else {
    *cursor = nullptr;
  }
  return start;
}

}  // namespace

void begin() {
  s_count = 0;
  if (s_staged == nullptr) {
    s_staged = static_cast<services::adsb::Aircraft*>(
        malloc(sizeof(services::adsb::Aircraft) * services::adsb::kMaxAircraft));
  }
}

void add(const char* args) {
  if (s_staged == nullptr || s_count >= services::adsb::kMaxAircraft || args == nullptr) return;
  char buf[200];
  strlcpy(buf, args, sizeof(buf));
  char* cur = buf;
  const char* f[12] = {};
  for (int i = 0; i < 12; ++i) f[i] = nextField(&cur);
  if (f[0] == nullptr || f[1] == nullptr || f[0][0] == '\0' || f[1][0] == '\0') return;

  services::adsb::Aircraft& a = s_staged[s_count];
  memset(&a, 0, sizeof(a));
  a.lat = strtof(f[0], nullptr);
  a.lon = strtof(f[1], nullptr);
  a.track_deg = f[2] ? strtof(f[2], nullptr) : 0.0f;
  a.gs_knots = f[3] ? strtof(f[3], nullptr) : 0.0f;
  a.alt_ft = f[4] ? static_cast<int32_t>(strtol(f[4], nullptr, 10)) : 0;
  a.vrate_fpm = f[5] ? static_cast<int16_t>(strtol(f[5], nullptr, 10)) : 0;
  a.pos_age_ms = 0;
  if (f[6]) strlcpy(a.callsign, f[6], sizeof(a.callsign));
  if (f[7]) strlcpy(a.type, f[7], sizeof(a.type));
  // Optional route fields from the PC bridge: from_code,from_city,to_code,to_city
  // ("-" = looked up, no route known; missing = not looked up yet).
  if (f[8] != nullptr && a.callsign[0] != '\0') {
    if (strcmp(f[8], "-") == 0) {
      services::route::put(a.callsign, nullptr);
    } else {
      services::route::Route r{};
      strlcpy(r.from_code, f[8], sizeof(r.from_code));
      if (f[9]) services::route::foldAscii(f[9], r.from_city, sizeof(r.from_city));
      if (f[10]) strlcpy(r.to_code, f[10], sizeof(r.to_code));
      if (f[11]) services::route::foldAscii(f[11], r.to_city, sizeof(r.to_city));
      services::route::put(a.callsign, &r);
    }
  }
  ++s_count;
}

void end(const char* args) {
  const long long unix_time = args != nullptr ? atoll(args) : 0;
  if (unix_time > 1700000000LL) {
    services::clock::startSync();  // applies the time zone (NTP stays idle without Wi-Fi)
    timeval tv{static_cast<time_t>(unix_time), 0};
    settimeofday(&tv, nullptr);
  }
  if (s_staged != nullptr) services::adsb::publishExternal(s_staged, s_count);
  s_last_batch_ms = millis();
  s_have_batch = true;
  Console.printf("PR:AOK {\"n\":%u}\n", static_cast<unsigned>(s_count));
}

bool active() { return s_have_batch && millis() - s_last_batch_ms < kActiveMs; }

}  // namespace services::usb_feed
