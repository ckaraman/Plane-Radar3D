#include "ui/radar3d.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/clock_service.h"
#include "services/display_prefs.h"
#include "services/map_store.h"
#include "services/radar_location.h"
#include "services/route_service.h"
#include "services/terrain.h"
#include "services/sht30.h"
#include "services/weather_parse.h"
#include "services/usb_feed.h"
#include "ui/radar_range.h"
#include "ui/screen_switch.h"
#include "ui/weather_icons.h"
#include "ui/fonts_tr.h"

namespace ui::radar3d {

namespace {

constexpr float kDegToRad = 0.01745329252f;
constexpr float kKmPerDeg = 111.0f;
constexpr int kW = config::kDisplayWidth;
constexpr int kH = config::kDisplayHeight;

// --- 16-color palette (index = color number in the 4-bit sprite) ---
enum Pal : uint8_t {
  kBg = 0,
  kLandDark,  // shaded slopes of the terrain
  kLand,      // land fill (flat ground and lit slopes)
  kRingDim,
  kRing,
  kCyan,      // FL250+ and the home marker
  kCream,     // FL100-250
  kOrange,    // below FL100
  kText,
  kTextDim,
  kPanel,
  kRed,       // home marker
  kLandLight,  // sunlit slopes of the terrain
  kCoast,
  kRoad,
  kSea,       // sea fill of the map layer (lighter than the land/ground)
};

struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kPaletteHighContrast[16] = {
    {0, 0, 0},       {58, 68, 34},    {96, 112, 54},   {60, 130, 165},
    {90, 180, 225},  {110, 215, 255}, {255, 248, 225}, {255, 170, 60},
    {255, 255, 255}, {175, 188, 198}, {12, 26, 38},    {255, 45, 45},
    {146, 164, 84},  {40, 96, 150},   {178, 176, 160}, {8, 40, 92},
};
constexpr Rgb kPalette[16] = {
    {5, 8, 13},      {44, 52, 27},    {76, 89, 43},    {31, 81, 104},
    {42, 112, 144},  {91, 200, 245},  {237, 230, 211}, {255, 159, 67},
    {230, 237, 242}, {125, 139, 150}, {11, 22, 32},    {225, 40, 40},
    {112, 128, 64},  {34, 80, 126},   {146, 144, 130}, {8, 32, 72},
};

// --- Camera (same numbers as the canvas mockup, scaled with the range) ---
constexpr float kTerrainRelief = 0.15f;  // highest terrain point = 15 % of the radius
// The disc is scaled to fill this box (any tilt); aircraft rise into the space above it.
constexpr float kFitLeft = 6.0f;
constexpr float kFitRight = 474.0f;
constexpr float kFitBottom = 304.0f;
constexpr float kFitTop = 84.0f;
constexpr float kCamDistOverR = 2.1f;  // camera ground distance / outer radius

// --- Layout ---
constexpr int kTopBarH = 22;
// Arrow keys like a keyboard, bottom left: up above down, left/down/right in a row.
// Big enough for the resistive panel; the 4 px gap keeps neighbouring keys apart.
constexpr int kBtnSize = 42;
constexpr int kPadStep = kBtnSize + 4;
constexpr int kPadLeft = 6;
constexpr int kPadTop = kH - 4 - kBtnSize - kPadStep;
constexpr int kPadW = 2 * kPadStep + kBtnSize;
constexpr int kPadH = kPadStep + kBtnSize;
constexpr int kPitchStepDeg = 8;
constexpr int kCardW = 196;
constexpr int kCardHFull = 116;
constexpr int kCardHCompact = 44;
constexpr int kCardX = kW - kCardW - 4;
constexpr int kCardY = 22 + 4;  // under the top bar, in the sky area
constexpr int kTapRadiusPx = 38;
constexpr float kYawStepDeg = 30.0f;

LGFX_Sprite s_spr(&tft);
bool s_card_expanded = false;  // tap the card to switch between 2 lines and full details
int cardH() { return s_card_expanded ? kCardHFull : kCardHCompact; }
bool s_ready = false;

float s_yaw_cur = 20.0f;  // animated camera heading
float s_yaw_target = 20.0f;
char s_selected[9] = "";  // callsign of the tapped aircraft ("" = nearest)

struct Cam {
  float cyaw, syaw, cp, sp, D, H, R, zscale, tscale, pitch_deg;
  float f, cx, cy;  // screen scale and centre, fitted so the disc fills the view
};
bool s_palette_hc = false;
bool s_palette_set = false;

struct Hit {
  int x, y;
  char callsign[9];
};
Hit s_hits[services::adsb::kMaxAircraft];
size_t s_hit_count = 0;

services::adsb::Aircraft s_planes[services::adsb::kMaxAircraft];

struct DrawItem {
  uint8_t idx;
  float x_km, y_km, z;  // world position (z already scaled)
  float depth;
};
DrawItem s_items[services::adsb::kMaxAircraft];
struct LabelPos {
  int x, y, size;
};
LabelPos s_label_pos[services::adsb::kMaxAircraft];

// ---------------------------------------------------------------- projection

Cam makeCam() {
  Cam c;
  const float yaw = s_yaw_cur * kDegToRad;
  c.pitch_deg = services::display_prefs::get().pitch_deg;
  const float pitch = c.pitch_deg * kDegToRad;
  c.cyaw = cosf(yaw);
  c.syaw = sinf(yaw);
  c.cp = cosf(pitch);
  c.sp = sinf(pitch);
  c.R = ui::radar::rangeCurrent().outer_km;
  c.D = c.R * kCamDistOverR;
  c.H = c.D * tanf(pitch);
  // Altitude is exaggerated so it reads at a 50 km range; scale it down with
  // smaller ranges so the view keeps its proportions.
  c.zscale = 0.0003048f * static_cast<float>(services::display_prefs::get().exag) * (c.R / 50.0f);
  // Terrain: scale so the highest point of the loaded grid rises to ~15 % of the
  // radius, whatever the range or how flat the area is, so the relief stays visible.
  const float max_h = std::max(60.0f, services::terrain::maxHeightM());
  c.tscale = kTerrainRelief * c.R / max_h;

  // Fit: project the outer ring with unit scale, then scale/centre it into the box.
  float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
  for (int i = 0; i < 48; ++i) {
    const float t = static_cast<float>(i) / 48.0f * 6.2831853f;
    const float xr = c.R * cosf(t), yr = c.R * sinf(t);
    const float py = yr + c.D, pz = -c.H;
    const float d = py * c.cp - pz * c.sp;
    const float v = py * c.sp + pz * c.cp;
    const float ux = xr / d, uy = -v / d;
    xmin = std::min(xmin, ux); xmax = std::max(xmax, ux);
    ymin = std::min(ymin, uy); ymax = std::max(ymax, uy);
  }
  c.f = std::min((kFitRight - kFitLeft) / (xmax - xmin), (kFitBottom - kFitTop) / (ymax - ymin));
  c.cx = 0.5f * (kFitLeft + kFitRight) - c.f * 0.5f * (xmin + xmax);
  c.cy = kFitBottom - c.f * ymax;
  return c;
}

/** Camera-frame point (xr right, yr forward, z up, km) -> screen. */
bool projectCam(const Cam& c, float xr, float yr, float z, float* sx, float* sy,
                float* depth = nullptr) {
  const float py = yr + c.D;
  const float pz = z - c.H;
  const float d = py * c.cp - pz * c.sp;
  if (d < 0.05f * c.R) {
    return false;  // behind / too close to the camera
  }
  const float v = py * c.sp + pz * c.cp;
  *sx = c.cx + c.f * xr / d;
  *sy = c.cy - c.f * v / d;
  if (depth != nullptr) *depth = d;
  return true;
}

/** World point (x east, y north, z up, km from home) -> screen. */
bool project(const Cam& c, float x, float y, float z, float* sx, float* sy,
             float* depth = nullptr) {
  const float xr = x * c.cyaw - y * c.syaw;
  const float yr = x * c.syaw + y * c.cyaw;
  return projectCam(c, xr, yr, z, sx, sy, depth);
}

int ri(float v) { return static_cast<int>(lroundf(v)); }

/** Ground height in drawing units (km, scaled) at a world point; 0 without terrain data. */
float groundZ(const Cam& c, float x, float y) {
  if (!services::terrain::available()) return 0.0f;
  return services::terrain::heightM(x, y) * c.tscale;
}

// ---------------------------------------------------------------- helpers

uint8_t altitudeColor(int32_t ft) {
  if (ft < 10000) return kOrange;
  if (ft < 25000) return kCream;
  return kCyan;
}

void formatAltitude(int32_t ft, char* out, size_t n) {
  if (ft <= 0) {
    snprintf(out, n, "YER");
  } else if (ft >= 1000) {
    // Same style as the round radar: 1.2K ft, 35K ft (decimal only when not .0)
    const int tenths = static_cast<int>((ft + 50) / 100);
    if (tenths % 10 == 0) {
      snprintf(out, n, "%dK ft", tenths / 10);
    } else {
      snprintf(out, n, "%d.%dK ft", tenths / 10, tenths % 10);
    }
  } else {
    snprintf(out, n, "%d ft", static_cast<int>(ft));
  }
}

const char* compass8(float bearing_deg) {
  static const char* kNames[8] = {"K", "KD", "D", "GD", "G", "GB", "B", "KB"};
  int i = static_cast<int>(lroundf(bearing_deg / 45.0f)) % 8;
  if (i < 0) i += 8;
  return kNames[i];
}

void offsetKm(float lat, float lon, float* x, float* y) {
  const float lat0 = static_cast<float>(services::location::lat());
  const float lon0 = static_cast<float>(services::location::lon());
  *x = (lon - lon0) * kKmPerDeg * cosf(lat0 * kDegToRad);
  *y = (lat - lat0) * kKmPerDeg;
}

/** Position now, dead-reckoned along the track from the last fetch. */
void currentPositionKm(const services::adsb::Aircraft& a, unsigned long fetched_ms, float* x,
                       float* y) {
  offsetKm(a.lat, a.lon, x, y);
  const unsigned long elapsed = millis() - fetched_ms + a.pos_age_ms;
  const float dist = a.gs_knots * 1.852f * (static_cast<float>(elapsed) / 3600000.0f);
  const float r = a.track_deg * kDegToRad;
  *x += dist * sinf(r);
  *y += dist * cosf(r);
}

void dottedLine(int x0, int y0, int x1, int y1, uint8_t color, int on = 2, int off = 2) {
  const int dx = x1 - x0, dy = y1 - y0;
  const int steps = std::max(std::abs(dx), std::abs(dy));
  if (steps == 0) return;
  const int period = on + off;
  for (int i = 0; i <= steps; ++i) {
    if (i % period < on) {
      s_spr.drawPixel(x0 + dx * i / steps, y0 + dy * i / steps, color);
    }
  }
}

void thickLine(int x0, int y0, int x1, int y1, uint8_t color) {
  for (int o = -1; o <= 1; ++o) {
    s_spr.drawLine(x0 + o, y0, x1 + o, y1, color);
    s_spr.drawLine(x0, y0 + o, x1, y1 + o, color);
  }
}

/** Top-down airplane silhouette, rotated to the on-screen heading. */
void drawPlaneShape(int cx, int cy, float heading_deg, float scale, uint8_t color);

/** Aircraft icon with a 1 px dark outline so it stands out on land, sea and map lines. */
void drawPlaneIcon(int cx, int cy, float heading_deg, float scale, uint8_t color) {
  drawPlaneShape(cx - 1, cy, heading_deg, scale, kBg);
  drawPlaneShape(cx + 1, cy, heading_deg, scale, kBg);
  drawPlaneShape(cx, cy - 1, heading_deg, scale, kBg);
  drawPlaneShape(cx, cy + 1, heading_deg, scale, kBg);
  drawPlaneShape(cx, cy, heading_deg, scale, color);
}

void drawPlaneShape(int cx, int cy, float heading_deg, float scale, uint8_t color) {
  const float rad = heading_deg * kDegToRad;
  const float fx = sinf(rad) * scale, fy = -cosf(rad) * scale;
  const float rx = cosf(rad) * scale, ry = sinf(rad) * scale;
  auto px = [&](float f, float r) { return cx + ri(f * fx + r * rx); };
  auto py = [&](float f, float r) { return cy + ri(f * fy + r * ry); };
  auto tri = [&](float f0, float r0, float f1, float r1, float f2, float r2) {
    s_spr.fillTriangle(px(f0, r0), py(f0, r0), px(f1, r1), py(f1, r1), px(f2, r2), py(f2, r2),
                       color);
  };
  auto quad = [&](float f0, float r0, float f1, float r1, float f2, float r2, float f3,
                  float r3) {
    tri(f0, r0, f1, r1, f2, r2);
    tri(f0, r0, f2, r2, f3, r3);
  };
  tri(8.0f, 0.0f, 5.0f, 1.3f, 5.0f, -1.3f);
  quad(5.0f, -1.3f, 5.0f, 1.3f, -7.0f, 1.0f, -7.0f, -1.0f);
  quad(2.0f, 1.0f, -2.0f, 8.0f, -4.0f, 8.0f, -1.0f, 1.0f);
  quad(2.0f, -1.0f, -1.0f, -1.0f, -4.0f, -8.0f, -2.0f, -8.0f);
  quad(-5.0f, 0.8f, -7.2f, 3.8f, -8.6f, 3.8f, -7.0f, 0.8f);
  quad(-5.0f, -0.8f, -7.0f, -0.8f, -8.6f, -3.8f, -7.2f, -3.8f);
}

void setFont(const lgfx::IFont* f) {
  s_spr.setFont(f);
  s_spr.setTextSize(1);
}

/** Text with a dark halo (top-left datum), readable over any background. */
void haloText(const char* t, int x, int y, uint8_t color) {
  s_spr.setTextDatum(textdatum_t::top_left);
  s_spr.setTextColor(kBg);
  s_spr.drawString(t, x - 1, y);
  s_spr.drawString(t, x + 1, y);
  s_spr.drawString(t, x, y - 1);
  s_spr.drawString(t, x, y + 1);
  s_spr.setTextColor(color);
  s_spr.drawString(t, x, y);
}

/** Bytes taking up the first `chars` letters of a UTF-8 string. */
int utf8Bytes(const char* s, int chars) {
  int i = 0;
  while (s[i] != '\0' && chars > 0) {
    ++i;
    while ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    --chars;
  }
  return i;
}

// ---------------------------------------------------------------- label layout

struct Box {
  int x, y, w, h;
};
Box s_label_boxes[services::adsb::kMaxAircraft + 8];
size_t s_label_box_count = 0;

bool intersects(const Box& a, const Box& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/** Fixed overlay boxes: top bar + legend, controls, info card. */
bool overlapsUi(int x, int y, int w, int h) {
  const Box me{x, y, w, h};
  const Box fixed[] = {
      {0, 0, kW, kTopBarH + 2},
      {0, kTopBarH, 112, 58},                                  // legend
      {kPadLeft - 2, kPadTop - 2, kPadW + 4, kPadH + 4},        // arrow keys
      {kCardX - 2, kCardY - 2, kCardW + 4, cardH() + 4},       // card
      {ui::kSwitchX - 2, ui::kSwitchY - 2, ui::kSwitchW + 4, ui::kSwitchH + 4},  // HAVA
      {ui::kWaterBtnX - 2, ui::kWaterBtnY - 2, ui::kWaterBtnW + 4, ui::kWaterBtnH + 4},  // SU
  };
  for (const Box& b : fixed) {
    if (intersects(me, b)) return true;
  }
  return false;
}

/** Reserve a label box if it is free; returns false when it would overlap. */
bool claimLabel(const Box& b) {
  if (b.x < 0 || b.x + b.w > kW || b.y < kTopBarH || b.y + b.h > kH) return false;
  if (overlapsUi(b.x, b.y, b.w, b.h)) return false;
  for (size_t i = 0; i < s_label_box_count; ++i) {
    if (intersects(b, s_label_boxes[i])) return false;
  }
  if (s_label_box_count < sizeof(s_label_boxes) / sizeof(s_label_boxes[0])) {
    s_label_boxes[s_label_box_count++] = b;
  }
  return true;
}

// ---------------------------------------------------------------- layers

void drawGround(const Cam& c) {
  const float horizon = c.cy - c.f * tanf(c.pitch_deg * kDegToRad);
  const int hy = std::max(kTopBarH, ri(horizon));
  s_spr.fillScreen(kBg);
  s_spr.drawFastHLine(0, hy, kW, kRingDim);
}

// ---------------------------------------------------------------- sea fill
//
// The map blob's type-2 rings are sea polygons (even-odd). They are projected
// once per camera change and filled scanline by scanline, clipped to the
// projected outer ring so the sea stays on the radar disc.

constexpr size_t kMaxRings = 256;
struct RingSet {
  int16_t* xy = nullptr;  // x0,y0,x1,y1,... screen coordinates
  size_t cap = 0;         // points the buffer can hold
  size_t points = 0;
  uint16_t start[kMaxRings];
  uint16_t len[kMaxRings];
  int16_t ymin[kMaxRings];
  int16_t ymax[kMaxRings];
  size_t count = 0;
};
RingSet s_sea;    // map type 2: sea polygons (even-odd), at sea level
RingSet s_lakes;  // map type 3: lakes and reservoirs, draped on the terrain
int16_t s_disc_l[kH];
int16_t s_disc_r[kH];
struct SeaKey {
  uint32_t gen, tgen;
  float yaw, R, pitch;
  double lat, lon;
} s_sea_key = {0xFFFFFFFFu, 0, -1, -1, -1, 0, 0};

void buildDiscSpans(const Cam& c) {
  constexpr int kSeg = 144;
  static int16_t px[kSeg], py[kSeg];
  for (int i = 0; i < kSeg; ++i) {
    const float t = static_cast<float>(i) / kSeg * 6.2831853f;
    float sx = 0, sy = 0;
    project(c, c.R * cosf(t), c.R * sinf(t), 0, &sx, &sy);
    px[i] = static_cast<int16_t>(ri(sx));
    py[i] = static_cast<int16_t>(ri(sy));
  }
  for (int y = 0; y < kH; ++y) {
    const float ys = y + 0.5f;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < kSeg; ++i) {
      const int j = (i + 1) % kSeg;
      const float y0 = py[i], y1 = py[j];
      if ((y0 <= ys) != (y1 <= ys)) {
        const float x = px[i] + (ys - y0) * (px[j] - px[i]) / (y1 - y0);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
    }
    s_disc_l[y] = lo > hi ? 1 : static_cast<int16_t>(ceilf(lo));
    s_disc_r[y] = lo > hi ? 0 : static_cast<int16_t>(floorf(hi));
  }
}

/** Project all map rings of one type (2 sea, 3 lakes) into a ring set. */
void buildRings(const Cam& c, uint8_t want, bool draped, RingSet* set) {
  set->points = 0;
  set->count = 0;
  const uint8_t* m = services::map::data();
  const size_t size = services::map::size();
  if (m == nullptr || size < services::map::kHeaderBytes) return;
  int32_t lat_e7, lon_e7;
  uint16_t count;
  memcpy(&lat_e7, m + 4, 4);
  memcpy(&lon_e7, m + 8, 4);
  memcpy(&count, m + 12, 2);
  float ox, oy;
  offsetKm(lat_e7 / 1e7f, lon_e7 / 1e7f, &ox, &oy);

  size_t need = 0;
  size_t pos = services::map::kHeaderBytes;
  for (uint16_t pl = 0; pl < count && pos + 3 <= size; ++pl) {
    uint16_t n;
    memcpy(&n, m + pos + 1, 2);
    if (m[pos] == want) need += static_cast<size_t>(n) * 2;  // clipping can add points
    pos += 3 + static_cast<size_t>(n) * 4;
  }
  if (need == 0) return;
  if (need > set->cap) {
    int16_t* buf = static_cast<int16_t*>(realloc(set->xy, need * 2 * sizeof(int16_t)));
    if (buf == nullptr) return;  // not enough RAM: no fill, lines still draw
    set->xy = buf;
    set->cap = need;
  }

  pos = services::map::kHeaderBytes;
  for (uint16_t pl = 0; pl < count && pos + 3 <= size; ++pl) {
    const uint8_t type = m[pos];
    uint16_t n;
    memcpy(&n, m + pos + 1, 2);
    pos += 3;
    if (pos + static_cast<size_t>(n) * 4 > size) return;
    if (type != want || n < 3 || set->count >= kMaxRings) {
      pos += static_cast<size_t>(n) * 4;
      continue;
    }
    const size_t start = set->points;
    int16_t ymin = 32767, ymax = -32768;
    // Clip the ring against a plane in front of the camera (Sutherland-Hodgman) so
    // far-away points behind the viewer do not fold the polygon over the disc.
    const float near_y = -(c.D - 0.3f * c.R);
    auto toCam = [&](size_t at, float* xr, float* yr, float* z) {
      int16_t xi, yi;
      memcpy(&xi, m + at, 2);
      memcpy(&yi, m + at + 2, 2);
      const float x = ox + xi * 0.01f, y = oy + yi * 0.01f;
      *xr = x * c.cyaw - y * c.syaw;
      *yr = x * c.syaw + y * c.cyaw;
      *z = draped ? groundZ(c, x, y) : 0.0f;
    };
    auto emit = [&](float xr, float yr, float z) {
      float sx = 0, sy = 0;
      projectCam(c, xr, yr, z, &sx, &sy);
      sx = std::max(-2000.0f, std::min(2480.0f, sx));
      sy = std::max(-2000.0f, std::min(2320.0f, sy));
      const int16_t ix = static_cast<int16_t>(ri(sx)), iy = static_cast<int16_t>(ri(sy));
      set->xy[set->points * 2] = ix;
      set->xy[set->points * 2 + 1] = iy;
      ymin = std::min(ymin, iy);
      ymax = std::max(ymax, iy);
      ++set->points;
    };
    float pxr, pyr, pz;
    toCam(pos + static_cast<size_t>(n - 1) * 4, &pxr, &pyr, &pz);
    for (uint16_t i = 0; i < n; ++i) {
      float xr, yr, z;
      toCam(pos + static_cast<size_t>(i) * 4, &xr, &yr, &z);
      const bool in = yr >= near_y, pin = pyr >= near_y;
      if (in != pin) {
        const float t = (near_y - pyr) / (yr - pyr);
        emit(pxr + t * (xr - pxr), near_y, pz + t * (z - pz));
      }
      if (in) emit(xr, yr, z);
      pxr = xr;
      pyr = yr;
      pz = z;
    }
    pos += static_cast<size_t>(n) * 4;
    const size_t got = set->points - start;
    if (got < 3) {
      set->points = start;
      continue;
    }
    set->start[set->count] = static_cast<uint16_t>(start);
    set->len[set->count] = static_cast<uint16_t>(got);
    set->ymin[set->count] = ymin;
    set->ymax[set->count] = ymax;
    ++set->count;
  }
}

/** Scanline even-odd fill of a ring set, clipped to the radar disc. */
void fillRings(const RingSet& set, uint8_t color) {
  if (set.count == 0) return;
  constexpr int kMaxCross = 128;
  float xs[kMaxCross];
  for (int y = kTopBarH; y < kH; ++y) {
    if (s_disc_l[y] > s_disc_r[y]) continue;
    const float ys = y + 0.5f;
    int n = 0;
    for (size_t r = 0; r < set.count && n < kMaxCross; ++r) {
      if (ys < set.ymin[r] || ys > set.ymax[r]) continue;
      const int16_t* p = set.xy + set.start[r] * 2;
      const int len = set.len[r];
      for (int i = 0; i < len && n < kMaxCross; ++i) {
        const int j = (i + 1 == len) ? 0 : i + 1;
        const float y0 = p[i * 2 + 1], y1 = p[j * 2 + 1];
        if ((y0 <= ys) != (y1 <= ys)) {
          xs[n++] = p[i * 2] + (ys - y0) * (p[j * 2] - p[i * 2]) / (y1 - y0);
        }
      }
    }
    if (n < 2) continue;
    for (int i = 1; i < n; ++i) {  // insertion sort, n is small
      const float v = xs[i];
      int k = i;
      while (k > 0 && xs[k - 1] > v) {
        xs[k] = xs[k - 1];
        --k;
      }
      xs[k] = v;
    }
    for (int k = 0; k + 1 < n; k += 2) {
      const int xa = std::max<int>(static_cast<int>(ceilf(xs[k])), s_disc_l[y]);
      const int xb = std::min<int>(static_cast<int>(floorf(xs[k + 1])), s_disc_r[y]);
      if (xb >= xa) s_spr.drawFastHLine(xa, y, xb - xa + 1, color);
    }
  }
}

// ---------------------------------------------------------------- terrain relief
//
// The terrain is drawn as filled cells (no grid lines), back to front, each in the
// land colour or, on slopes facing away from a light in the north-west, the shaded
// land colour. Only cells fully on land (all corners above sea level) and inside the
// disc are drawn; flat land and the sea come from the fills underneath. Projection,
// shading and draw order are cached until the camera or the data changes.

constexpr int kReliefN = 40;  // cells per side over the disc's bounding square
int16_t s_rv_x[(kReliefN + 1) * (kReliefN + 1)];
int16_t s_rv_y[(kReliefN + 1) * (kReliefN + 1)];
uint16_t s_rc_order[kReliefN * kReliefN];
uint8_t s_rv_shade[(kReliefN + 1) * (kReliefN + 1)];  // 0 dark .. 128 flat .. 255 lit
uint16_t s_rc_count = 0;
const float* s_rc_depth = nullptr;  // only valid while sorting

// Grid vertices that lie in the sea (map type 2 rings, world coordinates). The terrain
// grid is coarse, so without this the relief cells would spill over the coastline.
// Depends only on the range, the map and the location, not on the camera angle.
uint8_t s_sea_vertex[(kReliefN + 1) * (kReliefN + 1)];
struct MaskKey {
  uint32_t gen;
  float R;
  double lat, lon;
} s_mask_key = {0xFFFFFFFFu, -1, 0, 0};

void buildSeaMask(float R) {
  constexpr int N = kReliefN;
  memset(s_sea_vertex, 0, sizeof(s_sea_vertex));
  const uint8_t* m = services::map::data();
  const size_t size = services::map::size();
  if (m == nullptr || size < services::map::kHeaderBytes) return;
  int32_t lat_e7, lon_e7;
  uint16_t count;
  memcpy(&lat_e7, m + 4, 4);
  memcpy(&lon_e7, m + 8, 4);
  memcpy(&count, m + 12, 2);
  float ox, oy;
  offsetKm(lat_e7 / 1e7f, lon_e7 / 1e7f, &ox, &oy);
  const float step = 2.0f * R / N;
  size_t pos = services::map::kHeaderBytes;
  for (uint16_t pl = 0; pl < count && pos + 3 <= size; ++pl) {
    const uint8_t type = m[pos];
    uint16_t n;
    memcpy(&n, m + pos + 1, 2);
    pos += 3;
    if (pos + static_cast<size_t>(n) * 4 > size) return;
    if (type != 2 || n < 3) {
      pos += static_cast<size_t>(n) * 4;
      continue;
    }
    // Even-odd: every ring edge toggles the vertices to its right on the same row.
    for (uint16_t e = 0; e < n; ++e) {
      const size_t pa = pos + static_cast<size_t>(e) * 4;
      const size_t pb = pos + static_cast<size_t>((e + 1) % n) * 4;
      int16_t ax, ay, bx, by;
      memcpy(&ax, m + pa, 2);
      memcpy(&ay, m + pa + 2, 2);
      memcpy(&bx, m + pb, 2);
      memcpy(&by, m + pb + 2, 2);
      const float x0 = ox + ax * 0.01f, y0 = oy + ay * 0.01f;
      const float x1 = ox + bx * 0.01f, y1 = oy + by * 0.01f;
      if (y0 == y1) continue;
      const float ylo = std::min(y0, y1), yhi = std::max(y0, y1);
      int j0 = static_cast<int>(ceilf((ylo + R) / step)), j1 = static_cast<int>(floorf((yhi + R) / step));
      j0 = std::max(j0, 0);
      j1 = std::min(j1, N);
      for (int j = j0; j <= j1; ++j) {
        const float yv = -R + j * step;
        if (!((y0 <= yv) != (y1 <= yv))) continue;
        const float xc = x0 + (yv - y0) * (x1 - x0) / (y1 - y0);
        int i0 = static_cast<int>(ceilf((xc + R) / step));
        i0 = std::max(i0, 0);
        for (int i = i0; i <= N; ++i) s_sea_vertex[j * (N + 1) + i] ^= 1;
      }
    }
    pos += static_cast<size_t>(n) * 4;
  }
}

void buildRelief(const Cam& c) {
  s_rc_count = 0;
  if (!services::terrain::available()) return;
  constexpr int N = kReliefN;
  const float step = 2.0f * c.R / N;
  const MaskKey mk{services::map::generation(), c.R, services::location::lat(), services::location::lon()};
  if (mk.gen != s_mask_key.gen || mk.R != s_mask_key.R || mk.lat != s_mask_key.lat ||
      mk.lon != s_mask_key.lon) {
    s_mask_key = mk;
    buildSeaMask(c.R);
  }
  // Scratch buffers, only needed while building (kept off the static RAM budget).
  float* hm = static_cast<float*>(malloc(sizeof(float) * (N + 1) * (N + 1)));
  bool* okv = static_cast<bool*>(malloc(sizeof(bool) * (N + 1) * (N + 1)));
  float* depth = static_cast<float*>(malloc(sizeof(float) * N * N));
  if (hm == nullptr || okv == nullptr || depth == nullptr) {
    free(hm);
    free(okv);
    free(depth);
    return;
  }
  for (int j = 0; j <= N; ++j) {
    for (int i = 0; i <= N; ++i) {
      const float x = -c.R + i * step, y = -c.R + j * step;
      const int k = j * (N + 1) + i;
      hm[k] = services::terrain::heightM(x, y);
      float sx = 0, sy = 0;
      okv[k] = project(c, x, y, hm[k] * c.tscale, &sx, &sy);
      s_rv_x[k] = static_cast<int16_t>(std::max(-2000.0f, std::min(2480.0f, sx)));
      s_rv_y[k] = static_cast<int16_t>(std::max(-2000.0f, std::min(2320.0f, sy)));
    }
  }
  // Light from the north-west, a bit above the horizon (world frame: x east, y north).
  // Shading is computed per vertex (central differences) so it can be blended smoothly
  // across each cell when drawing.
  const float lx = -0.55f, ly = 0.55f, lz = 0.63f;
  constexpr float kGain = 2.2f;  // exaggerate the contrast so gentle hills still read
  for (int j = 0; j <= N; ++j) {
    for (int i = 0; i <= N; ++i) {
      const int k = j * (N + 1) + i;
      const int il = std::max(i - 1, 0), ir = std::min(i + 1, N);
      const int jd = std::max(j - 1, 0), ju = std::min(j + 1, N);
      const float dzdx = (hm[j * (N + 1) + ir] - hm[j * (N + 1) + il]) * c.tscale / ((ir - il) * step);
      const float dzdy = (hm[ju * (N + 1) + i] - hm[jd * (N + 1) + i]) * c.tscale / ((ju - jd) * step);
      const float inv = 1.0f / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.0f);
      const float lit = (-dzdx * lx - dzdy * ly + lz) * inv;
      float v = kGain * (lit - lz) * 255.0f;
      // Dead zone: nearly flat ground stays the plain land colour (no dither speckle).
      constexpr float kFlat = 14.0f;
      v = v > kFlat ? v - kFlat : (v < -kFlat ? v + kFlat : 0.0f);
      v += 128.0f;
      s_rv_shade[k] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v)));
    }
  }
  const float r2 = c.R * c.R;
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const int k00 = j * (N + 1) + i, k10 = k00 + 1, k01 = k00 + N + 1, k11 = k01 + 1;
      const float x0 = -c.R + i * step, y0 = -c.R + j * step, x1 = x0 + step, y1 = y0 + step;
      if (x0 * x0 + y0 * y0 > r2 || x1 * x1 + y0 * y0 > r2 || x0 * x0 + y1 * y1 > r2 ||
          x1 * x1 + y1 * y1 > r2) {
        continue;  // outside the disc
      }
      if (hm[k00] < 1.0f || hm[k10] < 1.0f || hm[k01] < 1.0f || hm[k11] < 1.0f) continue;  // sea / shore
      if (s_sea_vertex[k00] | s_sea_vertex[k10] | s_sea_vertex[k01] | s_sea_vertex[k11]) continue;
      if (!okv[k00] || !okv[k10] || !okv[k01] || !okv[k11]) continue;
      const uint16_t idx = static_cast<uint16_t>(j * N + i);
      const float cxw = x0 + 0.5f * step, cyw = y0 + 0.5f * step;
      depth[idx] = cxw * c.syaw + cyw * c.cyaw;  // forward distance in the camera frame
      s_rc_order[s_rc_count++] = idx;
    }
  }
  // Far cells first (painter's algorithm).
  s_rc_depth = depth;
  std::sort(s_rc_order, s_rc_order + s_rc_count,
            [](uint16_t a, uint16_t b) { return s_rc_depth[a] > s_rc_depth[b]; });
  s_rc_depth = nullptr;
  free(hm);
  free(okv);
  free(depth);
}

