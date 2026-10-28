#include "services/terrain.h"
#include "hardware/console.h"

#include <Arduino.h>
#include <Preferences.h>

#include <ArduinoJson.h>

#include <algorithm>
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
float s_max_h = 0.0f;

void updateMax() {
  s_max_h = 0.0f;
  if (!s_have) return;
  for (int i = 0; i < s_hdr.n * s_hdr.n; ++i) s_max_h = std::max<float>(s_max_h, s_h[i]);
}

// Upload staging
bool s_staging = false;
Header s_stage_hdr{};
int16_t* s_stage = nullptr;  // only during an upload
uint64_t s_rows_mask = 0;

void reply(const char* tag, const char* body) {
  char line[64];
  const int n = snprintf(line, sizeof(line), "PR:%s %s\n", tag, body);
  if (n > 0) Console.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
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
      updateMax();
    }
  }
  p.end();
}

bool available() { return s_have; }
float maxHeightM() { return s_have ? s_max_h : 0.0f; }
uint32_t generation() { return s_gen; }

float heightM(float x_km, float y_km) {
  if (!s_have) return 0.0f;
  // Offset of the grid centre from the radar centre, kept until the location or the
  // grid changes (this is called for every projected point, many thousand per frame).
  static uint32_t c_gen = 0;
  static double c_lat = 1e9, c_lon = 1e9;
  static float gx = 0, gy = 0, inv_step = 1, half = 0;
  const double lat = services::location::lat(), lon = services::location::lon();
  if (c_gen != s_gen || lat != c_lat || lon != c_lon) {
    c_gen = s_gen;
    c_lat = lat;
    c_lon = lon;
    const float lat0 = static_cast<float>(lat);
    gx = static_cast<float>(s_hdr.lon - lon) * kKmPerDeg * cosf(lat0 * kDegToRad);
    gy = static_cast<float>(s_hdr.lat - lat) * kKmPerDeg;
    half = s_hdr.half_km;
    inv_step = static_cast<float>(s_hdr.n - 1) / (2.0f * s_hdr.half_km);
  }
  const int n = s_hdr.n;
  // The grid covers a square; towards its edge the heights fade out over a ring, so
  // the relief ends on a smooth circle instead of on the square's steps.
  const float dx = x_km - gx, dy = y_km - gy, r2 = dx * dx + dy * dy;
  const float r_out = half, r_in = half * 0.82f;
  if (r2 >= r_out * r_out) return 0.0f;
  float fade = 1.0f;
  if (r2 > r_in * r_in) {
    const float t = (r_out - sqrtf(r2)) / (r_out - r_in);  // 1 inside .. 0 at the edge
    fade = t * t * (3.0f - 2.0f * t);                       // smoothstep
  }
  const float fx = (x_km - gx + half) * inv_step;
  const float fy = (y_km - gy + half) * inv_step;
  if (fx < 0.0f || fy < 0.0f || fx > n - 1 || fy > n - 1) return 0.0f;
  const int ix = std::min(static_cast<int>(fx), n - 2);
  const int iy = std::min(static_cast<int>(fy), n - 2);
  const float tx = fx - ix, ty = fy - iy;
  const int16_t* row = s_h + iy * n + ix;
  const float h00 = row[0], h10 = row[1], h01 = row[n], h11 = row[n + 1];
  const float h = (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
  return h > 0.0f ? h * fade : 0.0f;  // sea and below-sea-level land drawn at sea level
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
  if (s_stage == nullptr) s_stage = static_cast<int16_t*>(malloc(sizeof(int16_t) * kMaxN * kMaxN));
  if (s_stage == nullptr) {
    replyError("no_ram");
    return;
  }
  memset(s_stage, 0, sizeof(int16_t) * kMaxN * kMaxN);
  s_rows_mask = 0;
  s_staging = true;
  replyRow(-1);
}

void handleRow(char* args) {
  if (!s_staging || s_stage == nullptr) {
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
  free(s_stage);
  s_stage = nullptr;
  s_have = true;
  ++s_gen;
  updateMax();
  save();
  reply("OK", "{}");
  Console.printf("terrain: %dx%d grid, %.1f km\n", n, n, s_hdr.half_km);
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
