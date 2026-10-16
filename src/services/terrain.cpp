#include "services/terrain.h"

#include <Arduino.h>
#include <Preferences.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "services/radar_location.h"

namespace services::terrain {

namespace {

constexpr char kNs[] = "terrain";
constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.01745329252f;

struct Header {
  uint8_t version;
  uint8_t n;
  uint16_t reserved;
  float half_km;
  double lat;
  double lon;
};

// Active grid
bool s_have = false;
Header s_hdr{};
int16_t s_h[kMaxN * kMaxN];
uint32_t s_gen = 1;

// Upload staging
bool s_staging = false;
Header s_stage_hdr{};
int16_t s_stage[kMaxN * kMaxN];
uint64_t s_rows_mask = 0;

void reply(const char* tag, const char* body) {
  char line[64];
  const int n = snprintf(line, sizeof(line), "PR:%s %s\n", tag, body);
  if (n > 0) Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
}

void replyRow(int row) {
  char body[24];
  snprintf(body, sizeof(body), "{\"row\":%d}", row);
  reply("ELEVACK", body);
}

void replyError(const char* code) {
  char body[48];
  snprintf(body, sizeof(body), "{\"msg\":\"%s\"}", code);
  reply("ERR", body);
}

void save() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putBytes("hdr", &s_hdr, sizeof(s_hdr));
  p.putBytes("grid", s_h, sizeof(int16_t) * s_hdr.n * s_hdr.n);
  p.end();
}

}  // namespace

void init() {
  Preferences p;
  if (!p.begin(kNs, true)) return;
  Header h{};
  if (p.getBytes("hdr", &h, sizeof(h)) == sizeof(h) && h.version == 1 && h.n >= 2 &&
      h.n <= kMaxN && h.half_km > 0.5f) {
    const size_t want = sizeof(int16_t) * h.n * h.n;
    if (p.getBytes("grid", s_h, want) == want) {
      s_hdr = h;
      s_have = true;
      ++s_gen;
    }
  }
  p.end();
}

bool available() { return s_have; }
uint32_t generation() { return s_gen; }

float heightM(float x_km, float y_km) {
  if (!s_have) return 0.0f;
  // Offset of the grid centre from the radar centre.
  const float lat0 = static_cast<float>(services::location::lat());
  const float lon0 = static_cast<float>(services::location::lon());
  const float gx = static_cast<float>(s_hdr.lon - lon0) * kKmPerDeg * cosf(lat0 * kDegToRad);
  const float gy = static_cast<float>(s_hdr.lat - lat0) * kKmPerDeg;
  const int n = s_hdr.n;
  const float step = 2.0f * s_hdr.half_km / static_cast<float>(n - 1);
  const float fx = (x_km - gx + s_hdr.half_km) / step;
  const float fy = (y_km - gy + s_hdr.half_km) / step;
  if (fx < 0.0f || fy < 0.0f || fx > n - 1 || fy > n - 1) return 0.0f;
  const int ix = std::min(static_cast<int>(fx), n - 2);
  const int iy = std::min(static_cast<int>(fy), n - 2);
  const float tx = fx - ix, ty = fy - iy;
  const float h00 = s_h[iy * n + ix], h10 = s_h[iy * n + ix + 1];
  const float h01 = s_h[(iy + 1) * n + ix], h11 = s_h[(iy + 1) * n + ix + 1];
  const float h = (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
  return h > 0.0f ? h : 0.0f;  // sea and below-sea-level land drawn at sea level
}

void handleBegin(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    replyError("bad_json");
    return;
  }
  const int n = doc["n"] | 0;
  const float half = doc["half"] | 0.0f;
  if (n < 2 || n > kMaxN || half < 0.5f || half > 200.0f || !doc["lat"].is<double>() ||
      !doc["lon"].is<double>()) {
    replyError("bad_elev");
    return;
  }
  s_stage_hdr = Header{1, static_cast<uint8_t>(n), 0, half, doc["lat"].as<double>(),
                       doc["lon"].as<double>()};
  memset(s_stage, 0, sizeof(s_stage));
  s_rows_mask = 0;
  s_staging = true;
  replyRow(-1);
}

void handleRow(char* args) {
  if (!s_staging) {
    replyError("elev_not_started");
    return;
  }
  char* cur = nullptr;
  const long row = strtol(args, &cur, 10);
  const int n = s_stage_hdr.n;
  if (row < 0 || row >= n || cur == nullptr) {
    replyError("bad_elev");
    return;
  }
  for (int i = 0; i < n; ++i) {
    while (*cur == ' ' || *cur == ',') ++cur;
    if (*cur == '\0') {
      replyError("bad_elev");
      return;
    }
    char* next = nullptr;
    long v = strtol(cur, &next, 10);
    if (next == cur) {
      replyError("bad_elev");
      return;
    }
    if (v < -500) v = -500;
    if (v > 9000) v = 9000;
    s_stage[row * n + i] = static_cast<int16_t>(v);
    cur = next;
  }
  s_rows_mask |= (1ULL << row);
  replyRow(static_cast<int>(row));
}

void handleEnd() {
  const int n = s_stage_hdr.n;
  const uint64_t full = (n >= 64) ? ~0ULL : ((1ULL << n) - 1ULL);
  if (!s_staging || s_rows_mask != full) {
    s_staging = false;
    replyError("bad_elev");
    return;
  }
  s_staging = false;
  s_hdr = s_stage_hdr;
  memcpy(s_h, s_stage, sizeof(int16_t) * n * n);
  s_have = true;
  ++s_gen;
  save();
  reply("OK", "{}");
  Serial.printf("terrain: %dx%d grid, %.1f km\n", n, n, s_hdr.half_km);
}

void clear() {
  s_have = false;
  ++s_gen;
  Preferences p;
  if (p.begin(kNs, false)) {
    p.clear();
    p.end();
  }
}

void handleClear() {
  clear();
  reply("OK", "{}");
}

}  // namespace services::terrain