// 4x4 ordered-dither thresholds (0..15).
constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
constexpr uint8_t kLandTones[3] = {kLandDark, kLand, kLandLight};

inline void putPixel4(uint8_t* buf, int x, int y, uint8_t c) {
  uint8_t& b = buf[y * (kW / 2) + (x >> 1)];
  b = (x & 1) ? static_cast<uint8_t>((b & 0xF0) | c) : static_cast<uint8_t>((b & 0x0F) | (c << 4));
}

/** Triangle with the shade interpolated across it, dithered between the land tones. */
void shadeTriangle(uint8_t* buf, float x0, float y0, float s0, float x1, float y1, float s1,
                   float x2, float y2, float s2) {
  // Sort by y.
  if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); std::swap(s0, s1); }
  if (y2 < y0) { std::swap(x0, x2); std::swap(y0, y2); std::swap(s0, s2); }
  if (y2 < y1) { std::swap(x1, x2); std::swap(y1, y2); std::swap(s1, s2); }
  if (y2 - y0 < 0.5f) return;
  const int ya = std::max(kTopBarH + 1, static_cast<int>(ceilf(y0 - 0.5f)));
  const int yb = std::min(kH - 1, static_cast<int>(ceilf(y2 - 0.5f)) - 1);
  for (int y = ya; y <= yb; ++y) {
    const float yc = y + 0.5f;
    // Long edge 0-2
    const float t = (yc - y0) / (y2 - y0);
    float xa = x0 + t * (x2 - x0), sa = s0 + t * (s2 - s0);
    float xb, sb;
    if (yc < y1) {
      const float u = (y1 - y0) > 1e-3f ? (yc - y0) / (y1 - y0) : 1.0f;
      xb = x0 + u * (x1 - x0);
      sb = s0 + u * (s1 - s0);
    } else {
      const float u = (y2 - y1) > 1e-3f ? (yc - y1) / (y2 - y1) : 0.0f;
      xb = x1 + u * (x2 - x1);
      sb = s1 + u * (s2 - s1);
    }
    if (xb < xa) { std::swap(xa, xb); std::swap(sa, sb); }
    const int xs = std::max(0, static_cast<int>(ceilf(xa - 0.5f)));
    const int xe = std::min(kW - 1, static_cast<int>(ceilf(xb - 0.5f)) - 1);
    if (xe < xs) continue;
    const float ds = (xb - xa) > 1e-3f ? (sb - sa) / (xb - xa) : 0.0f;
    float sv = sa + (xs + 0.5f - xa) * ds;
    const uint8_t* bay = kBayer[y & 3];
    for (int x = xs; x <= xe; ++x, sv += ds) {
      // sv: 0..2 (dark .. flat .. lit); the fraction picks the tone through the dither.
      float tv = sv < 0.0f ? 0.0f : (sv > 1.999f ? 1.999f : sv);
      const int base = static_cast<int>(tv);
      const int lvl = (tv - base) * 16.0f > bay[x & 3] + 0.5f ? base + 1 : base;
      putPixel4(buf, x, y, kLandTones[lvl > 2 ? 2 : lvl]);
    }
  }
}

void drawRelief() {
  constexpr int N = kReliefN;
  uint8_t* buf = static_cast<uint8_t*>(s_spr.getBuffer());
  if (buf == nullptr) return;
  constexpr float kShadeScale = 2.0f / 256.0f;
  for (uint16_t n = 0; n < s_rc_count; ++n) {
    const uint16_t idx = s_rc_order[n];
    const int i = idx % N, j = idx / N;
    const int k00 = j * (N + 1) + i, k10 = k00 + 1, k01 = k00 + N + 1, k11 = k01 + 1;
    const float a0 = s_rv_shade[k00] * kShadeScale, a1 = s_rv_shade[k10] * kShadeScale;
    const float a2 = s_rv_shade[k11] * kShadeScale, a3 = s_rv_shade[k01] * kShadeScale;
    shadeTriangle(buf, s_rv_x[k00], s_rv_y[k00], a0, s_rv_x[k10], s_rv_y[k10], a1, s_rv_x[k11],
                  s_rv_y[k11], a2);
    shadeTriangle(buf, s_rv_x[k00], s_rv_y[k00], a0, s_rv_x[k11], s_rv_y[k11], a2, s_rv_x[k01],
                  s_rv_y[k01], a3);
  }
}

/** Land, sea, terrain and lakes: everything on the ground below the map lines. */
void drawSurface(const Cam& c) {
  const SeaKey key{services::map::generation(), services::terrain::generation(), s_yaw_cur, c.R,
                   c.pitch_deg, services::location::lat(), services::location::lon()};
  if (key.gen != s_sea_key.gen || key.tgen != s_sea_key.tgen || key.yaw != s_sea_key.yaw ||
      key.R != s_sea_key.R || key.pitch != s_sea_key.pitch || key.lat != s_sea_key.lat ||
      key.lon != s_sea_key.lon) {
    s_sea_key = key;
    buildDiscSpans(c);
    buildRings(c, 2, false, &s_sea);
    buildRings(c, 3, true, &s_lakes);
    buildRelief(c);
  }
  for (int y = kTopBarH; y < kH; ++y) {  // flat land
    if (s_disc_l[y] <= s_disc_r[y]) s_spr.drawFastHLine(s_disc_l[y], y, s_disc_r[y] - s_disc_l[y] + 1, kLand);
  }
  fillRings(s_sea, kSea);
  drawRelief();
  fillRings(s_lakes, kSea);
}

void drawMap(const Cam& c) {
  const uint8_t* m = services::map::data();
  const size_t size = services::map::size();
  if (m == nullptr || size < services::map::kHeaderBytes) {
    return;
  }
  int32_t lat_e7, lon_e7;
  uint16_t count;
  memcpy(&lat_e7, m + 4, 4);
  memcpy(&lon_e7, m + 8, 4);
  memcpy(&count, m + 12, 2);
  float ox, oy;
  offsetKm(lat_e7 / 1e7f, lon_e7 / 1e7f, &ox, &oy);
  const float r2 = c.R * c.R * 1.02f;

  size_t pos = services::map::kHeaderBytes;
  for (uint16_t pl = 0; pl < count && pos + 3 <= size; ++pl) {
    const uint8_t type = m[pos];
    uint16_t n;
    memcpy(&n, m + pos + 1, 2);
    pos += 3;
    if (pos + static_cast<size_t>(n) * 4 > size) {
      return;
    }
    if (type == 2 || type > 3) {  // sea fill rings: the coast lines already outline them
      pos += static_cast<size_t>(n) * 4;
      continue;
    }
    const uint8_t color = type == 1 ? kRoad : kCoast;  // coast and lake shores
    const bool closed = type == 3;
    float fsx = 0, fsy = 0;
    bool have_first = false;
    bool have_prev = false;
    float psx = 0, psy = 0;
    for (uint16_t i = 0; i < n; ++i, pos += 4) {
      int16_t xi, yi;
      memcpy(&xi, m + pos, 2);
      memcpy(&yi, m + pos + 2, 2);
      const float x = ox + xi * 0.01f;
      const float y = oy + yi * 0.01f;
      float sx, sy;
      if (x * x + y * y > r2 || !project(c, x, y, groundZ(c, x, y), &sx, &sy)) {
        have_prev = false;
        continue;
      }
      if (have_prev) {
        s_spr.drawLine(ri(psx), ri(psy), ri(sx), ri(sy), color);
      } else if (i == 0) {
        fsx = sx;
        fsy = sy;
        have_first = true;
      }
      psx = sx;
      psy = sy;
      have_prev = true;
    }
    if (closed && have_first && have_prev) s_spr.drawLine(ri(psx), ri(psy), ri(fsx), ri(fsy), color);
  }
}

void ringPath(const Cam& c, float r, uint8_t color, bool dashed) {
  constexpr int kSeg = 72;
  float px = 0, py = 0;
  for (int i = 0; i <= kSeg; ++i) {
    const float t = static_cast<float>(i) / kSeg * 6.2831853f;
    float sx, sy;
    const float rx = r * cosf(t), ry = r * sinf(t);
    if (!project(c, rx, ry, groundZ(c, rx, ry), &sx, &sy)) continue;
    if (i > 0 && (!dashed || (i % 2) == 0)) {
      s_spr.drawLine(ri(px), ri(py), ri(sx), ri(sy), color);
    }
    px = sx;
    py = sy;
  }
}

/** Inner ring radii (km) with round numbers for each range preset. */
void innerRings(float R, float out[2]) {
  if (R <= 15.5f) {
    out[0] = 5;
    out[1] = 10;
  } else if (R <= 25.5f) {
    out[0] = 10;
    out[1] = 20;
  } else if (R <= 35.5f) {
    out[0] = 10;
    out[1] = 20;
  } else {
    out[0] = 10;
    out[1] = 25;
  }
}

bool overlapsUi(int x, int y, int w, int h);

void drawRings(const Cam& c) {
  float inner[2];
  innerRings(c.R, inner);
  if (ui::radar::showRings()) {
    ringPath(c, inner[0], kRingDim, true);
    ringPath(c, inner[1], kRingDim, true);
  }
  ringPath(c, c.R, kRing, false);

  setFont(&fonts::Font0);
  s_spr.setTextDatum(textdatum_t::middle_center);
  const float radii[3] = {inner[0], inner[1], c.R};
  for (int i = 0; i < 3; ++i) {
    if (i < 2 && !ui::radar::showRings()) continue;
    const float r = radii[i];
    float sx, sy;
    if (projectCam(c, r * 0.62f, -r * 0.78f, 0, &sx, &sy)) {
      char label[12];
      ui::radar::formatRing3Label(label, sizeof(label), r, ui::radar::useMiles());
      if (i < 2) {  // "10km" -> "10" on the inner rings
        for (char* p = label; *p; ++p) {
          if (*p < '0' || *p > '9') {
            *p = '\0';
            break;
          }
        }
      }
      s_spr.setTextColor(kRing);
      s_spr.drawString(label, ri(sx) + 12, ri(sy) + 6);
    }
  }

  // Cardinal letters (Turkish: K north, D east, G south, B west).
  setFont(&ui::font::SansBold9);
  const char* names[4] = {"K", "D", "G", "B"};
  const float dirs[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
  for (int i = 0; i < 4; ++i) {
    float sx, sy;
    if (project(c, dirs[i][0] * c.R * 1.14f, dirs[i][1] * c.R * 1.14f, 0, &sx, &sy) &&
        sy < kH - 10 && !overlapsUi(ri(sx) - 8, ri(sy) - 8, 16, 16)) {
      s_spr.setTextColor(kRing);
      s_spr.drawString(names[i], ri(sx), ri(sy));
    }
  }

  float hx, hy;
  if (project(c, 0, 0, groundZ(c, 0, 0), &hx, &hy)) {
    // Red house on the radar centre: roof triangle over a square body, dark outline.
    const int x = ri(hx), y = ri(hy);
    s_spr.fillTriangle(x - 8, y - 5, x + 8, y - 5, x, y - 13, kBg);
    s_spr.fillRect(x - 6, y - 6, 13, 9, kBg);
    s_spr.fillTriangle(x - 7, y - 6, x + 7, y - 6, x, y - 12, kRed);
    s_spr.fillRect(x - 5, y - 6, 11, 8, kRed);
    s_spr.fillRect(x - 1, y - 2, 3, 4, kBg);  // door
    s_spr.drawFastHLine(x - 3, y + 3, 7, kRed);
  }
}

/** Index of the aircraft shown in the card (selected, else nearest), or -1.
 *  Only aircraft inside the outer ring count. */
int cardAircraft(size_t count, const float* dist_km, float R) {
  int nearest = -1;
  int selected = -1;
  for (size_t i = 0; i < count; ++i) {
    if (dist_km[i] > R) continue;
    if (s_selected[0] != '\0' && strcmp(s_planes[i].callsign, s_selected) == 0) {
      selected = static_cast<int>(i);
    }
    if (nearest < 0 || dist_km[i] < dist_km[nearest]) {
      nearest = static_cast<int>(i);
    }
  }
  if (selected >= 0) return selected;
  s_selected[0] = '\0';  // the selected aircraft left the area
  return nearest;
}

void drawAircraft(const Cam& c, size_t count, unsigned long fetched_ms, float* dist_km,
                  float* bearing_deg, int card_idx_out[1]) {
  size_t n_items = 0;
  for (size_t i = 0; i < count; ++i) {
    float x, y;
    currentPositionKm(s_planes[i], fetched_ms, &x, &y);
    dist_km[i] = sqrtf(x * x + y * y);
    float b = atan2f(x, y) / kDegToRad;
    if (b < 0) b += 360.0f;
    bearing_deg[i] = b;
    if (dist_km[i] > c.R * 1.05f) continue;
    const float z = static_cast<float>(std::max<int32_t>(0, s_planes[i].alt_ft)) * c.zscale;
    float sx, sy, d;
    if (!project(c, x, y, z, &sx, &sy, &d)) continue;
    s_items[n_items++] = {static_cast<uint8_t>(i), x, y, z, d};
  }
  // Far first, so near aircraft are drawn on top.
  for (size_t i = 1; i < n_items; ++i) {
    DrawItem t = s_items[i];
    size_t j = i;
    while (j > 0 && s_items[j - 1].depth < t.depth) {
      s_items[j] = s_items[j - 1];
      --j;
    }
    s_items[j] = t;
  }

  const int card_idx = cardAircraft(count, dist_km, c.R);
  card_idx_out[0] = card_idx;
  const bool labels = services::display_prefs::get().labels;
  const float unit = c.R / 50.0f;  // depth unit of the mockup
  s_hit_count = 0;

  for (size_t k = 0; k < n_items; ++k) {
    const DrawItem& it = s_items[k];
    const auto& a = s_planes[it.idx];
    const uint8_t col = altitudeColor(a.alt_ft);
    float sx, sy, gx, gy, d;
    project(c, it.x_km, it.y_km, it.z, &sx, &sy, &d);
    project(c, it.x_km, it.y_km, groundZ(c, it.x_km, it.y_km), &gx, &gy);

    // Trail: where it was over the last minute (straight track).
    const float tr = a.track_deg * kDegToRad;
    const float back_km = a.gs_knots * 1.852f * (12.0f / 3600.0f);
    float lx = sx, ly = sy;
    for (int s = 1; s <= 5; ++s) {
      float tx, ty;
      if (!project(c, it.x_km - sinf(tr) * back_km * s, it.y_km - cosf(tr) * back_km * s, it.z,
                   &tx, &ty)) {
        break;
      }
      dottedLine(ri(lx), ri(ly), ri(tx), ri(ty), col, 1, 3);
      lx = tx;
      ly = ty;
    }

    // Drop line to the ground and the ground shadow give the depth cue.
    dottedLine(ri(sx), ri(sy), ri(gx), ri(gy), col, 2, 2);
    s_spr.fillEllipse(ri(gx), ri(gy), 3, 1, col);

    // Icon, rotated to where the track points on screen.
    float ax, ay;
    float heading = 0.0f;
    if (project(c, it.x_km + sinf(tr) * 2.0f * unit, it.y_km + cosf(tr) * 2.0f * unit, it.z, &ax,
                &ay)) {
      heading = atan2f(ax - sx, -(ay - sy)) / kDegToRad;
    }
    float size = 16.0f * 100.0f / (d / unit);
    if (size < 9.0f) size = 9.0f;
    if (size > 18.0f) size = 18.0f;
    drawPlaneIcon(ri(sx), ri(sy), heading, size / 16.0f * 1.1f, col);

    if (static_cast<int>(it.idx) == card_idx && s_selected[0] != '\0') {
      s_spr.drawCircle(ri(sx), ri(sy), 15, col);
    }

    s_label_pos[k] = {ri(sx), ri(sy), ri(size)};

    if (s_hit_count < services::adsb::kMaxAircraft) {
      Hit& h = s_hits[s_hit_count++];
      h.x = ri(sx);
      h.y = ri(sy);
      memcpy(h.callsign, a.callsign, sizeof(h.callsign));
    }
  }

  if (!labels) return;
  // Labels last, nearest aircraft first (the one in the card before all), each
  // to the right of its icon, else to the left, else left out. Callsign in bold
  // with a dark halo; the altitude line is dropped when the sky is busy (the card
  // still shows it), so many aircraft do not turn into a wall of text.
  const bool busy = n_items > 6;
  s_label_box_count = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t k = n_items; k-- > 0;) {
      const bool is_card = static_cast<int>(s_items[k].idx) == card_idx;
      if ((pass == 0) != is_card) continue;
      const auto& a = s_planes[s_items[k].idx];
      const LabelPos& lp = s_label_pos[k];
      char alt[12];
      formatAltitude(a.alt_ft, alt, sizeof(alt));
      const bool two_lines = !busy || is_card;
      setFont(&ui::font::SansBold8);
      int w = s_spr.textWidth(a.callsign);
      if (two_lines) {
        setFont(&ui::font::Sans8);
        w = std::max<int>(w, s_spr.textWidth(alt));
      }
      w += 3;
      const int h = two_lines ? 30 : 16;
      const int off = ri(lp.size * 0.6f) + 5;
      Box box{lp.x + off, lp.y - 9, w, h};
      if (!claimLabel(box)) {
        box.x = lp.x - off - w;
        if (!claimLabel(box)) continue;
      }
      setFont(&ui::font::SansBold8);
      haloText(a.callsign, box.x + 1, box.y, altitudeColor(a.alt_ft));
      if (two_lines) {
        setFont(&ui::font::Sans8);
        haloText(alt, box.x + 1, box.y + 14, kText);
      }
    }
  }
}

void drawTopBar(size_t in_range, unsigned long fetched_ms) {
  s_spr.fillRect(0, 0, kW, kTopBarH, kPanel);
  s_spr.drawFastHLine(0, kTopBarH, kW, kRingDim);
  const bool fresh = fetched_ms != 0 && millis() - fetched_ms < 20000;
  s_spr.fillCircle(10, 11, 3, fresh ? kCyan : kOrange);

  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::middle_left);
  s_spr.setTextColor(kText);
  s_spr.drawString("RADAR 3B", 19, 11);
  const int src_x = 19 + s_spr.textWidth("RADAR 3B") + 10;
  s_spr.setTextColor(kTextDim);
  s_spr.drawString(services::usb_feed::active() ? "USB" : "adsb.fi", src_x, 11);

  char buf[40];
  char range[12];
  ui::radar::formatRing3Label(range, sizeof(range), ui::radar::rangeCurrent().outer_km,
                              ui::radar::useMiles());
  snprintf(buf, sizeof(buf), "%u uçak  /  %s", static_cast<unsigned>(in_range), range);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.setTextColor(kText);
  s_spr.drawString(buf, kW / 2, 11);

  // Right side: indoor temperature / humidity (SHT30) and the time.
  int x = kW - 8;
  s_spr.setTextDatum(textdatum_t::middle_right);
  tm t;
  if (services::clock::now(&t)) {
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    s_spr.setTextColor(kText);
    s_spr.drawString(buf, x, 11);
    x -= s_spr.textWidth(buf) + 12;
  }
  if (services::sht30::valid()) {
    snprintf(buf, sizeof(buf), "%%%d", static_cast<int>(lroundf(services::sht30::humidity())));
    s_spr.setTextColor(kTextDim);
    s_spr.drawString(buf, x, 11);
    x -= s_spr.textWidth(buf) + 10;
    snprintf(buf, sizeof(buf), "%.1f°C", services::sht30::temperatureC());
    s_spr.drawString(buf, x, 11);
  }
}

void drawLegend() {
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::middle_left);
  const struct {
    uint8_t col;
    const char* text;
  } rows[3] = {{kCyan, "25K ft +"}, {kCream, "10-25K ft"}, {kOrange, "10K ft altı"}};
  for (int i = 0; i < 3; ++i) {
    const int y = kTopBarH + 13 + i * 17;
    s_spr.fillCircle(12, y, 5, rows[i].col);
    s_spr.setTextColor(kText);
    s_spr.drawString(rows[i].text, 23, y + 1);
  }
}

enum class Arrow { Left, Right, Up, Down };

struct PadButton {
  int x, y;
  Arrow dir;
};
PadButton padButton(int i) {
  const int row2 = kPadTop + kPadStep;
  switch (i) {
    case 0: return {kPadLeft, row2, Arrow::Left};
    case 1: return {kPadLeft + 2 * kPadStep, row2, Arrow::Right};
    case 2: return {kPadLeft + kPadStep, kPadTop, Arrow::Up};
    default: return {kPadLeft + kPadStep, row2, Arrow::Down};
  }
}

void drawButton(int x, int y, Arrow dir) {
  s_spr.fillRoundRect(x, y, kBtnSize, kBtnSize, 7, kPanel);
  s_spr.drawRoundRect(x, y, kBtnSize, kBtnSize, 7, kRing);
  const int cx = x + kBtnSize / 2, cy = y + kBtnSize / 2;
  constexpr int a = 8, b = 5;
  switch (dir) {
    case Arrow::Left:
      thickLine(cx + b, cy - a, cx - b, cy, kCyan);
      thickLine(cx - b, cy, cx + b, cy + a, kCyan);
      break;
    case Arrow::Right:
      thickLine(cx - b, cy - a, cx + b, cy, kCyan);
      thickLine(cx + b, cy, cx - b, cy + a, kCyan);
      break;
    case Arrow::Up:
      thickLine(cx - a, cy + b, cx, cy - b, kCyan);
      thickLine(cx, cy - b, cx + a, cy + b, kCyan);
      break;
    case Arrow::Down:
      thickLine(cx - a, cy - b, cx, cy + b, kCyan);
      thickLine(cx, cy + b, cx + a, cy - b, kCyan);
      break;
  }
}

/** Bottom right: opens the forecast screen. Sun + cloud and "HAVA". */
void drawSwitchButton() {
  using namespace ui;
  s_spr.fillRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, 8, kPanel);
  s_spr.drawRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, 8, kRing);
  const weather::IconPalette<Pal> pal{kPanel, kOrange, kCream, kCream, kTextDim,
                                      kCyan,  kText,   kOrange, kTextDim};
  weather::drawWeatherIcon(s_spr, kSwitchX + 22, kSwitchY + kSwitchH / 2, 14,
                           services::weather::Condition::PartlyCloudy, true, pal);
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::middle_left);
  s_spr.setTextColor(kText);
  setFont(&ui::font::SansBold8);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.drawString("HAVA", (kSwitchX + 40 + kSwitchX + kSwitchW) / 2, kSwitchY + kSwitchH / 2 + 1);
}

void drawControls() {
  for (int i = 0; i < 4; ++i) {
    const PadButton b = padButton(i);
    drawButton(b.x, b.y, b.dir);
  }
  drawSwitchButton();
  ui::drawWaterButton(s_spr, ui::kWaterBtnX, ui::kWaterBtnY, ui::kWaterBtnW, ui::kWaterBtnH, kPanel,
                      kRing, kCyan, kText, kBg);
}

/** Two-line card: callsign + route codes, then altitude / speed / distance / climb. */
void drawCardCompact(int idx, const float* dist_km) {
  const auto& a = s_planes[idx];
  const uint8_t col = altitudeColor(a.alt_ft);
  s_spr.fillRoundRect(kCardX, kCardY, kCardW, kCardHCompact, 6, kPanel);
  s_spr.drawRoundRect(kCardX, kCardY, kCardW, kCardHCompact, 6, kRingDim);
  const int x0 = kCardX + 8, x1 = kCardX + kCardW - 8;

  setFont(&ui::font::SansBold9);
  s_spr.setTextDatum(textdatum_t::top_left);
  s_spr.setTextColor(col);
  s_spr.drawString(a.callsign, x0, kCardY + 4);

  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::top_right);
  services::route::Route rt;
  const services::route::Status rs = services::route::get(a.callsign, &rt);
  char buf[40];
  if (rs == services::route::Status::Found) {
    snprintf(buf, sizeof(buf), "%s > %s", rt.from_code[0] ? rt.from_code : "?", rt.to_code[0] ? rt.to_code : "?");
    s_spr.setTextColor(kText);
  } else {
    snprintf(buf, sizeof(buf), "%s", a.type[0] ? a.type : (s_selected[0] ? "" : "en yakin"));
    s_spr.setTextColor(kTextDim);
  }
  s_spr.drawString(buf, x1, kCardY + 6);

  char alt[12];
  formatAltitude(a.alt_ft, alt, sizeof(alt));
  snprintf(buf, sizeof(buf), "%s  %dkt  %.1fkm", alt, static_cast<int>(lroundf(a.gs_knots)), dist_km[idx]);
  s_spr.setTextDatum(textdatum_t::top_left);
  s_spr.setTextColor(kText);
  s_spr.drawString(buf, x0, kCardY + 24);
  const int used = x0 + s_spr.textWidth(buf);
  char tr[12] = "";
  if (a.vrate_fpm > 300) snprintf(tr, sizeof(tr), "^%d", a.vrate_fpm);
  else if (a.vrate_fpm < -300) snprintf(tr, sizeof(tr), "v%d", -a.vrate_fpm);
  if (tr[0] && used + 6 + s_spr.textWidth(tr) <= x1) {
    s_spr.setTextDatum(textdatum_t::top_right);
    s_spr.setTextColor(a.vrate_fpm > 0 ? kCyan : kOrange);
    s_spr.drawString(tr, x1, kCardY + 24);
  }
}

void drawCard(int idx, const float* dist_km, const float* bearing_deg) {
  if (idx < 0) return;
  if (!s_card_expanded) {
    drawCardCompact(idx, dist_km);
    return;
  }
  const auto& a = s_planes[idx];
  const uint8_t col = altitudeColor(a.alt_ft);
  s_spr.fillRoundRect(kCardX, kCardY, kCardW, kCardHFull, 6, kPanel);
  s_spr.drawRoundRect(kCardX, kCardY, kCardW, kCardHFull, 6, kRingDim);

  const int x0 = kCardX + 9;
  int y = kCardY + 6;
  setFont(&ui::font::SansBold9);
  s_spr.setTextDatum(textdatum_t::top_left);
  s_spr.setTextColor(col);
  s_spr.drawString(a.callsign, x0, y);
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::top_right);
  s_spr.setTextColor(kTextDim);
  // Tapped aircraft: its type here. Otherwise the card follows the nearest one
  // and says so (the type moves to the direction line below).
  s_spr.drawString(s_selected[0] ? a.type : "EN YAKIN", kCardX + kCardW - 9, y + 2);

  // Route (from adsbdb, by callsign): "IST > ESB  Istanbul > Ankara"
  y += 22;
  s_spr.setTextDatum(textdatum_t::top_left);
  services::route::Route rt;
  const services::route::Status rs = services::route::get(a.callsign, &rt);
  const int avail = kCardW - 18;
  if (rs == services::route::Status::Found) {
    char codes[16];
    snprintf(codes, sizeof(codes), "%s > %s", rt.from_code[0] ? rt.from_code : "?",
             rt.to_code[0] ? rt.to_code : "?");
    s_spr.setTextColor(kText);
    s_spr.drawString(codes, x0, y);
    int cx = x0 + s_spr.textWidth(codes) + 8;
    char cities[48];
    for (int cut = 19; cut >= 4; cut -= 3) {
      // Cut by letters, not bytes: Turkish letters are two bytes in UTF-8.
      snprintf(cities, sizeof(cities), "%.*s > %.*s", utf8Bytes(rt.from_city, cut), rt.from_city,
               utf8Bytes(rt.to_city, cut), rt.to_city);
      if (cx + s_spr.textWidth(cities) <= x0 + avail) break;
      cities[0] = '\0';
    }
    if (cities[0] && (rt.from_city[0] || rt.to_city[0])) {
      s_spr.setTextColor(kTextDim);
      s_spr.drawString(cities, cx, y);
    }
  } else {
    s_spr.setTextColor(kTextDim);
    s_spr.drawString(rs == services::route::Status::Pending ? "Rota aranıyor..." : "Rota bilinmiyor", x0, y);
  }

  y += 17;
  char buf[32];
  char alt[12];
  formatAltitude(a.alt_ft, alt, sizeof(alt));
  s_spr.setTextDatum(textdatum_t::top_left);
  const int col2 = x0 + 100;
  auto field = [&](int x, int yy, const char* key, const char* val) {
    s_spr.setTextColor(kTextDim);
    s_spr.drawString(key, x, yy);
    s_spr.setTextColor(kText);
    s_spr.drawString(val, x + s_spr.textWidth(key) + 5, yy);
  };
  field(x0, y, "İRT", alt);
  snprintf(buf, sizeof(buf), "%d kt", static_cast<int>(lroundf(a.gs_knots)));
  field(col2, y, "HIZ", buf);
  y += 17;
  snprintf(buf, sizeof(buf), "%.1f km", dist_km[idx]);
  field(x0, y, "MSF", buf);
  snprintf(buf, sizeof(buf), "%03d", static_cast<int>(lroundf(a.track_deg)) % 360);
  field(col2, y, "RTA", buf);
  y += 17;
  if (s_selected[0] == '\0' && a.type[0]) {
    snprintf(buf, sizeof(buf), "%s  -  %s yönünde", a.type, compass8(bearing_deg[idx]));
  } else {
    snprintf(buf, sizeof(buf), "%s yönünde", compass8(bearing_deg[idx]));
  }
  s_spr.setTextColor(kTextDim);
  s_spr.drawString(buf, x0, y);
  y += 17;
  if (a.vrate_fpm > 300) {
    snprintf(buf, sizeof(buf), "^ tırmanıyor  +%d fpm", a.vrate_fpm);
    s_spr.setTextColor(kCyan);
  } else if (a.vrate_fpm < -300) {
    snprintf(buf, sizeof(buf), "v alçalıyor  %d fpm", a.vrate_fpm);
    s_spr.setTextColor(kOrange);
  } else {
    snprintf(buf, sizeof(buf), "- seyir irtifasında");
    s_spr.setTextColor(kTextDim);
  }
  s_spr.drawString(buf, x0, y);
}

void drawEmptyNote(unsigned long fetched_ms) {
  setFont(&ui::font::Sans9);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.setTextColor(kTextDim);
  s_spr.drawString(fetched_ms == 0 ? "Uçak verisi bekleniyor..." : "Menzilde uçak yok", kW / 2,
                   kTopBarH + 26);
}

void applyPalette() {
  // Set every frame: the forecast screen shares the sprite with its own palette.
  const bool hc = services::display_prefs::get().high_contrast;
  const Rgb* pal = hc ? kPaletteHighContrast : kPalette;
  for (int i = 0; i < 16; ++i) s_spr.setPaletteColor(i, pal[i].r, pal[i].g, pal[i].b);
  s_palette_hc = hc;
  s_palette_set = true;
}

void animateYaw() {
  float diff = fmodf(s_yaw_target - s_yaw_cur + 540.0f, 360.0f) - 180.0f;
  if (fabsf(diff) < 0.5f) {
    s_yaw_cur = s_yaw_target;
  } else {
    s_yaw_cur += diff * 0.35f;
  }
}

}  // namespace

bool init() {
  s_spr.setColorDepth(4);
  s_spr.setPsram(false);
  if (s_spr.createSprite(kW, kH) == nullptr) {
    Serial.println("radar3d: sprite allocation failed");
    return false;
  }
  s_spr.createPalette();
  applyPalette();
  s_yaw_cur = s_yaw_target = static_cast<float>(services::display_prefs::get().yaw_deg);
  s_ready = true;
  return true;
}

void drawFrame() {
  if (!s_ready) return;
  applyPalette();
  animateYaw();
  const Cam c = makeCam();

  unsigned long fetched_ms = 0;
  const size_t count =
      services::adsb::snapshotAircraft(s_planes, services::adsb::kMaxAircraft, &fetched_ms);
  static float dist_km[services::adsb::kMaxAircraft];
  static float bearing[services::adsb::kMaxAircraft];

  drawGround(c);
  drawSurface(c);
  drawMap(c);
  drawRings(c);
  int card_idx = -1;
  drawAircraft(c, count, fetched_ms, dist_km, bearing, &card_idx);
  size_t in_range = 0;
  for (size_t i = 0; i < count; ++i) {
    if (dist_km[i] <= c.R) ++in_range;
  }
  drawTopBar(in_range, fetched_ms);
  drawLegend();
  if (in_range == 0) drawEmptyNote(fetched_ms);
  drawControls();
  if (in_range > 0) drawCard(card_idx, dist_km, bearing);

#ifdef RADAR3D_HOST_TEST
  extern void hostCaptureFrame(LGFX_Sprite & spr);  // desktop preview harness
  hostCaptureFrame(s_spr);
#else
  s_spr.pushSprite(0, 0);
#endif
}

LGFX_Sprite* frame() { return s_ready ? &s_spr : nullptr; }

#ifdef RADAR3D_HOST_TEST
bool hostHitPos(const char* callsign, int* x, int* y) {
  for (size_t i = 0; i < s_hit_count; ++i) {
    if (strcmp(s_hits[i].callsign, callsign) == 0) {
      *x = s_hits[i].x;
      *y = s_hits[i].y;
      return true;
    }
  }
  return false;
}
#endif

bool handleTap(int x, int y) {
  for (int i = 0; i < 4; ++i) {
    const PadButton b = padButton(i);
    if (x >= b.x - 2 && x < b.x + kBtnSize + 2 && y >= b.y - 2 && y < b.y + kBtnSize + 2) {
      auto p = services::display_prefs::get();
      if (i == 0 || i == 1) {
        // Right arrow turns the view to the right (the map swings left), left to the left.
        s_yaw_target += i == 0 ? kYawStepDeg : -kYawStepDeg;
        s_yaw_target = fmodf(s_yaw_target + 360.0f, 360.0f);
        p.yaw_deg = static_cast<uint16_t>(lroundf(s_yaw_target)) % 360;
      } else if (i == 2) {
        p.pitch_deg = static_cast<uint8_t>(std::min(62, p.pitch_deg + kPitchStepDeg));
      } else {
        p.pitch_deg = static_cast<uint8_t>(std::max(10, p.pitch_deg - kPitchStepDeg));
      }
      services::display_prefs::save(p);
      return true;
    }
  }
  if (y < kTopBarH + 6) {
    ui::radar::rangeNext();
    return true;
  }
  int best = -1;
  int best_d2 = kTapRadiusPx * kTapRadiusPx;
  for (size_t i = 0; i < s_hit_count; ++i) {
    const int dx = s_hits[i].x - x, dy = s_hits[i].y - y;
    const int d2 = dx * dx + dy * dy;
    if (d2 <= best_d2) {
      best_d2 = d2;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0) {
    memcpy(s_selected, s_hits[best].callsign, sizeof(s_selected));
    return true;
  }
  const bool on_card = x >= kCardX && y >= kCardY && y < kCardY + cardH();
  if (on_card) {
    s_card_expanded = !s_card_expanded;
    return true;
  }
  if (!on_card && s_selected[0] != '\0') {
    s_selected[0] = '\0';  // tap on empty sky: back to "nearest"
    return true;
  }
  return false;
}

}  // namespace ui::radar3d
