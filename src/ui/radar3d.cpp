#include "ui/radar3d.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>
#if !defined(RADAR3D_HOST_TEST)
#include <esp_heap_caps.h>
#if !defined(RADAR3D_HOST_TEST)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif
#endif

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/frame_push.h"
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
#include "ui/ui_scale.h"
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
// The disc is as large as the screen allows: its thin left / right tips may run a
// little past the edges, the near rim sits just above the bottom, and the aircraft
// height scale (fitted to the traffic) uses whatever room is left above it.
constexpr float kFitLeft = -ui::Sf(22.0f);
constexpr float kFitRight = kW + ui::Sf(22.0f);
constexpr float kFitBottom = kH - ui::Sf(6.0f);
constexpr float kFitTop = ui::Sf(70.0f);
constexpr float kCamDistOverR = 2.1f;  // camera ground distance / outer radius

// --- Layout ---
using ui::S;
constexpr int kTopBarH = S(22);
// Tilt range: steeper views turn the disc into a circle that leaves the sides of the
// wide screen black, flatter ones squash it; within this range it fills the width.
constexpr int kPitchMinDeg = 14;
constexpr int kPitchMaxDeg = 26;
// Altitude legend: a thin strip under the top bar, hidden while an aircraft is selected.
constexpr int kStripH = S(18);
// Selected aircraft, "corners" style: who / where to on the left, flight values on the right.
constexpr int kInfoY = kTopBarH + S(4);
constexpr int kInfoH = S(62);
constexpr int kInfoLW = S(168);
constexpr int kInfoRW = S(150);
constexpr int kInfoRX = kW - kInfoRW - S(4);
constexpr int kTapRadiusPx = S(38);

// The radar is drawn in horizontal bands into a small buffer in internal RAM (fast,
// no cache misses) and each band is copied straight into the panel's frame buffer:
// PSRAM is written once per frame instead of drawing a whole frame in PSRAM, reading
// it back and writing it again. s_spr is a full-screen "view" whose buffer pointer is
// moved so that the current band's rows land in the band buffer; drawing is clipped
// to those rows. The forecast and water screens keep a whole-frame sprite (s_frame).
LGFX_Sprite s_spr(&tft);
LGFX_Sprite s_frame(&tft);
// One band of the full height: measured on the board, drawing in 40-row bands into
// internal RAM cost far more in per-band overhead than it saved (the frame time is
// CPU, not memory, bound), so the frame is drawn in one pass into s_frame's buffer.
constexpr int kBandH = kH;
uint8_t* s_band_mem = nullptr;
int s_band_y0 = 0, s_band_y1 = kH;  // rows of the current band [y0, y1)
bool s_first_band = true;           // per-frame state is updated in the first band only

void selectBand(int y0) {
  s_band_y0 = y0;
  s_band_y1 = std::min(kH, y0 + kBandH);
  // Rows y0..y1-1 of the full-screen view map onto the band buffer.
  s_spr.setBuffer(s_band_mem - static_cast<ptrdiff_t>(y0) * kW, kW, kH);
  s_spr.setClipRect(0, y0, kW, s_band_y1 - y0);
}
/** How the selected aircraft's details are shown (display_prefs info_style). */
bool infoInLabel() { return services::display_prefs::get().info_style == 1; }
bool s_ready = false;

float s_yaw_cur = 20.0f;  // animated camera heading
// Altitude auto-fit: shrinks the vertical scale smoothly when the highest aircraft
// would rise above the top of the view, and relaxes back when there is room.
float s_zfit = 1.0f;
float s_alt_px = 0.0f;  // altitude scale: screen pixels per foot (fitted to the traffic)
unsigned long s_last_frame_ms = 0;
unsigned long s_prefs_dirty_ms = 0;  // view changed by the arrows, not yet saved
float s_dt = 0.2f;  // seconds since the previous frame (clamped)

/** Fraction to move towards a target this frame for an exponential ease with time
 *  constant tau seconds: the same speed at any frame rate. */
float ease(float tau) { return 1.0f - expf(-s_dt / tau); }

float s_yaw_target = 20.0f;
float s_pitch_cur = -1.0f;  // camera tilt (deg), fractional while dragging; < 0: from prefs
bool s_dragging = false;    // a finger is turning the map
float s_yaw_vel = 0.0f;     // deg/s the map keeps turning after the finger lifts
// While an aircraft the user tapped is selected, the camera turns to keep it on the
// screen's centre line, in front of home. Deselecting returns to the saved heading.
bool s_follow = false;
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
size_t s_item_count = 0;
float s_item_size[services::adsb::kMaxAircraft];  // icon size (px) of each drawn aircraft
// 5" board: the frame is an 8-bit palette sprite (256 colours) in PSRAM. Indices 0-15
// are the palette above (all drawing code keeps using them); the rest hold shading
// ramps: smooth hill shading with elevation tints, sea haze and 3D aircraft.
constexpr bool kPlane3d = true;
constexpr uint8_t kTerrainBase = 16;   // 8 elevation bands x 16 shades
constexpr int kTerrainShades = 16;
constexpr int kTerrainBands = 8;
constexpr uint8_t kSeaBase = 144;      // 16 sea tones, far (hazy) .. near (deep)
constexpr uint8_t kPlaneBase = 160;    // 3 altitude colours x 24 shades
constexpr int kPlaneShades = 24;
constexpr uint8_t kAsphalt = 232;      // runway surface
constexpr uint8_t kMarking = 233;      // runway markings
constexpr uint8_t kUrbanBase = 234;    // built-up areas: 8 shades (234..241)
constexpr uint8_t kForestBase = 242;   // forest and woodland: 8 shades (242..249)
struct LabelPos {
  int x, y, size;
};
LabelPos s_label_pos[services::adsb::kMaxAircraft];

// ---------------------------------------------------------------- projection

Cam makeCam() {
  Cam c;
  const float yaw = s_yaw_cur * kDegToRad;
  if (s_pitch_cur < 0.0f) s_pitch_cur = services::display_prefs::get().pitch_deg;
  c.pitch_deg = std::max<float>(kPitchMinDeg, std::min<float>(kPitchMaxDeg, s_pitch_cur));
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
  c.zscale = 0.0003048f * static_cast<float>(services::display_prefs::get().exag) * (c.R / 50.0f) * s_zfit;
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
// ---------------------------------------------------------------- outlined blits
//
// Aircraft models and labels have a one-pixel dark outline. Drawing it as four
// offset copies (4 x the triangles, 4 x the text) made the aircraft the slowest part
// of a still frame. Instead each is drawn once into a small scratch buffer (0 =
// empty) and copied to the frame, the outline added on the way (empty pixels next to
// a drawn one).
uint8_t* scratchBuf(size_t bytes) {
  static uint8_t* buf = nullptr;
  static size_t cap = 0;
  if (cap < bytes) {
    free(buf);
#if defined(RADAR3D_HOST_TEST)
    buf = static_cast<uint8_t*>(malloc(bytes));
#else
    buf = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
#endif
    cap = buf ? bytes : 0;
  }
  return buf;
}

/** Copy a w x h scratch image (stride w) to the frame at (x, y), outlining it in kBg.
 *  The scratch has a one-pixel empty border for the outline. */
void blitOutlined(const uint8_t* src, int w, int h, int x, int y, int clip_top) {
  uint8_t* frame = s_band_mem;
  for (int r = 0; r < h; ++r) {
    const int fy = y + r;
    if (fy < clip_top || fy >= kH) continue;
    const uint8_t* row = src + r * w;
    const uint8_t* up = r > 0 ? row - w : nullptr;
    const uint8_t* dn = r + 1 < h ? row + w : nullptr;
    uint8_t* out = frame + static_cast<size_t>(fy) * kW;
    const int c0 = std::max(0, -x), c1 = std::min(w, kW - x);
    for (int c = c0; c < c1; ++c) {
      const uint8_t v = row[c];
      if (v) {
        out[x + c] = v;
      } else if ((c > 0 && row[c - 1]) || (c + 1 < w && row[c + 1]) || (up && up[c]) || (dn && dn[c])) {
        out[x + c] = kBg;
      }
    }
  }
}

/** Filled triangle into a scratch buffer (inclusive spans, like LovyanGFX's). */
void scratchTriangle(uint8_t* buf, int w, int h, int x0, int y0, int x1, int y1, int x2, int y2, uint8_t col) {
  if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
  if (y2 < y0) { std::swap(x0, x2); std::swap(y0, y2); }
  if (y2 < y1) { std::swap(x1, x2); std::swap(y1, y2); }
  auto span = [&](int y, int a, int b) {
    if (y < 0 || y >= h) return;
    if (a > b) std::swap(a, b);
    a = std::max(a, 0);
    b = std::min(b, w - 1);
    if (a <= b) memset(buf + y * w + a, col, b - a + 1);
  };
  if (y0 == y2) {
    span(y0, std::min({x0, x1, x2}), std::max({x0, x1, x2}));
    return;
  }
  for (int y = y0; y <= y2; ++y) {
    const int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
    int xb;
    if (y < y1 || (y == y1 && y1 == y0)) {
      xb = y1 == y0 ? x1 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    } else {
      xb = y2 == y1 ? x2 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
    }
    span(y, xa, xb);
  }
}

LGFX_Sprite s_txt;  // scratch for one line of label text (palette index 1 on 0)
bool s_txt_ok = false;

void haloText(const char* t, int x, int y, uint8_t color) {
  const int tw = s_spr.textWidth(t), th = s_spr.fontHeight();
  if (!s_txt_ok) {
    s_txt.setColorDepth(8);
    s_txt.setPsram(true);
    s_txt_ok = s_txt.createSprite(kW / 2, S(40)) != nullptr;
    if (s_txt_ok) s_txt.createPalette();
  }
  if (s_txt_ok && tw + 2 <= s_txt.width() && th + 2 <= s_txt.height() && tw > 0) {
    s_txt.setFont(s_spr.getFont());
    s_txt.setTextSize(1);
    s_txt.setTextDatum(textdatum_t::top_left);
    s_txt.fillRect(0, 0, tw + 2, th + 2, 0);
    s_txt.setTextColor(1);
    s_txt.drawString(t, 1, 1);
    // Scratch rows are s_txt.width() apart: copy the used part into a packed block.
    const int w = tw + 2, h = th + 2;
    uint8_t* sb = scratchBuf(static_cast<size_t>(w) * h);
    if (sb != nullptr) {
      const uint8_t* src = static_cast<const uint8_t*>(s_txt.getBuffer());
      for (int r = 0; r < h; ++r) {
        const uint8_t* in = src + r * s_txt.width();
        uint8_t* o = sb + r * w;
        for (int c = 0; c < w; ++c) o[c] = in[c] ? color : 0;
      }
      blitOutlined(sb, w, h, x - 1, y - 1, 0);
      return;
    }
  }
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
      {0, 0, kW, kTopBarH + S(2)},
      {0, kTopBarH, s_selected[0] ? 0 : kW, kStripH + S(2)},       // legend strip

      {S(2), kInfoY - S(2), (s_selected[0] && !infoInLabel()) ? kInfoLW + S(4) : 0, kInfoH + S(4)},  // info
      {kInfoRX - S(2), kInfoY - S(2), (s_selected[0] && !infoInLabel()) ? kInfoRW + S(4) : 0, kInfoH + S(4)},
      {ui::kSwitchX - S(2), ui::kSwitchY - S(2), ui::kSwitchW + S(4), ui::kSwitchH + S(4)},  // HAVA
      {ui::kRadarWaterX - S(2), ui::kWaterBtnY - S(2), ui::kWaterBtnW + S(4), ui::kWaterBtnH + S(4)},  // SU
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

// ---------------------------------------------------------------- places
// Points of interest and runways from the map download (map blob entry types 5 and
// 4, written by the setup page for the 5" board). Kept quiet so they read as
// background: airports as an airport badge with their three-letter code over their
// runways, landmarks as a small diamond, district names as dim text only. Drawn on
// the ground before the aircraft; names only where aircraft labels leave room.
// Built-up grid (see urbanAt).
uint8_t s_urban[64 * 64];
int s_urban_n = 0;
float s_urban_r = 0.0f, s_urban_ox = 0.0f, s_urban_oy = 0.0f;
// Forest grid (map entry type 7, same layout as the built-up grid).
uint8_t s_forest[64 * 64];
int s_forest_n = 0;
float s_forest_r = 0.0f, s_forest_ox = 0.0f, s_forest_oy = 0.0f;

struct Poi {
  float x, y;     // km from home
  uint8_t kind;   // 0 airport, 1 landmark, 2 district / town
  uint8_t cat;    // landmark category (see drawLandmarkIcon); 0 = generic
  char name[24];
};
struct Runway {
  float x0, y0, x1, y1;  // centre line ends, km from home
};
Poi s_pois[24];
size_t s_poi_count = 0;
// Where each landmark symbol was drawn (centre of the badge), for taps.
int16_t s_poi_sx[24], s_poi_sy[24];
bool s_poi_vis[24];
int s_poi_open = -1;                  // landmark whose name is shown (tapped)
unsigned long s_poi_open_until = 0;
Runway s_rwys[16];
size_t s_rwy_count = 0;
uint32_t s_poi_gen = 0xFFFFFFFFu;

void loadPois() {
#if defined(RADAR3D_HOST_TEST) && defined(POI_PREVIEW)
  if (s_poi_gen == 1) return;
  s_poi_gen = 1;
  const Poi sample[] = {{-3, 27, 0, 0, "IST"},  {14, -6, 0, 0, "ISL"},   {19, 9, 1, 3, "Kule"},
                        {9, -13, 1, 5, "Köprü"}, {-24, 34, 1, 7, "Park"}, {-16, -10, 1, 1, "Stad"},
                        {26, -22, 1, 2, "Zoo"},  {-8, 6, 1, 4, "Kale"},   {30, 14, 1, 6, "Müze"},
                        {-30, 14, 1, 8, "Cami"}, {-36, -18, 1, 0, "Diğer"}};
  s_poi_count = sizeof(sample) / sizeof(sample[0]);
  memcpy(s_pois, sample, sizeof(sample));
  const Runway rw[] = {{-4.2f, 29.6f, -3.6f, 25.2f}, {-2.4f, 29.8f, -1.8f, 25.4f}, {-6.0f, 28.5f, -5.5f, 24.8f},
                       {12.4f, -7.0f, 15.6f, -5.0f}};
  s_rwy_count = sizeof(rw) / sizeof(rw[0]);
  memcpy(s_rwys, rw, sizeof(rw));
  {  // sample built-up area: a town around home stretching north-east
    s_urban_n = 32;
    s_urban_r = ui::radar::rangeCurrent().outer_km;
    for (int j = 0; j < 32; ++j)
      for (int i = 0; i < 32; ++i) {
        const float x = (i - 15.5f) / 16.0f, y = (j - 15.5f) / 16.0f;
        const float d = hypotf(x - 0.15f, (y - 0.25f) * 1.6f);
        s_urban[j * 32 + i] = static_cast<uint8_t>(std::max(0.0f, std::min(15.0f, (0.45f - d) * 60.0f)));
      }
    // sample forest: a wooded belt to the north-west
    s_forest_n = 32;
    s_forest_r = s_urban_r;
    for (int j = 0; j < 32; ++j)
      for (int i = 0; i < 32; ++i) {
        const float x = (i - 15.5f) / 16.0f, y = (j - 15.5f) / 16.0f;
        const float d = hypotf((x + 0.45f) * 1.3f, y - 0.55f);
        s_forest[j * 32 + i] = static_cast<uint8_t>(std::max(0.0f, std::min(15.0f, (0.38f - d) * 70.0f)));
      }
  }
  s_poi_open = 2;  // preview: the tower's name as if it had been tapped
  s_poi_open_until = 0xFFFFFFFFul;
  const float k = ui::radar::rangeCurrent().outer_km / 50.0f;  // sample laid out for 50 km
  for (size_t i = 0; i < s_poi_count; ++i) { s_pois[i].x *= k; s_pois[i].y *= k; }
  for (size_t i = 0; i < s_rwy_count; ++i) {
    // keep real runway length (~4 km) while moving the airport with the scale
    Runway& r = s_rwys[i];
    const float mx = 0.5f * (r.x0 + r.x1), my = 0.5f * (r.y0 + r.y1);
    r.x0 += mx * (k - 1); r.x1 += mx * (k - 1); r.y0 += my * (k - 1); r.y1 += my * (k - 1);
  }
#else
  const uint32_t gen = services::map::generation();
  if (gen == s_poi_gen) return;
  s_poi_gen = gen;
  s_poi_count = s_rwy_count = 0;
  s_urban_n = 0;
  s_forest_n = 0;
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
  size_t pos = services::map::kHeaderBytes;
  for (uint16_t pl = 0; pl < count && pos + 3 <= size; ++pl) {
    const uint8_t type = m[pos];
    uint16_t n;
    memcpy(&n, m + pos + 1, 2);
    pos += 3;
    if (pos + static_cast<size_t>(n) * 4 > size) return;
    const uint8_t* e = m + pos;
    pos += static_cast<size_t>(n) * 4;
    auto pt = [&](int k, float* x, float* y) {
      int16_t xi, yi;
      memcpy(&xi, e + k * 4, 2);
      memcpy(&yi, e + k * 4 + 2, 2);
      *x = ox + xi * 0.01f;
      *y = oy + yi * 0.01f;
    };
    if (type == 4 && n >= 2 && s_rwy_count < sizeof(s_rwys) / sizeof(s_rwys[0])) {
      Runway& r = s_rwys[s_rwy_count++];
      pt(0, &r.x0, &r.y0);
      pt(n - 1, &r.x1, &r.y1);
    } else if ((type == 6 || type == 7) && n >= 2) {
      // Built-up (6) / forest (7) grid: word 0 = [N, 0, radius x10 (u16)], then N*N
      // nibbles (low first), rows south to north, cells west to east.
      const int gn = e[0];
      uint16_t r10;
      memcpy(&r10, e + 2, 2);
      if (gn > 0 && gn <= 64 && static_cast<size_t>((gn * gn + 1) / 2) <= (static_cast<size_t>(n) - 1) * 4) {
        uint8_t* g = type == 6 ? s_urban : s_forest;
        (type == 6 ? s_urban_n : s_forest_n) = gn;
        (type == 6 ? s_urban_r : s_forest_r) = r10 / 10.0f;
        (type == 6 ? s_urban_ox : s_forest_ox) = ox;
        (type == 6 ? s_urban_oy : s_forest_oy) = oy;
        for (int k = 0; k < gn * gn; ++k) g[k] = (e[4 + k / 2] >> ((k & 1) * 4)) & 0x0F;
      }
    } else if (type == 5 && n >= 2 && s_poi_count < sizeof(s_pois) / sizeof(s_pois[0])) {
      // word 0: position; word 1: kind, name length; then the UTF-8 name.
      Poi& p = s_pois[s_poi_count];
      pt(0, &p.x, &p.y);
      p.kind = e[4];
      p.cat = e[6];
      const size_t len = std::min<size_t>({e[5], sizeof(p.name) - 1, (static_cast<size_t>(n) - 2) * 4});
      memcpy(p.name, e + 8, len);
      p.name[len] = '\0';
      if (p.kind <= 2 && len > 0) ++s_poi_count;
    }
  }
#endif
}

// ---------------------------------------------------------------- layers


// ---------------------------------------------------------------- sea fill
//
// The map blob's type-2 rings are sea polygons (even-odd). They are projected
// once per camera change and filled scanline by scanline, clipped to the
// projected outer ring so the sea stays on the radar disc.

constexpr size_t kMaxRings = 128;
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
  // Each rim edge is walked over the rows it crosses (testing all 144 edges on all
  // 480 rows took several ms on every camera move).
  static float span_lo[kH], span_hi[kH];
  for (int y = 0; y < kH; ++y) { span_lo[y] = 1e9f; span_hi[y] = -1e9f; }
  for (int i = 0; i < kSeg; ++i) {
    const int j = (i + 1) % kSeg;
    float x0 = px[i], y0 = py[i], x1 = px[j], y1 = py[j];
    if (y0 == y1) continue;
    if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
    // rows whose centre lies in [y0, y1)
    const int r0 = std::max(0, static_cast<int>(ceilf(y0 - 0.5f)));
    const int r1 = std::min(kH - 1, static_cast<int>(ceilf(y1 - 0.5f)) - 1);
    const float dx = (x1 - x0) / (y1 - y0);
    for (int y = r0; y <= r1; ++y) {
      const float x = x0 + (y + 0.5f - y0) * dx;
      span_lo[y] = std::min(span_lo[y], x);
      span_hi[y] = std::max(span_hi[y], x);
    }
  }
  for (int y = 0; y < kH; ++y) {
    const float lo = span_lo[y], hi = span_hi[y];
    // Kept on screen: the disc's tips may run past the edges, and a span starting at a
    // negative x would write into the row before (or before the band buffer).
    s_disc_l[y] = lo > hi ? 1 : static_cast<int16_t>(std::max(0.0f, ceilf(lo)));
    s_disc_r[y] = lo > hi ? 0 : static_cast<int16_t>(std::min<float>(kW - 1, floorf(hi)));
    if (s_disc_l[y] > s_disc_r[y]) { s_disc_l[y] = 1; s_disc_r[y] = 0; }
  }
}

/**
 * Project all map rings of one type (2 sea, 3 lakes) into a ring set, keeping only
 * the points the fill needs: points closer than ~1.5 px to the previous one are
 * dropped, and runs of points that all lie beyond the same screen edge collapse to
 * their ends (a straight edge between them stays beyond that edge, so the filled
 * area on screen is unchanged). Two passes: count, then allocate exactly and fill.
 */
bool buildRings(const Cam& c, uint8_t want, bool draped, RingSet* set) {
  set->points = 0;
  set->count = 0;
  const uint8_t* m = services::map::data();
  const size_t size = services::map::size();
  if (m == nullptr || size < services::map::kHeaderBytes) return true;
  int32_t lat_e7, lon_e7;
  uint16_t count;
  memcpy(&lat_e7, m + 4, 4);
  memcpy(&lon_e7, m + 8, 4);
  memcpy(&count, m + 12, 2);
  float ox, oy;
  offsetKm(lat_e7 / 1e7f, lon_e7 / 1e7f, &ox, &oy);
  const float near_y = -(c.D - 0.3f * c.R);

  auto outcode = [](int x, int y) {
    return (x < -8 ? 1 : 0) | (x > kW + 8 ? 2 : 0) | (y < kTopBarH - 8 ? 4 : 0) | (y > kH + 8 ? 8 : 0);
  };

  for (int pass = 0; pass < 2; ++pass) {
    const bool write = pass == 1;
    size_t total = 0;
    set->points = 0;
    set->count = 0;
    size_t pos = services::map::kHeaderBytes;
    for (uint16_t pl = 0; pl < count && pos + 3 <= size; ++pl) {
      const uint8_t type = m[pos];
      uint16_t n;
      memcpy(&n, m + pos + 1, 2);
      pos += 3;
      if (pos + static_cast<size_t>(n) * 4 > size) return true;
      if (type != want || n < 3 || set->count >= kMaxRings) {
        pos += static_cast<size_t>(n) * 4;
        continue;
      }
      // Ring state: committed points, plus one pending point that may still be merged.
      size_t ring_pts = 0;
      const size_t start = write ? set->points : 0;
      int16_t ymin = 32767, ymax = -32768;
      bool have_last = false, have_pend = false;
      int16_t lx = 0, ly = 0, px = 0, py = 0;
      auto commit = [&](int16_t x, int16_t y) {
        if (write) {
          if (set->points >= set->cap) return;
          set->xy[set->points * 2] = x;
          set->xy[set->points * 2 + 1] = y;
          ++set->points;
          ymin = std::min(ymin, y);
          ymax = std::max(ymax, y);
        }
        ++ring_pts;
        lx = x;
        ly = y;
        have_last = true;
      };
      auto add = [&](int16_t x, int16_t y) {
        if (have_last && !have_pend && std::abs(x - lx) + std::abs(y - ly) < 2) return;
        if (have_pend) {
          if (have_last && (outcode(lx, ly) & outcode(px, py) & outcode(x, y)) != 0) {
            px = x;  // pending point is redundant: replace it
            py = y;
            return;
          }
          commit(px, py);
        }
        px = x;
        py = y;
        have_pend = true;
      };
      auto emit = [&](float xr, float yr, float z) {
        float sx = 0, sy = 0;
        projectCam(c, xr, yr, z, &sx, &sy);
        sx = std::max(-2000.0f, std::min(2480.0f, sx));
        sy = std::max(-2000.0f, std::min(2320.0f, sy));
        add(static_cast<int16_t>(ri(sx)), static_cast<int16_t>(ri(sy)));
      };
      auto toCam = [&](size_t at, float* xr, float* yr, float* z) {
        int16_t xi, yi;
        memcpy(&xi, m + at, 2);
        memcpy(&yi, m + at + 2, 2);
        const float x = ox + xi * 0.01f, y = oy + yi * 0.01f;
        *xr = x * c.cyaw - y * c.syaw;
        *yr = x * c.syaw + y * c.cyaw;
        *z = draped ? groundZ(c, x, y) : 0.0f;
      };
      // Clip against a plane in front of the camera (Sutherland-Hodgman) so far points
      // behind the viewer do not fold the polygon over the disc.
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
      if (have_pend) commit(px, py);
      pos += static_cast<size_t>(n) * 4;
      if (ring_pts < 3) {
        if (write) set->points = start;
        continue;
      }
      total += ring_pts;
      if (write) {
        set->start[set->count] = static_cast<uint16_t>(start);
        set->len[set->count] = static_cast<uint16_t>(set->points - start);
        set->ymin[set->count] = ymin;
        set->ymax[set->count] = ymax;
      }
      ++set->count;
    }
    if (!write) {
      set->count = 0;
      if (total == 0) return true;
      // Grow with headroom and keep the buffer: while the view turns, the point count
      // changes a little every frame, and re-allocating each time fragments the heap
      // that the Wi-Fi TLS connection needs. Only a lot of slack is given back.
      if (total > set->cap || total * 3 + 1024 < set->cap) {
        free(set->xy);
        const size_t want_cap = total + total / 4 + 64;
        set->xy = static_cast<int16_t*>(malloc(want_cap * 2 * sizeof(int16_t)));
        set->cap = set->xy ? want_cap : 0;
        if (set->xy == nullptr) return false;  // not enough RAM right now: retry next frame
      }
    }
  }
  return true;
}

/** Scanline even-odd fill of a ring set, clipped to the radar disc. */
int s_disc_top = 0, s_disc_bottom = 0;  // screen rows the disc covers (sea gradient)

// Scanline fill with an edge table: each edge is set up once and only the edges
// crossing the current row are visited (testing every edge on every row cost ~100 ms
// per frame with a detailed coastline).
struct FillEdge {
  int16_t r0, r1;  // first / last row it crosses
  float x, dx;     // x at row r0 (pixel centre), change per row
};
// One fill in progress: edges set up once per frame, rows filled band by band in
// increasing y (the active edge list carries over from one band to the next).
struct FillState {
  FillEdge* edges = nullptr;
  uint16_t* order = nullptr;
  size_t cap = 0;
  uint16_t bucket[kH + 1];
  size_t ne = 0, next = 0;
  int na = 0, y = 0;
  uint16_t active[256];
  uint16_t fillpos[kH + 1];  // scratch while sorting (per fill: both cores set up fills)
  uint8_t color = 0;
};
FillState s_fill_sea, s_fill_lake;
FillState s_fill_sea2, s_fill_lake2;  // the second core's share

void fillBegin(FillState& st, const RingSet& set, uint8_t color) {
  st.ne = 0;
  st.na = 0;
  st.y = kTopBarH;
  st.color = color;
  if (set.count == 0 || set.points == 0) return;
  if (st.cap < set.points) {
    free(st.edges);
    free(st.order);
    st.cap = set.points + 256;
    st.edges = static_cast<FillEdge*>(malloc(st.cap * sizeof(FillEdge)));
    st.order = static_cast<uint16_t*>(malloc(st.cap * sizeof(uint16_t)));
    if (st.edges == nullptr || st.order == nullptr || st.cap > 65535) {
      free(st.edges);
      free(st.order);
      st.edges = nullptr;
      st.order = nullptr;
      st.cap = 0;
      return;
    }
  }
  // Edges, bucketed by their first row (counting sort).
  size_t ne = 0;
  memset(st.bucket, 0, sizeof(st.bucket));
  for (size_t r = 0; r < set.count; ++r) {
    const int16_t* p = set.xy + set.start[r] * 2;
    const int len = set.len[r];
    for (int i = 0; i < len; ++i) {
      const int j = (i + 1 == len) ? 0 : i + 1;
      float x0 = p[i * 2], y0 = p[i * 2 + 1], x1 = p[j * 2], y1 = p[j * 2 + 1];
      if (y0 == y1) continue;
      if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
      // Rows whose centre ys = r + 0.5 lies in [y0, y1).
      int r0 = static_cast<int>(ceilf(y0 - 0.5f)), r1 = static_cast<int>(ceilf(y1 - 0.5f)) - 1;
      r0 = std::max(r0, kTopBarH);
      r1 = std::min(r1, kH - 1);
      if (r1 < r0) continue;
      const float dx = (x1 - x0) / (y1 - y0);
      st.edges[ne] = {static_cast<int16_t>(r0), static_cast<int16_t>(r1), x0 + (r0 + 0.5f - y0) * dx, dx};
      ++st.bucket[r0];
      ++ne;
    }
  }
  if (ne < 2) return;
  {  // bucket start offsets, then place the edges
    uint16_t sum = 0;
    for (int y = 0; y <= kH; ++y) { const uint16_t c = st.bucket[y]; st.bucket[y] = sum; sum += c; }
    memcpy(st.fillpos, st.bucket, sizeof(st.fillpos));
    for (size_t e = 0; e < ne; ++e) st.order[st.fillpos[st.edges[e].r0]++] = static_cast<uint16_t>(e);
  }
  st.ne = ne;
  st.next = st.bucket[kTopBarH];
}

/** As fillBegin, but ready to fill from row ystart on (edges already under way are
 *  advanced to it): the second core starts its share of the ground in mid screen. */
void fillBeginAt(FillState& st, const RingSet& set, uint8_t color, int ystart) {
  fillBegin(st, set, color);
  if (st.ne < 2 || ystart <= kTopBarH) return;
  st.na = 0;
  for (size_t k = 0; k < st.bucket[std::min(ystart, kH)]; ++k) {
    FillEdge& e = st.edges[st.order[k]];
    if (e.r1 < ystart) continue;  // finished above
    e.x += (ystart - e.r0) * e.dx;
    if (st.na < 256) st.active[st.na++] = st.order[k];
  }
  st.next = st.bucket[std::min(ystart, kH)];
  st.y = ystart;
}

/** Fill rows st.y .. y_end - 1 into a band buffer whose first row is y0. */
void fillRows(FillState& st, int y_end, uint8_t* band, int y0) {
  if (st.ne < 2) return;
  constexpr int kMaxActive = 256;
  float xs[kMaxActive];
  for (; st.y < y_end; ++st.y) {
    const int y = st.y;
    const size_t end = st.bucket[y + 1 <= kH ? y + 1 : kH];
    for (; st.next < end && st.next < st.ne; ++st.next) {
      if (st.na < kMaxActive) st.active[st.na++] = st.order[st.next];
    }
    int w = 0;
    for (int k = 0; k < st.na; ++k) {
      if (st.edges[st.active[k]].r1 >= y) st.active[w++] = st.active[k];
    }
    st.na = w;
    if (st.na < 2) continue;
    const bool in_disc = s_disc_l[y] <= s_disc_r[y];
    int n = 0;
    for (int k = 0; k < st.na; ++k) {
      FillEdge& e = st.edges[st.active[k]];
      xs[n++] = e.x;
      e.x += e.dx;  // ready for the next row
    }
    if (!in_disc) continue;
    for (int i = 1; i < n; ++i) {  // insertion sort, n is small
      const float v = xs[i];
      int k = i;
      while (k > 0 && xs[k - 1] > v) { xs[k] = xs[k - 1]; --k; }
      xs[k] = v;
    }
    uint8_t col = st.color;
    if (st.color == kSea && s_disc_bottom > s_disc_top) {  // haze towards the horizon
      const int t = (y - s_disc_top) * 15 / (s_disc_bottom - s_disc_top);
      col = static_cast<uint8_t>(kSeaBase + std::max(0, std::min(15, t)));
    }
    uint8_t* row = band + static_cast<size_t>(y - y0) * kW;
    for (int k = 0; k + 1 < n; k += 2) {
      const int xa = std::max<int>(static_cast<int>(ceilf(xs[k])), s_disc_l[y]);
      const int xb = std::min<int>(static_cast<int>(floorf(xs[k + 1])), s_disc_r[y]);
      if (xb >= xa) memset(row + xa, col, xb - xa + 1);
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
int16_t s_rc_ymin[kReliefN * kReliefN];  // screen rows each cell covers (band culling)
int16_t s_rc_ymax[kReliefN * kReliefN];
uint8_t s_rv_shade[(kReliefN + 1) * (kReliefN + 1)];  // 0 dark .. 128 flat .. 255 lit
uint8_t s_rv_elev[(kReliefN + 1) * (kReliefN + 1)];   // 0 sea level .. 255 highest point
int8_t s_rv_urban[(kReliefN + 1) * (kReliefN + 1)];  // land cover: -127 forest .. 0 open .. 127 built-up
uint16_t s_rc_count = 0;

// Grid vertices that lie in the sea (map type 2 rings, world coordinates). The terrain
// grid is coarse, so without this the relief cells would spill over the coastline.
// Depends only on the range, the map and the location, not on the camera angle.
uint8_t s_sea_vertex_bits[((kReliefN + 1) * (kReliefN + 1) + 7) / 8];
inline bool seaVertex(int k) { return (s_sea_vertex_bits[k >> 3] >> (k & 7)) & 1; }
inline void toggleSeaVertex(int k) { s_sea_vertex_bits[k >> 3] ^= static_cast<uint8_t>(1u << (k & 7)); }
struct MaskKey {
  uint32_t gen;
  float R;
  double lat, lon;
} s_mask_key = {0xFFFFFFFFu, -1, 0, 0};

void buildSeaMask(float R) {
  constexpr int N = kReliefN;
  memset(s_sea_vertex_bits, 0, sizeof(s_sea_vertex_bits));
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
        for (int i = i0; i <= N; ++i) toggleSeaVertex(j * (N + 1) + i);
      }
    }
    pos += static_cast<size_t>(n) * 4;
  }
}

// Vertices too low to carry relief (sea level / shore), same grid as the sea mask.
uint8_t s_low_vertex_bits[((kReliefN + 1) * (kReliefN + 1) + 7) / 8];
inline bool lowVertex(int k) { return (s_low_vertex_bits[k >> 3] >> (k & 7)) & 1; }

// Heights and shading depend only on the range, the terrain grid and the location --
// not on the camera -- so they are worked out once per change and kept. Turning the
// view then only re-projects the vertices: no memory is allocated while rotating
// (with Wi-Fi up, the network task needs ~40 KB for TLS at the same moment, and a
// failed allocation here used to leave the relief out until the next camera change).
// Built-up areas (map entry type 6 from the setup page): a coarse grid of how much of
// each cell is town (0..15). The relief is tinted towards a grey-brown there, so the
// city reads at a glance without any names.

/** 0..1 built-up share at a ground point (km from home), bilinear. */
float gridAt(const uint8_t* g, int n, float r, float ox, float oy, float x, float y) {
  if (n == 0) return 0.0f;
  const float cell = 2.0f * r / n;
  const float fx = (x - ox + r) / cell - 0.5f, fy = (y - oy + r) / cell - 0.5f;
  const int i0 = static_cast<int>(floorf(fx)), j0 = static_cast<int>(floorf(fy));
  const float tx = fx - i0, ty = fy - j0;
  auto at = [&](int i, int j) {
    if (i < 0 || j < 0 || i >= n || j >= n) return 0.0f;
    return g[j * n + i] / 15.0f;
  };
  const float v = (at(i0, j0) * (1 - tx) + at(i0 + 1, j0) * tx) * (1 - ty) +
                  (at(i0, j0 + 1) * (1 - tx) + at(i0 + 1, j0 + 1) * tx) * ty;
  // Faded out towards the edge of the grid's circle: no square border of town or forest.
  const float dx = x - ox, dy = y - oy, d2 = dx * dx + dy * dy, r_in = r * 0.85f;
  if (d2 >= r * r) return 0.0f;
  if (d2 <= r_in * r_in) return v;
  const float t = (r - sqrtf(d2)) / (r - r_in);
  return v * t * t * (3.0f - 2.0f * t);
}
float urbanAt(float x, float y) { return gridAt(s_urban, s_urban_n, s_urban_r, s_urban_ox, s_urban_oy, x, y); }
float forestAt(float x, float y) { return gridAt(s_forest, s_forest_n, s_forest_r, s_forest_ox, s_forest_oy, x, y); }

void loadPois();

struct ShadeKey {
  uint32_t mgen;
  uint32_t tgen;
  float R, tscale;
  double lat, lon;
} s_shade_key = {0, 0, -1, -1, 0, 0};

void buildShade(const Cam& c) {
  constexpr int N = kReliefN;
  const float step = 2.0f * c.R / N;
  memset(s_low_vertex_bits, 0, sizeof(s_low_vertex_bits));
  auto h = [&](int i, int j) {
    return services::terrain::heightM(-c.R + i * step, -c.R + j * step);
  };
  // Light from the north-west, a bit above the horizon (world frame: x east, y north).
  // Shading is computed per vertex (central differences) so it can be blended smoothly
  // across each cell when drawing.
  const float lx = -0.55f, ly = 0.55f, lz = 0.63f;
  constexpr float kGain = 2.2f;  // exaggerate the contrast so gentle hills still read
  for (int j = 0; j <= N; ++j) {
    for (int i = 0; i <= N; ++i) {
      const int k = j * (N + 1) + i;
      if (h(i, j) < 1.0f) s_low_vertex_bits[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
      const int il = std::max(i - 1, 0), ir = std::min(i + 1, N);
      const int jd = std::max(j - 1, 0), ju = std::min(j + 1, N);
      const float dzdx = (h(ir, j) - h(il, j)) * c.tscale / ((ir - il) * step);
      const float dzdy = (h(i, ju) - h(i, jd)) * c.tscale / ((ju - jd) * step);
      const float inv = 1.0f / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.0f);
      const float lit = (-dzdx * lx - dzdy * ly + lz) * inv;
      float v = kGain * (lit - lz) * 255.0f;
      // Dead zone: nearly flat ground stays the plain land colour (no dither speckle).
      constexpr float kFlat = 14.0f;
      v = v > kFlat ? v - kFlat : (v < -kFlat ? v + kFlat : 0.0f);
      v += 128.0f;
      s_rv_shade[k] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v)));
      const float hmax = std::max(60.0f, services::terrain::maxHeightM());
      s_rv_elev[k] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, h(i, j) / hmax * 255.0f)));
      {  // town wins over forest where both are mapped
        const float ux = -c.R + i * step, uy = -c.R + j * step;
        const float u = urbanAt(ux, uy);
        // Forest share counts more: a half wooded cell already reads as forest.
        const float cover = u > 0.15f ? u : -std::min(1.0f, forestAt(ux, uy) * 1.6f);
        s_rv_urban[k] = static_cast<int8_t>(std::max(-127.0f, std::min(127.0f, cover * 127.0f)));
      }
    }
  }
}

float s_sort_syaw = 0.0f, s_sort_cyaw = 1.0f;  // camera heading, only while sorting

void buildRelief(const Cam& c) {
  s_rc_count = 0;
  if (!services::terrain::available()) return;
  constexpr int N = kReliefN;
  constexpr int16_t kOff = INT16_MIN;  // s_rv_x marker: vertex behind the camera
  const float step = 2.0f * c.R / N;
  const MaskKey mk{services::map::generation(), c.R, services::location::lat(), services::location::lon()};
  if (mk.gen != s_mask_key.gen || mk.R != s_mask_key.R || mk.lat != s_mask_key.lat ||
      mk.lon != s_mask_key.lon) {
    s_mask_key = mk;
    buildSeaMask(c.R);
  }
  loadPois();  // the built-up grid comes with the map
  const ShadeKey sk{services::map::generation(), services::terrain::generation(), c.R, c.tscale,
                    services::location::lat(), services::location::lon()};
  if (sk.mgen != s_shade_key.mgen || sk.tgen != s_shade_key.tgen || sk.R != s_shade_key.R || sk.tscale != s_shade_key.tscale ||
      sk.lat != s_shade_key.lat || sk.lon != s_shade_key.lon) {
    s_shade_key = sk;
    buildShade(c);
  }
  for (int j = 0; j <= N; ++j) {
    for (int i = 0; i <= N; ++i) {
      const float x = -c.R + i * step, y = -c.R + j * step;
      const int k = j * (N + 1) + i;
      float sx = 0, sy = 0;
      if (!project(c, x, y, services::terrain::heightM(x, y) * c.tscale, &sx, &sy)) {
        s_rv_x[k] = kOff;
        continue;
      }
      s_rv_x[k] = static_cast<int16_t>(std::max(-2000.0f, std::min(2480.0f, sx)));
      s_rv_y[k] = static_cast<int16_t>(std::max(-2000.0f, std::min(2320.0f, sy)));
    }
  }
  const float r2 = c.R * c.R;
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const int k00 = j * (N + 1) + i, k10 = k00 + 1, k01 = k00 + N + 1, k11 = k01 + 1;
      const float x0 = -c.R + i * step, y0 = -c.R + j * step, x1 = x0 + step, y1 = y0 + step;
      // Cells reaching into the disc (the drawing is clipped to its rim).
      const float nx = std::max(x0, std::min(0.0f, x1)), ny = std::max(y0, std::min(0.0f, y1));
      if (nx * nx + ny * ny > r2) continue;  // wholly outside
      // Cells wholly at sea level are left out: flat land there looks the same as the
      // land fill underneath, sea is covered by the sea fill. With a map, the vector sea
      // (and lakes) are drawn over the relief, so the coastline comes from the map.
      auto gone = [&](int k) { return lowVertex(k) || (s_sea.count > 0 && seaVertex(k)); };
      const bool covered = s_rv_urban[k00] | s_rv_urban[k10] | s_rv_urban[k01] | s_rv_urban[k11];
      if (gone(k00) && gone(k10) && gone(k01) && gone(k11) && !covered) continue;
      // Flat, low and bare everywhere: the cell would come out as the plain land fill
      // underneath (band 0, middle shade; band 1 is within 2 % of it), so it is skipped.
      auto plain = [&](int k) { return s_rv_shade[k] == 128 && s_rv_elev[k] < 36 && s_rv_urban[k] == 0; };
      if (plain(k00) && plain(k10) && plain(k01) && plain(k11)) continue;
      if (s_rv_x[k00] == kOff || s_rv_x[k10] == kOff || s_rv_x[k01] == kOff || s_rv_x[k11] == kOff) continue;
      s_rc_order[s_rc_count++] = static_cast<uint16_t>(j * N + i);
      s_rc_ymin[j * N + i] = std::min(std::min(s_rv_y[k00], s_rv_y[k10]), std::min(s_rv_y[k01], s_rv_y[k11]));
      s_rc_ymax[j * N + i] = std::max(std::max(s_rv_y[k00], s_rv_y[k10]), std::max(s_rv_y[k01], s_rv_y[k11]));
    }
  }
  // Far cells first (painter's algorithm): forward distance of the cell centre in
  // the camera frame, computed from the cell index (no depth buffer needed).
  s_sort_syaw = c.syaw;
  s_sort_cyaw = c.cyaw;
  std::sort(s_rc_order, s_rc_order + s_rc_count, [](uint16_t a, uint16_t b) {
    const float da = (a % kReliefN) * s_sort_syaw + (a / kReliefN) * s_sort_cyaw;
    const float db = (b % kReliefN) * s_sort_syaw + (b / kReliefN) * s_sort_cyaw;
    return da > db;
  });
}

// 4x4 ordered-dither thresholds (0..15).
constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/**
 * Triangle with the shade and the elevation interpolated across it, into the 8-bit
 * frame: 16 shades x 8 elevation bands, a light ordered dither between neighbours so
 * no steps show.
 */
/** ceil() for screen coordinates, without the library call (it was a large share of
 *  the per-row cost). */
inline int ceilToInt(float v) {
  const int i = static_cast<int>(v);
  return i + (v > static_cast<float>(i) ? 1 : 0);
}

/**
 * One row of a relief triangle: shade, band and (with kCover) land cover stepped in
 * fixed point. Four pixels per round, so the four dither thresholds of the row stay
 * in registers.
 */
template <bool kCover>
inline void shadeSpan(uint8_t* row, int xs, int xe, int y, int32_t sv, int32_t ev, int32_t uv, int32_t dsv,
                      int32_t dev, int32_t duv) {
  const uint8_t* bay = kBayer[y & 3];
  const uint8_t* bay2 = kBayer[(y + 2) & 3];
  const uint8_t* bay3 = kBayer[(y + 1) & 3];
  auto px = [&](int x, int ts, int te, int tc) {
    const int32_t s16 = std::min<int32_t>(15 * 16, std::max<int32_t>(0, sv >> 8));  // level * 16
    const int32_t e16 = std::min<int32_t>(7 * 16, std::max<int32_t>(0, ev >> 8));
    int sh = (s16 >> 4) + ((s16 & 15) > ts ? 1 : 0);
    int bd = (e16 >> 4) + ((e16 & 15) > te ? 1 : 0);
    sh = std::min(sh, 15);
    bd = std::min(bd, 7);
    uint8_t out = static_cast<uint8_t>(kTerrainBase + bd * kTerrainShades + sh);
    if (kCover) {  // land cover share (x16, + town, - forest) against the dither
      int32_t cov = uv >> 12;
      const bool forest = cov < 0;
      if (forest) cov = -cov;
      if (cov > tc) out = static_cast<uint8_t>((forest ? kForestBase : kUrbanBase) + (sh >> 1));
    }
    row[x] = out;
    sv += dsv;
    ev += dev;
    if (kCover) uv += duv;
  };
  const int ts0 = bay[xs & 3], ts1 = bay[(xs + 1) & 3], ts2 = bay[(xs + 2) & 3], ts3 = bay[(xs + 3) & 3];
  const int te0 = bay2[(xs + 1) & 3], te1 = bay2[(xs + 2) & 3], te2 = bay2[(xs + 3) & 3], te3 = bay2[xs & 3];
  const int tc0 = bay3[(xs + 3) & 3], tc1 = bay3[xs & 3], tc2 = bay3[(xs + 1) & 3], tc3 = bay3[(xs + 2) & 3];
  int x = xs;
  for (; x + 3 <= xe; x += 4) {
    px(x, ts0, te0, tc0);
    px(x + 1, ts1, te1, tc1);
    px(x + 2, ts2, te2, tc2);
    px(x + 3, ts3, te3, tc3);
  }
  if (x <= xe) px(x, ts0, te0, tc0);
  if (x + 1 <= xe) px(x + 1, ts1, te1, tc1);
  if (x + 2 <= xe) px(x + 2, ts2, te2, tc2);
}

void shadeTriangle(uint8_t* buf, int clip_y0, int clip_y1, float x0, float y0, float s0, float e0, float u0,
                   float x1, float y1, float s1, float e1, float u1, float x2, float y2, float s2, float e2,
                   float u2) {
  if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); std::swap(s0, s1); std::swap(e0, e1); std::swap(u0, u1); }
  if (y2 < y0) { std::swap(x0, x2); std::swap(y0, y2); std::swap(s0, s2); std::swap(e0, e2); std::swap(u0, u2); }
  if (y2 < y1) { std::swap(x1, x2); std::swap(y1, y2); std::swap(s1, s2); std::swap(e1, e2); std::swap(u1, u2); }
  const bool urban = u0 != 0.0f || u1 != 0.0f || u2 != 0.0f;  // any land cover
  if (y2 - y0 < 0.5f) return;
  const int ya = std::max(std::max(kTopBarH + 1, clip_y0), ceilToInt(y0 - 0.5f));
  const int yb = std::min(clip_y1 - 1, ceilToInt(y2 - 0.5f) - 1);
  if (yb < ya) return;
  // Shade, band and cover are linear over the triangle: their steps along x and y are
  // worked out once here (one division), so each row costs a few multiply-adds instead
  // of the three divisions and four library calls it took before.
  const float ex1 = x1 - x0, ey1 = y1 - y0, ex2 = x2 - x0, ey2 = y2 - y0;
  const float den = ex1 * ey2 - ex2 * ey1;
  if (fabsf(den) < 1e-6f) return;  // no area
  const float id = 1.0f / den;
  const float dsdx = ((s1 - s0) * ey2 - (s2 - s0) * ey1) * id, dsdy = ((s2 - s0) * ex1 - (s1 - s0) * ex2) * id;
  const float dedx = ((e1 - e0) * ey2 - (e2 - e0) * ey1) * id, dedy = ((e2 - e0) * ex1 - (e1 - e0) * ex2) * id;
  const float dudx = ((u1 - u0) * ey2 - (u2 - u0) * ey1) * id, dudy = ((u2 - u0) * ex1 - (u1 - u0) * ex2) * id;
  // Fixed point (8.8, in 16ths of a level for the dither): the inner loop is integer
  // only. Shade is clamped to 0..15, band to 0..7.
  const int32_t dsv = static_cast<int32_t>(dsdx * 4096.0f), dev = static_cast<int32_t>(dedx * 4096.0f);
  const int32_t duv = static_cast<int32_t>(dudx * 4096.0f * 16.0f);
  const float k02 = ex2 / ey2;
  const bool flat01 = !(ey1 > 1e-3f), flat12 = !((y2 - y1) > 1e-3f);
  const float k01 = flat01 ? 0.0f : ex1 / ey1;
  const float k12 = flat12 ? 0.0f : (x2 - x1) / (y2 - y1);
  for (int y = ya; y <= yb; ++y) {
    const float yc = y + 0.5f, py = yc - y0;
    float xa = x0 + py * k02;
    float xb = yc < y1 ? (flat01 ? x1 : x0 + py * k01) : (flat12 ? x1 : x1 + (yc - y1) * k12);
    if (xb < xa) std::swap(xa, xb);
    // Clipped to the disc, so the relief ends on its smooth rim, not on cell edges.
    const int xs = std::max<int>(s_disc_l[y], ceilToInt(xa - 0.5f));
    const int xe = std::min<int>(s_disc_r[y], ceilToInt(xb - 0.5f) - 1);
    if (xe < xs) continue;
    const float px = xs + 0.5f - x0;
    int32_t sv = static_cast<int32_t>((s0 + px * dsdx + py * dsdy) * 4096.0f);
    int32_t ev = static_cast<int32_t>((e0 + px * dedx + py * dedy) * 4096.0f);
    int32_t uv = static_cast<int32_t>((u0 + px * dudx + py * dudy) * 4096.0f * 16.0f);
    uint8_t* row = buf + y * kW;
    if (urban) {
      shadeSpan<true>(row, xs, xe, y, sv, ev, uv, dsv, dev, duv);
    } else {
      shadeSpan<false>(row, xs, xe, y, sv, ev, uv, dsv, dev, duv);
    }
  }
}

/** Relief cells overlapping rows [y0, y1), into buf (row y at buf + y * kW). */
void drawRelief(uint8_t* buf, int y0, int y1) {
  constexpr int N = kReliefN;
  if (buf == nullptr) return;
  constexpr float kShadeScale = 16.0f / 256.0f;  // 128 (flat) -> shade 8
  constexpr float kElevScale = 7.0f / 255.0f;
  for (uint16_t n = 0; n < s_rc_count; ++n) {
    const uint16_t idx = s_rc_order[n];
    if (s_rc_ymax[idx] < y0 || s_rc_ymin[idx] >= y1) continue;
    const int i = idx % N, j = idx / N;
    const int k00 = j * (N + 1) + i, k10 = k00 + 1, k01 = k00 + N + 1, k11 = k01 + 1;
    const float a0 = s_rv_shade[k00] * kShadeScale, a1 = s_rv_shade[k10] * kShadeScale;
    const float a2 = s_rv_shade[k11] * kShadeScale, a3 = s_rv_shade[k01] * kShadeScale;
    const float e0 = s_rv_elev[k00] * kElevScale, e1 = s_rv_elev[k10] * kElevScale;
    const float e2 = s_rv_elev[k11] * kElevScale, e3 = s_rv_elev[k01] * kElevScale;
    constexpr float kU = 1.0f / 127.0f;
    const float u0 = s_rv_urban[k00] * kU, u1 = s_rv_urban[k10] * kU, u2 = s_rv_urban[k11] * kU,
                u3 = s_rv_urban[k01] * kU;
    shadeTriangle(buf, y0, y1, s_rv_x[k00], s_rv_y[k00], a0, e0, u0, s_rv_x[k10], s_rv_y[k10], a1, e1, u1,
                  s_rv_x[k11], s_rv_y[k11], a2, e2, u2);
    shadeTriangle(buf, y0, y1, s_rv_x[k00], s_rv_y[k00], a0, e0, u0, s_rv_x[k11], s_rv_y[k11], a2, e2, u2,
                  s_rv_x[k01], s_rv_y[k01], a3, e3, u3);
  }
}

bool s_rebuilt = false;  // drawSurface re-projected the ground this frame (camera moved)
uint32_t s_gt[3];        // us in drawSurface: re-projection, relief, everything else (log)

constexpr int kGroundBandH = 16;
uint8_t* s_gband = nullptr;
bool s_gband_tried = false;

/** Ground rows [ya, yb) (band aligned) through a band buffer; returns us in the relief. */
uint32_t drawGroundRows(int ya, int yb, int hy, uint8_t* bandbuf, FillState& sea, FillState& lake) {
  uint32_t t_relief = 0;
  for (int y0 = ya; y0 < yb; y0 += kGroundBandH) {
    const int y1 = std::min(yb, y0 + kGroundBandH);
    uint8_t* band = bandbuf != nullptr ? bandbuf : s_band_mem + static_cast<size_t>(y0) * kW;
    memset(band, kBg, static_cast<size_t>(y1 - y0) * kW);
    if (hy >= y0 && hy < y1) memset(band + static_cast<size_t>(hy - y0) * kW, kRingDim, kW);
    for (int y = std::max(y0, kTopBarH); y < y1; ++y) {  // flat land
      if (s_disc_l[y] <= s_disc_r[y]) {
        memset(band + static_cast<size_t>(y - y0) * kW + s_disc_l[y], kLand, s_disc_r[y] - s_disc_l[y] + 1);
      }
    }
    const uint32_t tr = micros();
    drawRelief(band - static_cast<ptrdiff_t>(y0) * kW, y0, y1);
    t_relief += micros() - tr;
    fillRows(sea, y1, band, y0);
    fillRows(lake, y1, band, y0);
    if (bandbuf != nullptr) memcpy(s_band_mem + static_cast<size_t>(y0) * kW, band, static_cast<size_t>(y1 - y0) * kW);
  }
  return t_relief;
}

// ---------------------------------------------------------------- second core
uint8_t* s_gband2 = nullptr;
float s_split_frac = 0.35f;
struct GroundJob {
  int ya, yb, hy;
  uint32_t us;
};
GroundJob s_job;
void groundJobRun() {
  const uint32_t t = micros();
  fillBegin(s_fill_sea2, s_sea, kSea);
  fillBegin(s_fill_lake2, s_lakes, kSea);
  drawGroundRows(s_job.ya, s_job.yb, s_job.hy, s_gband2, s_fill_sea2, s_fill_lake2);
  s_job.us = micros() - t;
}
#if defined(RADAR3D_HOST_TEST)
// Host preview: the second core's share is drawn when it is waited for.
bool groundWorkerReady() {
  if (!s_gband2) s_gband2 = static_cast<uint8_t*>(malloc(static_cast<size_t>(kW) * kGroundBandH));
  return s_gband2 != nullptr;
}
void groundWorkerStart(int ya, int yb, int hy) { s_job = {ya, yb, hy, 0}; }
uint32_t groundWorkerWait() {
  groundJobRun();
  return s_job.us;
}
#else
SemaphoreHandle_t s_job_go = nullptr, s_job_done = nullptr;
bool s_worker_tried = false, s_worker_ok = false;
void groundWorkerTask(void*) {
  for (;;) {
    xSemaphoreTake(s_job_go, portMAX_DELAY);
    groundJobRun();
    xSemaphoreGive(s_job_done);
  }
}
bool groundWorkerReady() {
  if (!s_worker_tried) {
    s_worker_tried = true;
    // Its band buffer lives in PSRAM (small enough to stay in the cache): internal RAM
    // is kept for the network.
    s_gband2 = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(kW) * kGroundBandH, MALLOC_CAP_SPIRAM));
    s_job_go = xSemaphoreCreateBinary();
    s_job_done = xSemaphoreCreateBinary();
    s_worker_ok = s_gband2 && s_job_go && s_job_done &&
                  xTaskCreatePinnedToCore(groundWorkerTask, "ground", 6144, nullptr, 3, nullptr, 0) == pdPASS;
    Serial.printf("radar3d: second-core ground drawing %s\n", s_worker_ok ? "on" : "off");
  }
  return s_worker_ok;
}
void groundWorkerStart(int ya, int yb, int hy) {
  s_job = {ya, yb, hy, 0};
  xSemaphoreGive(s_job_go);
}
uint32_t groundWorkerWait() {
  xSemaphoreTake(s_job_done, portMAX_DELAY);
  return s_job.us;
}
#endif

/**
 * Background, land, terrain, sea and lakes: everything on the ground below the map
 * lines. Drawn band by band into a small buffer in internal RAM and copied out once
 * per band: drawing the many small relief triangles straight into the PSRAM frame
 * kept reloading cache lines and cost ~140 ms per frame.
 */

void drawSurface(const Cam& c) {
  const SeaKey key{services::map::generation(), services::terrain::generation(), s_yaw_cur, c.R,
                   c.pitch_deg, services::location::lat(), services::location::lon()};
  if (key.gen != s_sea_key.gen || key.tgen != s_sea_key.tgen || key.yaw != s_sea_key.yaw ||
      key.R != s_sea_key.R || key.pitch != s_sea_key.pitch || key.lat != s_sea_key.lat ||
      key.lon != s_sea_key.lon) {
    s_sea_key = key;
    s_rebuilt = true;
    const uint32_t t0 = micros();
    buildDiscSpans(c);
    const bool sea_ok = buildRings(c, 2, false, &s_sea);
    const bool lakes_ok = buildRings(c, 3, true, &s_lakes);
    buildRelief(c);
    // A fill that could not get its memory is retried on the next frame instead of
    // staying missing until the camera moves again.
    if (!sea_ok || !lakes_ok) s_sea_key.gen = 0xFFFFFFFFu;
    s_gt[0] += micros() - t0;
  }
  const uint32_t t_bands = micros();
  uint32_t t_relief = 0;
  if (!s_gband_tried) {
    s_gband_tried = true;
#if defined(RADAR3D_HOST_TEST)
    s_gband = static_cast<uint8_t*>(malloc(static_cast<size_t>(kW) * kGroundBandH));
#else
    s_gband = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(kW) * kGroundBandH, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
  }
  const float horizon = c.cy - c.f * tanf(c.pitch_deg * kDegToRad);
  const int hy = std::max(kTopBarH, ri(horizon));
  s_disc_top = kH;
  s_disc_bottom = 0;
  for (int y = kTopBarH; y < kH; ++y) {
    if (s_disc_l[y] <= s_disc_r[y]) {
      s_disc_top = std::min(s_disc_top, y);
      s_disc_bottom = std::max(s_disc_bottom, y);
    }
  }
  // Split between the two cores: the second core (which also refills the panel's
  // bounce buffers) takes the rows from the top down to y_split, this core the rest.
  // The split follows the measured times of both so they finish together.
  int y_split = kTopBarH;
  if (groundWorkerReady()) {
    int64_t total = 0;
    for (int y = kTopBarH; y < kH; ++y) if (s_disc_l[y] <= s_disc_r[y]) total += s_disc_r[y] - s_disc_l[y] + 1;
    int64_t acc = 0;
    const int64_t want = static_cast<int64_t>(total * s_split_frac);
    int y = kTopBarH;
    for (; y < kH && acc < want; ++y) if (s_disc_l[y] <= s_disc_r[y]) acc += s_disc_r[y] - s_disc_l[y] + 1;
    y_split = std::min(kH, (y + kGroundBandH - 1) / kGroundBandH * kGroundBandH);  // band aligned
  }
  if (y_split > kTopBarH) {
    groundWorkerStart(0, y_split, hy);
    fillBeginAt(s_fill_sea, s_sea, kSea, y_split);  // over the relief: the map's coastline cuts it cleanly
    fillBeginAt(s_fill_lake, s_lakes, kSea, y_split);
    t_relief = drawGroundRows(y_split, kH, hy, s_gband, s_fill_sea, s_fill_lake);
    const uint32_t main_us = micros() - t_bands;
    const uint32_t worker_us = groundWorkerWait();
    // Move the split towards equal times (small steps, so it settles without hunting).
    if (worker_us > main_us + main_us / 10) s_split_frac = std::max(0.05f, s_split_frac - 0.02f);
    else if (worker_us + worker_us / 10 < main_us) s_split_frac = std::min(0.7f, s_split_frac + 0.02f);
  } else {
    fillBegin(s_fill_sea, s_sea, kSea);
    fillBegin(s_fill_lake, s_lakes, kSea);
    t_relief = drawGroundRows(0, kH, hy, s_gband, s_fill_sea, s_fill_lake);
  }
  s_band_y0 = 0;
  s_band_y1 = kH;
  s_gt[1] += t_relief;
  s_gt[2] += micros() - t_bands - t_relief;
}

// Projected map points, kept while the camera stays put (re-projecting every point
// with its terrain height each frame cost ~20 ms).
int16_t* s_map_xy = nullptr;
size_t s_map_xy_cap = 0;
struct MapKey {
  uint32_t gen, tgen;
  float yaw, R, pitch;
} s_map_key = {0xFFFFFFFFu, 0, 0, 0, 0};

void drawMap(const Cam& c) {
  const uint8_t* m = services::map::data();
  const size_t size = services::map::size();
  if (m == nullptr || size < services::map::kHeaderBytes) {
    return;
  }
  const size_t max_pts = size / 4;
  if (s_map_xy_cap < max_pts) {
    free(s_map_xy);
    s_map_xy = static_cast<int16_t*>(malloc(max_pts * 2 * sizeof(int16_t)));
    s_map_xy_cap = s_map_xy ? max_pts : 0;
    s_map_key.gen = 0xFFFFFFFFu;
  }
  const MapKey key{services::map::generation(), services::terrain::generation(), s_yaw_cur, c.R, c.pitch_deg};
  const bool fresh = s_map_xy != nullptr && key.gen == s_map_key.gen && key.tgen == s_map_key.tgen &&
                     key.yaw == s_map_key.yaw && key.R == s_map_key.R && key.pitch == s_map_key.pitch;
  if (s_map_xy != nullptr) s_map_key = key;
  size_t pt_index = 0;
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
    // Motorways / trunk roads stand out a little: 2 px with a dark edge under them,
    // so the main axes read as the borders between parts of the city.
    auto seg = [&](int x0, int y0, int x1, int y1) {
      if (type == 1) {
        s_spr.drawLine(x0, y0 + 1, x1, y1 + 1, kBg);
        s_spr.drawLine(x0, y0 - 1, x1, y1 - 1, color);
      }
      s_spr.drawLine(x0, y0, x1, y1, color);
    };
    const bool closed = type == 3;
    float fsx = 0, fsy = 0;
    bool have_first = false;
    bool have_prev = false;
    float psx = 0, psy = 0;
    for (uint16_t i = 0; i < n; ++i, pos += 4) {
      int16_t xi, yi;
      memcpy(&xi, m + pos, 2);
      memcpy(&yi, m + pos + 2, 2);
      float sx, sy;
      int16_t* cache = s_map_xy != nullptr && pt_index < s_map_xy_cap ? s_map_xy + pt_index * 2 : nullptr;
      ++pt_index;
      if (fresh && cache != nullptr) {
        if (cache[0] == INT16_MIN) {
          have_prev = false;
          continue;
        }
        sx = cache[0];
        sy = cache[1];
      } else {
        const float x = ox + xi * 0.01f;
        const float y = oy + yi * 0.01f;
        // Coast lines at sea level, exactly on the edge of the (flat) sea fill; lake
        // shores and roads follow the terrain like the lakes do.
        const float z = type == 0 ? 0.0f : groundZ(c, x, y);
        const bool ok = x * x + y * y <= r2 && project(c, x, y, z, &sx, &sy) && fabsf(sx) < 30000 && fabsf(sy) < 30000;
        if (cache != nullptr) {
          cache[0] = ok ? static_cast<int16_t>(lroundf(sx)) : INT16_MIN;
          cache[1] = ok ? static_cast<int16_t>(lroundf(sy)) : 0;
          if (ok) { sx = cache[0]; sy = cache[1]; }
        }
        if (!ok) {
          have_prev = false;
          continue;
        }
      }
      if (have_prev) {
        seg(ri(psx), ri(psy), ri(sx), ri(sy));
      } else if (i == 0) {
        fsx = sx;
        fsy = sy;
        have_first = true;
      }
      psx = sx;
      psy = sy;
      have_prev = true;
    }
    if (closed && have_first && have_prev) seg(ri(psx), ri(psy), ri(fsx), ri(fsy));
  }
}

void ringPath(const Cam& c, float r, uint8_t color, bool dashed) {
  constexpr int kSeg = 120;
  // The outer ring is the rim of the (flat) disc the relief is clipped to: at ground
  // level, two pixels wide. Inner rings follow the terrain.
  const bool rim = !dashed;
  float px = 0, py = 0;
  for (int i = 0; i <= kSeg; ++i) {
    const float t = static_cast<float>(i) / kSeg * 6.2831853f;
    float sx, sy;
    const float rx = r * cosf(t), ry = r * sinf(t);
    if (!project(c, rx, ry, rim ? 0.0f : groundZ(c, rx, ry), &sx, &sy)) continue;
    if (i > 0 && (!dashed || (i % 3) != 0)) {
      s_spr.drawLine(ri(px), ri(py), ri(sx), ri(sy), color);
      if (rim) s_spr.drawLine(ri(px), ri(py) - 1, ri(sx), ri(sy) - 1, color);
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

#if defined(BOARD_WS5)
  setFont(&ui::font::Sans8);  // the built-in 6x8 font is too small on the 800x480 panel
#else
  setFont(&fonts::Font0);
#endif
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
      // The disc may run past the screen edge: keep the label on screen.
      const int half = s_spr.textWidth(label) / 2 + S(4);
      const int lx = std::max(half, std::min(kW - half, ri(sx) + S(12)));
      s_spr.drawString(label, lx, ri(sy) + S(6));
    }
  }

  // Cardinal letters (Turkish: K north, D east, G south, B west).
  setFont(&ui::font::SansBold9);
  const char* names[4] = {"K", "D", "G", "B"};
  const float dirs[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
  for (int i = 0; i < 4; ++i) {
    float sx, sy;
    if (project(c, dirs[i][0] * c.R * 1.14f, dirs[i][1] * c.R * 1.14f, 0, &sx, &sy) &&
        (sx = std::max<float>(S(10), std::min<float>(kW - S(10), sx)), sy < kH - S(10)) &&
        !overlapsUi(ri(sx) - S(8), ri(sy) - S(8), S(16), S(16))) {
      s_spr.setTextColor(kRing);
      s_spr.drawString(names[i], ri(sx), ri(sy));
    }
  }

  float hx, hy;
  if (project(c, 0, 0, groundZ(c, 0, 0), &hx, &hy)) {
    // Red house on the radar centre: roof triangle over a square body, dark outline.
    const int x = ri(hx), y = ri(hy);
    s_spr.fillTriangle(x - S(8), y - S(5), x + S(8), y - S(5), x, y - S(13), kBg);
    s_spr.fillRect(x - S(6), y - S(6), S(13), S(9), kBg);
    s_spr.fillTriangle(x - S(7), y - S(6), x + S(7), y - S(6), x, y - S(12), kRed);
    s_spr.fillRect(x - S(5), y - S(6), S(11), S(8), kRed);
    s_spr.fillRect(x - S(1), y - S(2), S(3), S(4), kBg);  // door
    s_spr.drawFastHLine(x - S(3), y + S(3), S(7), kRed);
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

// ------------------------------------------------------------ 3D aircraft
// Low-poly airliner, body frame: x forward, y right, z up; 16 units nose to tail
// (the size of the 2D icon), drawn kModelScale times the icon size.
constexpr float kModelScale = 1.75f;
struct V3 {
  float x, y, z;
};
struct Face {
  uint8_t a, b, c;
  uint8_t part;  // 0 fuselage, 1 wing, 2 tail, 3 engine
};
constexpr int kFusRing = 6;  // fuselage cross-section corners
// Fuselage stations: x, radius, centre z.
constexpr float kStation[][3] = {{6.2f, 1.05f, 0.0f}, {-4.6f, 1.0f, 0.05f}, {-7.6f, 0.35f, 0.45f}};
constexpr int kNoseV = 0;
constexpr int kTailV = 1;
constexpr int kStationV0 = 2;  // 3 stations x kFusRing vertices follow
constexpr int kWingV0 = kStationV0 + 3 * kFusRing;

V3 s_mesh_v[kWingV0 + 12 + 8 + 4 + 16];
Face s_mesh_f[80];
int s_mesh_nv = 0, s_mesh_nf = 0;

void buildMesh() {
  if (s_mesh_nv) return;
  int nv = 0, nf = 0;
  auto v = [&](float x, float y, float z) { s_mesh_v[nv] = {x, y, z}; return nv++; };
  auto f = [&](int a, int b, int c, uint8_t part) { s_mesh_f[nf++] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, part}; };
  v(8.4f, 0.0f, -0.15f);  // nose
  v(-8.0f, 0.0f, 0.55f);  // tail cone
  for (const auto& st : kStation) {
    for (int i = 0; i < kFusRing; ++i) {
      const float t = (i + 0.5f) * 6.2831853f / kFusRing;
      v(st[0], cosf(t) * st[1], st[2] + sinf(t) * st[1] * 0.95f);
    }
  }
  auto sv = [&](int s, int i) { return kStationV0 + s * kFusRing + (i % kFusRing); };
  for (int i = 0; i < kFusRing; ++i) {
    f(kNoseV, sv(0, i + 1), sv(0, i), 0);
    for (int s = 0; s < 2; ++s) {
      f(sv(s, i), sv(s, i + 1), sv(s + 1, i + 1), 0);
      f(sv(s, i), sv(s + 1, i + 1), sv(s + 1, i), 0);
    }
    f(kTailV, sv(2, i), sv(2, i + 1), 0);
  }
  // Wings: swept, slight dihedral; a top and a bottom face.
  for (int side = -1; side <= 1; side += 2) {
    const int a = v(2.2f, side * 0.9f, -0.35f), b = v(-2.4f, side * 8.6f, 0.45f);
    const int c = v(-3.7f, side * 8.6f, 0.45f), d = v(-1.8f, side * 0.9f, -0.35f);
    const int a2 = v(2.2f, side * 0.9f, -0.55f), d2 = v(-1.8f, side * 0.9f, -0.55f);
    f(a, b, c, 1); f(a, c, d, 1);
    f(a2, c, b, 1); f(a2, d2, c, 1);
  }
  // Horizontal stabilisers.
  for (int side = -1; side <= 1; side += 2) {
    const int a = v(-5.9f, side * 0.4f, 0.45f), b = v(-7.7f, side * 3.5f, 0.75f);
    const int c = v(-8.5f, side * 3.5f, 0.75f), d = v(-7.7f, side * 0.4f, 0.45f);
    f(a, b, c, 2); f(a, c, d, 2);
  }
  // Fin.
  {
    const int a = v(-5.4f, 0.0f, 0.9f), b = v(-7.7f, 0.0f, 3.9f);
    const int c = v(-8.6f, 0.0f, 3.9f), d = v(-8.1f, 0.0f, 0.6f);
    f(a, b, c, 2); f(a, c, d, 2);
  }
  // Engines under the wings (square nacelles, open ends are too small to see).
  for (int side = -1; side <= 1; side += 2) {
    const float y0 = side * 3.3f, zc = -0.95f, rr = 0.5f;
    int fr[4], bk[4];
    for (int i = 0; i < 4; ++i) {
      const float t = (i + 0.5f) * 1.5707963f;
      fr[i] = v(1.6f, y0 + cosf(t) * rr, zc + sinf(t) * rr);
      bk[i] = v(-0.9f, y0 + cosf(t) * rr * 0.85f, zc + sinf(t) * rr * 0.85f);
    }
    for (int i = 0; i < 4; ++i) {
      const int j = (i + 1) % 4;
      f(fr[i], fr[j], bk[j], 3); f(fr[i], bk[j], bk[i], 3);
    }
  }
  s_mesh_nv = nv;
  s_mesh_nf = nf;
}


/**
 * One aircraft as a shaded 3D model, into the frame: turned to its real heading
 * relative to the camera but seen from a steeper angle than the scene (55 deg) so
 * wings and tail read like the 2D icon; flat-shaded from the altitude colour's
 * palette ramp; dark outline as on the icon. Nose up when climbing, down when
 * descending.
 */
void drawPlaneModel(const Cam& c, const services::adsb::Aircraft& a, float sx, float sy, float size) {
  buildMesh();
  constexpr int kNv = sizeof(s_mesh_v) / sizeof(s_mesh_v[0]);
  constexpr int kNf = sizeof(s_mesh_f) / sizeof(s_mesh_f[0]);
  static int16_t px[kNv], py[kNv];
  static float pz[kNv];
  static V3 vv[kNv];
  static uint8_t order[kNf];
  static float fdepth[kNf];
  const float view = std::max(c.pitch_deg, 55.0f) * kDegToRad;
  const float cv = cosf(view), sv = sinf(view);
  const float lx = -0.5f, ly = 0.45f, lz = 0.74f;  // light in view space: upper left, front
  const float scale = size * kModelScale / 16.0f;  // px per model unit
  const float tr = a.track_deg * kDegToRad;
  const float wx = sinf(tr), wy = cosf(tr);
  const float fX = wx * c.cyaw - wy * c.syaw, fY = wx * c.syaw + wy * c.cyaw;  // camera ground frame
  const float rX = fY, rY = -fX;
  const float pitch = std::max(-0.18f, std::min(0.18f, a.vrate_fpm / 6000.0f));
  const float cpi = cosf(pitch), spi = sinf(pitch);
  for (int i = 0; i < s_mesh_nv; ++i) {
    const V3& m = s_mesh_v[i];
    const float bx = m.x * cpi - m.z * spi, bz = m.x * spi + m.z * cpi;
    const float X = bx * fX + m.y * rX, Y = bx * fY + m.y * rY, Z = bz;
    const float up = Y * sv + Z * cv, tow = -Y * cv + Z * sv;
    vv[i] = {X, up, tow};
    px[i] = static_cast<int16_t>(lroundf(sx + X * scale));
    py[i] = static_cast<int16_t>(lroundf(sy - up * scale));
    pz[i] = tow;
  }
  for (int i = 0; i < s_mesh_nf; ++i) {
    const Face& fc = s_mesh_f[i];
    fdepth[i] = pz[fc.a] + pz[fc.b] + pz[fc.c];
    order[i] = static_cast<uint8_t>(i);
  }
  std::sort(order, order + s_mesh_nf, [](uint8_t l, uint8_t r) { return fdepth[l] < fdepth[r]; });
  const uint8_t col0 = altitudeColor(a.alt_ft);
  const int ramp0 = kPlaneBase + (col0 == kOrange ? 0 : col0 == kCream ? 1 : 2) * kPlaneShades;
  {  // drawn once into a scratch buffer, outlined on the copy to the frame
    int x0 = INT16_MAX, y0 = INT16_MAX, x1 = INT16_MIN, y1 = INT16_MIN;
    for (int i = 0; i < s_mesh_nv; ++i) {
      x0 = std::min<int>(x0, px[i]); x1 = std::max<int>(x1, px[i]);
      y0 = std::min<int>(y0, py[i]); y1 = std::max<int>(y1, py[i]);
    }
    const int w = x1 - x0 + 3, h = y1 - y0 + 3;  // one empty pixel all round
    uint8_t* sb = (w > 0 && h > 0 && w <= 400 && h <= 400) ? scratchBuf(static_cast<size_t>(w) * h) : nullptr;
    if (sb != nullptr) {
      memset(sb, 0, static_cast<size_t>(w) * h);
      const int ox = 1 - x0, oy = 1 - y0;
      for (int n = 0; n < s_mesh_nf; ++n) {
        const Face& fc = s_mesh_f[order[n]];
        const V3 &A = vv[fc.a], &B = vv[fc.b], &C = vv[fc.c];
        const float ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
        const float qx = C.x - A.x, qy = C.y - A.y, qz = C.z - A.z;
        float nx = uy * qz - uz * qy, ny = uz * qx - ux * qz, nz = ux * qy - uy * qx;
        const float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len <= 0) continue;
        nx /= len; ny /= len; nz /= len;
        if (nz < 0) { nx = -nx; ny = -ny; nz = -nz; }
        float lit = 0.42f + 0.58f * std::max(0.0f, nx * lx + ny * ly + nz * lz);
        if (fc.part == 3) lit *= 0.7f;
        if (fc.part == 2) lit *= 0.93f;
        int j = static_cast<int>(lroundf((lit - 0.32f) / 0.78f * (kPlaneShades - 1)));
        j = std::max(0, std::min(kPlaneShades - 1, j));
        scratchTriangle(sb, w, h, px[fc.a] + ox, py[fc.a] + oy, px[fc.b] + ox, py[fc.b] + oy,
                        px[fc.c] + ox, py[fc.c] + oy, static_cast<uint8_t>(ramp0 + j));
      }
      blitOutlined(sb, w, h, x0 - 1, y0 - 1, 0);
      return;
    }
  }
  for (int o = 0; o < 4; ++o) {  // outline (fallback: no scratch memory)
    const int ox = o == 0 ? -1 : o == 1 ? 1 : 0, oy = o == 2 ? -1 : o == 3 ? 1 : 0;
    for (int i = 0; i < s_mesh_nf; ++i) {
      const Face& fc = s_mesh_f[i];
      if (fc.part == 3) continue;  // engines sit inside the wing's outline
      s_spr.fillTriangle(px[fc.a] + ox, py[fc.a] + oy, px[fc.b] + ox, py[fc.b] + oy, px[fc.c] + ox,
                         py[fc.c] + oy, kBg);
    }
  }
  const uint8_t col = altitudeColor(a.alt_ft);
  const int ramp = kPlaneBase + (col == kOrange ? 0 : col == kCream ? 1 : 2) * kPlaneShades;
  for (int n = 0; n < s_mesh_nf; ++n) {
    const Face& fc = s_mesh_f[order[n]];
    const V3 &A = vv[fc.a], &B = vv[fc.b], &C = vv[fc.c];
    const float ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
    const float qx = C.x - A.x, qy = C.y - A.y, qz = C.z - A.z;
    float nx = uy * qz - uz * qy, ny = uz * qx - ux * qz, nz = ux * qy - uy * qx;
    const float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len <= 0) continue;
    nx /= len; ny /= len; nz /= len;
    if (nz < 0) { nx = -nx; ny = -ny; nz = -nz; }  // the side facing the viewer
    float lit = 0.42f + 0.58f * std::max(0.0f, nx * lx + ny * ly + nz * lz);
    if (fc.part == 3) lit *= 0.7f;
    if (fc.part == 2) lit *= 0.93f;
    // Ramp: shade j has brightness 0.32 + 0.78 * j / 23.
    int j = static_cast<int>(lroundf((lit - 0.32f) / 0.78f * (kPlaneShades - 1)));
    j = std::max(0, std::min(kPlaneShades - 1, j));
    s_spr.fillTriangle(px[fc.a], py[fc.a], px[fc.b], py[fc.b], px[fc.c], py[fc.c],
                       static_cast<uint8_t>(ramp + j));
  }
}

// When a new position report arrives, the dead-reckoned position usually differs a
// little from where the aircraft was being drawn (feed delay, turns): it would jump,
// often backwards. Each aircraft keeps that difference as an offset that fades out
// over about a second, so it glides onto the reported track instead.
struct Glide {
  char callsign[9];
  float x, y;    // last drawn position (km)
  float ox, oy;  // offset still to fade out
  unsigned long fetched_ms, seen_ms;
};
Glide s_glide[services::adsb::kMaxAircraft];

void glidePosition(const services::adsb::Aircraft& a, unsigned long fetched_ms, float* x, float* y) {
  const unsigned long now = millis();
  Glide* g = nullptr;
  Glide* oldest = &s_glide[0];
  for (auto& e : s_glide) {
    if (strncmp(e.callsign, a.callsign, sizeof(e.callsign)) == 0 && e.callsign[0]) { g = &e; break; }
    if (e.seen_ms < oldest->seen_ms) oldest = &e;
  }
  if (g == nullptr || now - g->seen_ms > 15000) {
    if (g == nullptr) g = oldest;
    memcpy(g->callsign, a.callsign, sizeof(g->callsign));
    g->ox = g->oy = 0.0f;
    g->fetched_ms = fetched_ms;
  } else if (g->fetched_ms != fetched_ms) {  // new report: start from where it was drawn
    g->ox = g->x - *x;
    g->oy = g->y - *y;
    if (g->ox * g->ox + g->oy * g->oy > 4.0f) g->ox = g->oy = 0.0f;  // > 2 km: just move
    g->fetched_ms = fetched_ms;
  }
  const float k = 1.0f - ease(0.7f);
  g->ox *= k;
  g->oy *= k;
  *x += g->ox;
  *y += g->oy;
  g->x = *x;
  g->y = *y;
  g->seen_ms = now;
}

void drawAircraft(const Cam& c, size_t count, unsigned long fetched_ms, float* dist_km,
                  float* bearing_deg, int card_idx_out[1]) {
  size_t n_items = 0;
  for (size_t i = 0; i < count; ++i) {
    // Positions (and their glide state) are worked out once per frame, in the first band.
    static float fx[services::adsb::kMaxAircraft], fy[services::adsb::kMaxAircraft];
    float x, y;
    if (s_first_band) {
      currentPositionKm(s_planes[i], fetched_ms, &x, &y);
      glidePosition(s_planes[i], fetched_ms, &x, &y);
      fx[i] = x;
      fy[i] = y;
    } else {
      x = fx[i];
      y = fy[i];
    }
    dist_km[i] = sqrtf(x * x + y * y);
    float b = atan2f(x, y) / kDegToRad;
    if (b < 0) b += 360.0f;
    bearing_deg[i] = b;
    if (dist_km[i] > c.R * 1.05f) continue;
    // Ground point: depth for sorting; the altitude is added on screen (below).
    const float z = groundZ(c, x, y);
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

  // Altitude is drawn as a screen height above the aircraft's ground point, the same
  // number of pixels per foot near and far, so an aircraft holding its altitude keeps
  // the same height as it crosses the disc (perspective would shrink it with distance).
  // The scale follows the traffic: the highest aircraft is brought just under the
  // legend, so all-low traffic stands tall and mixed traffic still fits; eased so
  // aircraft entering or leaving do not make the view jump.
  {
    constexpr float kTopLimit = kTopBarH + kStripH + ui::Sf(26.0f);  // room for the model
    float target = 1e9f;
    for (size_t k = 0; k < n_items; ++k) {
      const int32_t alt = s_planes[s_items[k].idx].alt_ft;
      if (dist_km[s_items[k].idx] > c.R || alt < 500) continue;
      float gx, gy;
      if (!project(c, s_items[k].x_km, s_items[k].y_km, s_items[k].z, &gx, &gy)) continue;
      target = std::min(target, std::max(ui::Sf(16.0f), gy - kTopLimit) / static_cast<float>(alt));
    }
    // Upper bound: a lone low aircraft is not stretched beyond ~120 px per 3000 ft.
    const float kMaxPxPerFt = ui::Sf(0.04f) * services::display_prefs::get().exag / 2.0f;
    if (target > kMaxPxPerFt) target = kMaxPxPerFt;
    if (s_alt_px <= 0.0f) s_alt_px = target;
    // Shrink quickly (never out of the top), grow gently. Once per frame.
    if (s_first_band) s_alt_px += (target - s_alt_px) * (target < s_alt_px ? ease(0.12f) : ease(0.9f));
  }

  s_item_count = n_items;
  const int card_idx = cardAircraft(count, dist_km, c.R);
  card_idx_out[0] = card_idx;
  const bool labels = services::display_prefs::get().labels;
  const float unit = c.R / 50.0f;  // depth unit of the mockup
  s_hit_count = 0;

  for (size_t k = 0; k < n_items; ++k) {
    const DrawItem& it = s_items[k];
    const auto& a = s_planes[it.idx];
    const uint8_t col = altitudeColor(a.alt_ft);
    float gx, gy, d;
    project(c, it.x_km, it.y_km, it.z, &gx, &gy, &d);
    const float lift = static_cast<float>(std::max<int32_t>(0, a.alt_ft)) * s_alt_px;
    const float sx = gx, sy = gy - lift;

    // Trail: where it was over the last minute (straight track).
    const float tr = a.track_deg * kDegToRad;
    const float back_km = a.gs_knots * 1.852f * (12.0f / 3600.0f);
    float lx = sx, ly = sy;
    for (int s = 1; s <= 5; ++s) {
      float tx, ty;
      // Straight track: all points at the aircraft's own ground level (following the
      // terrain under them made the line wavy).
      const float bx = it.x_km - sinf(tr) * back_km * s, by = it.y_km - cosf(tr) * back_km * s;
      if (!project(c, bx, by, it.z, &tx, &ty)) break;
      ty -= lift;
      dottedLine(ri(lx), ri(ly), ri(tx), ri(ty), col, 1, 3);
      lx = tx;
      ly = ty;
    }

    // Drop line to the ground and the ground shadow give the depth cue.
    dottedLine(ri(sx), ri(sy), ri(gx), ri(gy), col, 2, 2);
    s_spr.fillEllipse(ri(gx), ri(gy), S(3), S(1), col);

    // Icon, rotated to where the track points on screen.
    float ax, ay;
    float heading = 0.0f;
    if (project(c, it.x_km + sinf(tr) * 2.0f * unit, it.y_km + cosf(tr) * 2.0f * unit, it.z, &ax,
                &ay)) {
      ay -= lift;
      heading = atan2f(ax - sx, -(ay - sy)) / kDegToRad;
    }
    float size = ui::Sf(16.0f) * 100.0f / (d / unit);
    if (size < ui::Sf(9.0f)) size = ui::Sf(9.0f);
    if (size > ui::Sf(18.0f)) size = ui::Sf(18.0f);
    s_item_size[k] = size;
    if (kPlane3d) {
      drawPlaneModel(c, a, sx, sy, size);
    } else {
      drawPlaneIcon(ri(sx), ri(sy), heading, size / 16.0f * 1.1f, col);
    }

    if (static_cast<int>(it.idx) == card_idx && s_selected[0] != '\0') {
      const int rad = kPlane3d ? std::max(S(15), ri(size * kModelScale * 0.62f)) : S(15);
      s_spr.drawCircle(ri(sx), ri(sy), rad, col);
      if (ui::kScaleNum != ui::kScaleDen) s_spr.drawCircle(ri(sx), ri(sy), rad + 1, col);
    }

    // Labels sit beside the model, which is larger than the icon was.
    s_label_pos[k] = {ri(sx), ri(sy), ri(kPlane3d ? size * kModelScale * 0.8f : size)};

    if (s_hit_count < services::adsb::kMaxAircraft) {
      Hit& h = s_hits[s_hit_count++];
      h.x = ri(sx);
      h.y = ri(sy);
      memcpy(h.callsign, a.callsign, sizeof(h.callsign));
    }
  }

  s_label_box_count = 0;  // aircraft labels claim first; place names fill in after
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
      if (is_card && s_selected[0] && infoInLabel()) {
        // "Label" style: the selected aircraft's details in a small black card at its tag.
        services::route::Route rt;
        const services::route::Status rs = services::route::get(a.callsign, &rt);
        char l1[12], l2[32], l3[40];
        snprintf(l1, sizeof(l1), "%s", a.type);
        char tr[12] = "";
        if (a.vrate_fpm > 300) snprintf(tr, sizeof(tr), "  ^%d", a.vrate_fpm);
        else if (a.vrate_fpm < -300) snprintf(tr, sizeof(tr), "  v%d", -a.vrate_fpm);
        snprintf(l2, sizeof(l2), "%s  %dkt%s", alt, static_cast<int>(lroundf(a.gs_knots)), tr);
        const float dk = dist_km[s_items[k].idx];
        if (rs == services::route::Status::Found) {
          snprintf(l3, sizeof(l3), "%.1fkm  %s > %s", dk, rt.from_code[0] ? rt.from_code : "?",
                   rt.to_code[0] ? rt.to_code : "?");
        } else {
          snprintf(l3, sizeof(l3), "%.1fkm", dk);
        }
        setFont(&ui::font::SansBold8);
        const int wc = s_spr.textWidth(a.callsign);
        setFont(&ui::font::Sans8);
        const int w = std::max({wc + S(8) + static_cast<int>(s_spr.textWidth(l1)),
                                static_cast<int>(s_spr.textWidth(l2)),
                                static_cast<int>(s_spr.textWidth(l3))}) + S(12);
        constexpr int h = S(48);
        const int off = ri(lp.size * 0.6f) + S(6);
        Box box{lp.x + off, lp.y - S(12), w, h};
        if (!claimLabel(box)) {
          box.x = lp.x - off - w;
          claimLabel(box);  // drawn anyway: the selected aircraft always gets its card
        }
        box.x = std::max(S(2), std::min(kW - S(2) - w, box.x));
        box.y = std::max(kTopBarH + S(3), std::min(kH - S(2) - h, box.y));
        s_spr.fillRoundRect(box.x, box.y, w, h, S(5), kBg);
        s_spr.drawRoundRect(box.x, box.y, w, h, S(5), kRingDim);
        const uint8_t col = altitudeColor(a.alt_ft);
        setFont(&ui::font::SansBold8);
        s_spr.setTextDatum(textdatum_t::top_left);
        s_spr.setTextColor(col);
        s_spr.drawString(a.callsign, box.x + S(6), box.y + S(3));
        setFont(&ui::font::Sans8);
        s_spr.setTextColor(kTextDim);
        s_spr.drawString(l1, box.x + S(6) + wc + S(8), box.y + S(4));
        s_spr.setTextColor(kText);
        s_spr.drawString(l2, box.x + S(6), box.y + S(18));
        s_spr.drawString(l3, box.x + S(6), box.y + S(32));
        continue;
      }
      const bool two_lines = !busy || is_card;
      setFont(&ui::font::SansBold8);
      int w = s_spr.textWidth(a.callsign);
      if (two_lines) {
        setFont(&ui::font::Sans8);
        w = std::max<int>(w, s_spr.textWidth(alt));
      }
      w += S(3);
      const int h = two_lines ? S(30) : S(16);
      const int off = ri(lp.size * 0.6f) + S(5);
      Box box{lp.x + off, lp.y - S(9), w, h};
      if (!claimLabel(box)) {
        box.x = lp.x - off - w;
        if (!claimLabel(box)) continue;
      }
      setFont(&ui::font::SansBold8);
      haloText(a.callsign, box.x + 1, box.y, altitudeColor(a.alt_ft));
      if (two_lines) {
        setFont(&ui::font::Sans8);
        haloText(alt, box.x + 1, box.y + S(14), kText);
      }
    }
  }
}

void drawTopBar(size_t in_range, unsigned long fetched_ms) {
  s_spr.fillRect(0, 0, kW, kTopBarH, kPanel);
  s_spr.drawFastHLine(0, kTopBarH, kW, kRingDim);
  const bool fresh = fetched_ms != 0 && millis() - fetched_ms < 20000;
  const int ty = kTopBarH / 2;
  s_spr.fillCircle(S(10), ty, S(3), fresh ? kCyan : kOrange);

  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::middle_left);
  s_spr.setTextColor(kText);
  s_spr.drawString("RADAR 3B", S(19), ty);
  const int src_x = S(19) + s_spr.textWidth("RADAR 3B") + S(10);
  s_spr.setTextColor(kTextDim);
  s_spr.drawString(services::usb_feed::active() ? "USB" : services::adsb::sourceName(), src_x, ty);

  char buf[40];
  char range[12];
  ui::radar::formatRing3Label(range, sizeof(range), ui::radar::rangeCurrent().outer_km,
                              ui::radar::useMiles());
  snprintf(buf, sizeof(buf), "%u uçak  /  %s", static_cast<unsigned>(in_range), range);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.setTextColor(kText);
  s_spr.drawString(buf, kW / 2, ty);

  // Right side: indoor temperature / humidity (SHT30) and the time.
  int x = kW - S(8);
  s_spr.setTextDatum(textdatum_t::middle_right);
  tm t;
  if (services::clock::now(&t)) {
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    s_spr.setTextColor(kText);
    s_spr.drawString(buf, x, ty);
    x -= s_spr.textWidth(buf) + S(12);
  }
  if (services::sht30::valid()) {
    snprintf(buf, sizeof(buf), "%%%d", static_cast<int>(lroundf(services::sht30::humidity())));
    s_spr.setTextColor(kTextDim);
    s_spr.drawString(buf, x, ty);
    x -= s_spr.textWidth(buf) + S(10);
    snprintf(buf, sizeof(buf), "%.1f°C", services::sht30::temperatureC());
    s_spr.drawString(buf, x, ty);
  }
}

void drawLegendStrip() {
  const int y0 = kTopBarH + 1;
  s_spr.fillRect(0, y0, kW, kStripH, kPanel);
  s_spr.drawFastHLine(0, y0 + kStripH, kW, kRingDim);
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::middle_left);
  s_spr.setTextColor(kText);
  const struct {
    uint8_t col;
    const char* text;
  } rows[3] = {{kCyan, "25K ft +"}, {kCream, "10-25K ft"}, {kOrange, "10K ft altı"}};
  int x = S(10);
  const int yc = y0 + kStripH / 2;
  for (const auto& r : rows) {
    s_spr.fillCircle(x + S(4), yc, S(4), r.col);
    s_spr.drawString(r.text, x + S(13), yc + 1);
    x += S(13) + s_spr.textWidth(r.text) + S(22);
  }
}

/** Bottom right: opens the forecast screen. Sun + cloud and "HAVA". */
void drawSwitchButton() {
  using namespace ui;
  s_spr.fillRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, S(8), kPanel);
  s_spr.drawRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, S(8), kRing);
  const weather::IconPalette<Pal> pal{kPanel, kOrange, kCream, kCream, kTextDim,
                                      kCyan,  kText,   kOrange, kTextDim};
  weather::drawWeatherIcon(s_spr, kSwitchX + S(22), kSwitchY + kSwitchH / 2, S(14),
                           services::weather::Condition::PartlyCloudy, true, pal);
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::middle_left);
  s_spr.setTextColor(kText);
  setFont(&ui::font::SansBold8);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.drawString("HAVA", (kSwitchX + S(40) + kSwitchX + kSwitchW) / 2, kSwitchY + kSwitchH / 2 + 1);
}

void drawControls() {
  // No arrow keys: the map is turned and tilted by dragging. Water bottom left,
  // forecast bottom right.
  drawSwitchButton();
  ui::drawWaterButton(s_spr, ui::kRadarWaterX, ui::kWaterBtnY, ui::kWaterBtnW, ui::kWaterBtnH, kPanel,
                      kRing, kCyan, kText, kBg);
}

/** Two-line card: callsign + route codes, then altitude / speed / distance / climb. */
/** "Corners" style: left panel who / where to, right panel the flight values. */
void drawInfoPanels(int idx, const float* dist_km, const float* bearing_deg) {
  if (idx < 0) return;
  const auto& a = s_planes[idx];
  const uint8_t col = altitudeColor(a.alt_ft);
  char buf[48];

  // Left: callsign, type, route codes, cities.
  int x0 = S(4);
  s_spr.fillRoundRect(x0, kInfoY, kInfoLW, kInfoH, S(6), kPanel);
  s_spr.drawRoundRect(x0, kInfoY, kInfoLW, kInfoH, S(6), kRingDim);
  setFont(&ui::font::SansBold9);
  s_spr.setTextDatum(textdatum_t::top_left);
  s_spr.setTextColor(col);
  s_spr.drawString(a.callsign, x0 + S(8), kInfoY + S(4));
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::top_right);
  s_spr.setTextColor(kTextDim);
  s_spr.drawString(a.type, x0 + kInfoLW - S(8), kInfoY + S(6));
  s_spr.setTextDatum(textdatum_t::top_left);
  services::route::Route rt;
  const services::route::Status rs = services::route::get(a.callsign, &rt);
  if (rs == services::route::Status::Found) {
    snprintf(buf, sizeof(buf), "%s > %s", rt.from_code[0] ? rt.from_code : "?",
             rt.to_code[0] ? rt.to_code : "?");
    s_spr.setTextColor(kText);
    s_spr.drawString(buf, x0 + S(8), kInfoY + S(24));
    if (rt.from_city[0] || rt.to_city[0]) {
      buf[0] = '\0';
      for (int cut = 19; cut >= 3; cut -= 2) {
        // Cut by letters, not bytes: Turkish letters are two bytes in UTF-8.
        snprintf(buf, sizeof(buf), "%.*s > %.*s", utf8Bytes(rt.from_city, cut), rt.from_city,
                 utf8Bytes(rt.to_city, cut), rt.to_city);
        if (static_cast<int>(s_spr.textWidth(buf)) <= kInfoLW - S(16)) break;
        buf[0] = '\0';
      }
      s_spr.setTextColor(kTextDim);
      s_spr.drawString(buf, x0 + S(8), kInfoY + S(41));
    }
  } else {
    s_spr.setTextColor(kTextDim);
    s_spr.drawString(rs == services::route::Status::Pending ? "Rota aranıyor..." : "Rota bilinmiyor",
                     x0 + S(8), kInfoY + S(24));
  }

  // Right: the flight values, each with its name ("Hız: 233 kt"). No distance to home.
  x0 = kInfoRX;
  s_spr.fillRoundRect(x0, kInfoY, kInfoRW, kInfoH, S(6), kPanel);
  s_spr.drawRoundRect(x0, kInfoY, kInfoRW, kInfoH, S(6), kRingDim);
  char alt[12];
  formatAltitude(a.alt_ft, alt, sizeof(alt));
  char spd[16];
  snprintf(spd, sizeof(spd), "%d kt", static_cast<int>(lroundf(a.gs_knots)));
  char vs[20];
  uint8_t vs_col = kText;
  if (a.vrate_fpm > 300) {
    snprintf(vs, sizeof(vs), "+%d fpm", a.vrate_fpm);
    vs_col = kCyan;
  } else if (a.vrate_fpm < -300) {
    snprintf(vs, sizeof(vs), "%d fpm", a.vrate_fpm);
    vs_col = kOrange;
  } else {
    snprintf(vs, sizeof(vs), "seyirde");
  }
  const struct {
    const char* name;
    const char* value;
    uint8_t col;
  } rows[3] = {{"Yükseklik:", alt, col}, {"Hız:", spd, kText}, {"Dikey:", vs, vs_col}};
  setFont(&ui::font::Sans8);
  s_spr.setTextDatum(textdatum_t::top_left);
  for (int r = 0; r < 3; ++r) {
    const int y = kInfoY + S(6) + r * S(18);
    s_spr.setTextColor(kTextDim);
    s_spr.drawString(rows[r].name, x0 + S(8), y);
    s_spr.setTextColor(rows[r].col);
    s_spr.drawString(rows[r].value, x0 + S(8) + s_spr.textWidth(rows[r].name) + S(5), y);
  }
}

void drawEmptyNote(unsigned long fetched_ms) {
  setFont(&ui::font::Sans9);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.setTextColor(kTextDim);
  s_spr.drawString(fetched_ms == 0 ? "Uçak verisi bekleniyor..." : "Menzilde uçak yok", kW / 2,
                   kTopBarH + kStripH + S(24));
  const char* err = services::adsb::lastError();
  if (fetched_ms == 0 && err[0] != '\0' && !services::usb_feed::active()) {
    char line[64];
    snprintf(line, sizeof(line), "Son deneme: %s", err);
    setFont(&ui::font::Sans8);
    s_spr.drawString(line, kW / 2, kTopBarH + kStripH + S(44));
  }
}

uint16_t s_lut565[256];  // palette as the panel's frame buffer stores it (byte-swapped RGB565)

void applyPaletteTo(LGFX_Sprite& g, bool force) {
  auto setPal = [&](int i, uint8_t r, uint8_t gg, uint8_t b) {
    g.setPaletteColor(i, r, gg, b);
    const uint16_t c = static_cast<uint16_t>(((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3));
    s_lut565[i] = static_cast<uint16_t>((c >> 8) | (c << 8));
  };
  // Set every frame: the forecast screen shares the sprite with its own palette.
  const bool hc = services::display_prefs::get().high_contrast;
  const Rgb* pal = hc ? kPaletteHighContrast : kPalette;
  for (int i = 0; i < 16; ++i) setPal(i, pal[i].r, pal[i].g, pal[i].b);
  if (!force && s_palette_set && s_palette_hc == hc) return;  // ramps unchanged
  auto clamp8 = [](float v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
  auto luma = [](const Rgb& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b + 1.0f; };
  // Terrain: band 0 shade 8 is exactly the flat land colour; darker / lighter shades
  // span the old dark / light land tones; higher bands drift to dry grass and rock.
  const Rgb land = pal[kLand];
  const float kd = luma(pal[kLandDark]) / luma(land), kl = luma(pal[kLandLight]) / luma(land);
  const Rgb high = hc ? Rgb{176, 160, 112} : Rgb{132, 120, 84};
  for (int b = 0; b < kTerrainBands; ++b) {
    const float e = static_cast<float>(b) / (kTerrainBands - 1);
    const float t = e * e * 0.9f;  // lowlands stay green
    const float br = land.r + (high.r - land.r) * t, bg = land.g + (high.g - land.g) * t,
                bb = land.b + (high.b - land.b) * t;
    for (int sh = 0; sh < kTerrainShades; ++sh) {
      const float f = sh < 8 ? kd + (1.0f - kd) * sh / 8.0f : 1.0f + (kl - 1.0f) * (sh - 8) / 7.0f;
      setPal(kTerrainBase + b * kTerrainShades + sh, clamp8(br * f), clamp8(bg * f),
                            clamp8(bb * f));
    }
  }
  // Sea: lighter and a little hazier towards the horizon, deeper close by.
  const Rgb sea = pal[kSea], haze = pal[kRingDim];
  for (int i = 0; i < 16; ++i) {
    const float t = i / 15.0f;  // 0 far .. 1 near
    const float hz = (1.0f - t) * 0.35f, dk = 1.08f - 0.22f * t;
    setPal(kSeaBase + i, clamp8((sea.r + (haze.r - sea.r) * hz) * dk),
                          clamp8((sea.g + (haze.g - sea.g) * hz) * dk),
                          clamp8((sea.b + (haze.b - sea.b) * hz) * dk));
  }
  // Aircraft: each altitude colour from deep shadow to a slight highlight.
  const uint8_t alt_cols[3] = {kOrange, kCream, kCyan};
  for (int a = 0; a < 3; ++a) {
    const Rgb c = pal[alt_cols[a]];
    for (int j = 0; j < kPlaneShades; ++j) {
      const float f = 0.32f + 0.78f * j / (kPlaneShades - 1);
      setPal(kPlaneBase + a * kPlaneShades + j, clamp8(c.r * f), clamp8(c.g * f),
                            clamp8(c.b * f));
    }
  }
  {  // built-up (warm grey) and forest (deep green), 8 shades each, lit like the terrain
    const Rgb ub = hc ? Rgb{124, 116, 100} : Rgb{98, 92, 80};
    const Rgb fo = hc ? Rgb{34, 150, 56} : Rgb{20, 112, 40};
    for (int i = 0; i < 8; ++i) {
      const int sh = i * 2 + 1;
      const float f = sh < 8 ? kd + (1.0f - kd) * sh / 8.0f : 1.0f + (kl - 1.0f) * (sh - 8) / 7.0f;
      setPal(kUrbanBase + i, clamp8(ub.r * f), clamp8(ub.g * f), clamp8(ub.b * f));
      setPal(kForestBase + i, clamp8(fo.r * f), clamp8(fo.g * f), clamp8(fo.b * f));
    }
  }
  setPal(kAsphalt, 70, 74, 78);
  setPal(kMarking, 236, 236, 228);
  s_palette_hc = hc;
  s_palette_set = true;
}

void applyPalette() { applyPaletteTo(s_spr, false); }

void animateYaw() {
  if (s_yaw_vel != 0.0f && !s_dragging) {  // glide on after a flick, slowing down
    s_yaw_target = s_yaw_cur = fmodf(s_yaw_cur + s_yaw_vel * s_dt + 360.0f, 360.0f);
    s_yaw_vel *= expf(-s_dt / 0.35f);
    if (fabsf(s_yaw_vel) < 4.0f) {
      s_yaw_vel = 0.0f;
      auto p = services::display_prefs::get();
      p.yaw_deg = static_cast<uint16_t>(lroundf(s_yaw_cur)) % 360;
      services::display_prefs::setNoSave(p);
      s_prefs_dirty_ms = millis();
    }
    return;
  }
  float diff = fmodf(s_yaw_target - s_yaw_cur + 540.0f, 360.0f) - 180.0f;
  // Following: small corrections are taken in one go (see followSelected).
  if (fabsf(diff) < 0.05f || (s_follow && fabsf(diff) < 1.5f)) {
    s_yaw_cur = s_yaw_target;
  } else {
    // Ease with a capped turn rate: a long swing (selecting an aircraft behind)
    // turns at a steady pace instead of a jump, then settles softly.
    float step = diff * ease(0.28f);
    const float max_step = 75.0f * s_dt;  // deg per second: small steps per frame
    step = std::max(-max_step, std::min(max_step, step));
    s_yaw_cur = fmodf(s_yaw_cur + step + 360.0f, 360.0f);
  }
}

}  // namespace

bool init() {
  // Whole-frame sprite for the forecast / water screens: 384 KB in PSRAM.
  s_frame.setColorDepth(8);
  s_frame.setPsram(true);
  if (s_frame.createSprite(kW, kH) == nullptr) {
    Serial.println("radar3d: sprite allocation failed");
    return false;
  }
  s_frame.createPalette();  // 8-bit with a palette (setColorDepth(8) alone is RGB332)
  // The radar draws into the same pixel memory (its own palette).
  s_band_mem = static_cast<uint8_t*>(s_frame.getBuffer());
  s_spr.setColorDepth(8);
  selectBand(0);
  s_spr.createPalette();
  applyPalette();
  s_yaw_cur = s_yaw_target = static_cast<float>(services::display_prefs::get().yaw_deg);
  s_ready = true;
  return true;
}

/** Turn the camera towards the selected aircraft (see s_follow). */
void followSelected(size_t count, unsigned long fetched_ms) {
  if (!s_follow) return;
  if (s_selected[0] == '\0') {  // deselected, or it left the area
    s_follow = false;
    s_yaw_target = static_cast<float>(services::display_prefs::get().yaw_deg);
    return;
  }
  const float R = ui::radar::rangeCurrent().outer_km;
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(s_planes[i].callsign, s_selected) != 0) continue;
    float x, y;
    currentPositionKm(s_planes[i], fetched_ms, &x, &y);
    // Right over home the bearing is meaningless: leave the view as it is.
    if (x * x + y * y < (0.12f * R) * (0.12f * R)) return;
    // yaw = bearing would put it straight ahead (behind home); the opposite heading
    // brings it to the front, between home and the viewer.
    float b = atan2f(x, y) / kDegToRad + 180.0f;
    if (b >= 360.0f) b -= 360.0f;
    // Followed in very small steps (~0.3 deg, a pixel or two at the rim), taken at
    // once: it reads as a smooth follow, and between steps the camera stands still so
    // the frames come from the cached ground instead of being redrawn every time.
    const float off = fabsf(fmodf(b - s_yaw_target + 540.0f, 360.0f) - 180.0f);
    if (off >= 0.3f) s_yaw_target = b;
    return;
  }
}

// Markers before the aircraft (so they pass over them); names after the aircraft
// labels, only where they leave room (aircraft names always win).
void drawRunway(const Cam& c, const Runway& r) {
  // A strip lying on the ground: asphalt with a dark edge, white threshold bars at
  // both ends and a dashed centre line. Wider than life so it reads at radar scale.
  const float dx = r.x1 - r.x0, dy = r.y1 - r.y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 0.2f) return;
  const float ux = dx / len, uy = dy / len, half_w = std::max(0.06f, c.R * 0.0045f);
  const float px = -uy * half_w, py = ux * half_w;
  const float gz = groundZ(c, 0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1));
  float sx[4], sy[4];
  const float cx[4] = {r.x0 + px, r.x1 + px, r.x1 - px, r.x0 - px};
  const float cy[4] = {r.y0 + py, r.y1 + py, r.y1 - py, r.y0 - py};
  for (int k = 0; k < 4; ++k) {
    if (!project(c, cx[k], cy[k], gz, &sx[k], &sy[k])) return;
  }
  for (int k = 0; k < 4; ++k) s_spr.drawLine(ri(sx[k]), ri(sy[k]), ri(sx[(k + 1) % 4]), ri(sy[(k + 1) % 4]), kBg);
  s_spr.fillTriangle(ri(sx[0]), ri(sy[0]), ri(sx[1]), ri(sy[1]), ri(sx[2]), ri(sy[2]), kAsphalt);
  s_spr.fillTriangle(ri(sx[0]), ri(sy[0]), ri(sx[2]), ri(sy[2]), ri(sx[3]), ri(sy[3]), kAsphalt);
  // Threshold bars (across each end) and the centre line, dashed.
  for (int end = 0; end < 2; ++end) {
    const float t = end == 0 ? 0.06f : 0.94f;
    float ax, ay, bx, by;
    const float mx = r.x0 + dx * t, my = r.y0 + dy * t;
    if (project(c, mx + px * 0.7f, my + py * 0.7f, gz, &ax, &ay) &&
        project(c, mx - px * 0.7f, my - py * 0.7f, gz, &bx, &by)) {
      s_spr.drawLine(ri(ax), ri(ay), ri(bx), ri(by), kMarking);
    }
  }
  float ax, ay, bx, by;
  if (project(c, r.x0 + dx * 0.12f, r.y0 + dy * 0.12f, gz, &ax, &ay) &&
      project(c, r.x0 + dx * 0.88f, r.y0 + dy * 0.88f, gz, &bx, &by)) {
    dottedLine(ri(ax), ri(ay), ri(bx), ri(by), kMarking, 3, 3);
  }
}

/** Airport badge: dark rounded square with a control tower and terminal drawn in
 *  light lines (no aircraft, so it is not mistaken for traffic), raised on a short
 *  stem above the airport so its runways stay visible underneath. */
constexpr int kBadgeLift = S(18);
void drawAirportBadge(int x, int gy) {
  const int y = gy - kBadgeLift;  // badge centre
  const float u = ui::Sf(1.0f);  // one design pixel
  auto X = [&](float v) { return x + ri(v * u); };
  auto Y = [&](float v) { return y + ri(v * u); };
  const int hw = ri(11 * u), hh = ri(10 * u);
  s_spr.drawLine(x, y + hh, x, gy, kBg);
  s_spr.drawLine(x + 1, y + hh, x + 1, gy, kText);
  s_spr.fillCircle(x + 1, gy, S(1), kText);
  s_spr.fillRoundRect(x - hw - 1, y - hh - 1, 2 * hw + 3, 2 * hh + 3, S(4), kBg);
  s_spr.fillRoundRect(x - hw, y - hh, 2 * hw + 1, 2 * hh + 1, S(4), kPanel);
  s_spr.drawRoundRect(x - hw, y - hh, 2 * hw + 1, 2 * hh + 1, S(4), kTextDim);
  // Tower: antenna, cab (wider, with a window band), shaft with two windows.
  s_spr.drawLine(X(-4), Y(-8), X(-4), Y(-6), kText);
  s_spr.fillRect(X(-8), Y(-6), ri(9 * u), ri(4 * u), kText);
  s_spr.fillRect(X(-7), Y(-5), ri(7 * u), ri(1.5f * u), kPanel);
  s_spr.drawRect(X(-6), Y(-2), ri(5 * u), ri(9 * u), kText);
  s_spr.fillRect(X(-5), Y(0), ri(3 * u), ri(1.5f * u), kText);
  s_spr.fillRect(X(-5), Y(3), ri(3 * u), ri(1.5f * u), kText);
  // Terminal: low building with a sloping roof and a row of windows.
  s_spr.drawLine(X(-1), Y(1), X(8), Y(-1), kText);
  s_spr.drawLine(X(8), Y(-1), X(8), Y(7), kText);
  s_spr.drawLine(X(-1), Y(1), X(-1), Y(7), kText);
  for (int k = 0; k < 3; ++k) s_spr.fillRect(X(0.5f + k * 2.6f), Y(3), ri(1.6f * u), ri(2 * u), kText);
  // Ground line.
  s_spr.fillRect(X(-9), Y(7), ri(18 * u), ri(1.5f * u), kText);
}

/** Landmark: a small round badge with a category symbol (no name), on a short stem.
 *  cat: 1 stadium, 2 zoo, 3 tower, 4 castle / historic, 5 bridge, 6 museum,
 *  7 park, 8 place of worship; anything else is a plain diamond. */
constexpr int kIconLift = S(13);
void drawLandmarkIcon(int x, int gy, uint8_t cat) {
  if (cat < 1 || cat > 8) {
    const int d = S(4);
    s_spr.fillTriangle(x, gy - d - 1, x - d - 1, gy, x + d + 1, gy, kBg);
    s_spr.fillTriangle(x, gy + d + 1, x - d - 1, gy, x + d + 1, gy, kBg);
    s_spr.fillTriangle(x, gy - d, x - d, gy, x + d, gy, kCream);
    s_spr.fillTriangle(x, gy + d, x - d, gy, x + d, gy, kCream);
    return;
  }
  const int y = gy - kIconLift;
  const float u = ui::Sf(1.0f);
  auto X = [&](float v) { return x + ri(v * u); };
  auto Y = [&](float v) { return y + ri(v * u); };
  auto L = [&](float v) { return std::max(1, ri(v * u)); };
  const int r = ri(8.5f * u);
  s_spr.drawLine(x, y + r, x, gy, kBg);
  s_spr.drawLine(x + 1, y + r, x + 1, gy, kTextDim);
  s_spr.fillCircle(x + 1, gy, S(1), kTextDim);
  s_spr.fillCircle(x, y, r + 1, kBg);
  s_spr.fillCircle(x, y, r, kPanel);
  s_spr.drawCircle(x, y, r, kTextDim);
  const uint8_t k = kCream;
  switch (cat) {
    case 1:  // stadium: oval bowl with the pitch inside
      s_spr.drawEllipse(x, y, L(5.5f), L(3.6f), k);
      s_spr.drawEllipse(x, y, L(4.5f), L(2.6f), k);
      s_spr.fillRect(X(-2), Y(-1), L(4), L(2), k);
      break;
    case 2:  // zoo: paw print
      s_spr.fillEllipse(x, Y(1.8f), L(2.6f), L(2.1f), k);
      s_spr.fillCircle(X(-3.4f), Y(-1.6f), L(1.2f), k);
      s_spr.fillCircle(X(-1.2f), Y(-3.6f), L(1.2f), k);
      s_spr.fillCircle(X(1.2f), Y(-3.6f), L(1.2f), k);
      s_spr.fillCircle(X(3.4f), Y(-1.6f), L(1.2f), k);
      break;
    case 3:  // tower: mast, pod, tapering shaft
      s_spr.drawLine(x, Y(-6), x, Y(-3), k);
      s_spr.fillEllipse(x, Y(-2), L(2.2f), L(1.3f), k);
      s_spr.fillTriangle(X(-0.8f), Y(-1), X(0.8f), Y(-1), X(2.2f), Y(5), k);
      s_spr.fillTriangle(X(-0.8f), Y(-1), X(-2.2f), Y(5), X(2.2f), Y(5), k);
      break;
    case 4:  // castle: wall with battlements and a gate
      s_spr.fillRect(X(-5), Y(-1), L(10), L(6), k);
      for (int i = 0; i < 3; ++i) s_spr.fillRect(X(-5 + i * 4), Y(-4), L(2), L(3), k);
      s_spr.fillRect(X(-1), Y(2), L(2), L(3), kPanel);
      break;
    case 5:  // bridge: deck, two pylons, cables
      s_spr.fillRect(X(-6), Y(2), L(12), L(1.5f), k);
      s_spr.fillRect(X(-3.5f), Y(-4), L(1.4f), L(8), k);
      s_spr.fillRect(X(2.1f), Y(-4), L(1.4f), L(8), k);
      s_spr.drawLine(X(-6), Y(1), X(-3), Y(-4), k);
      s_spr.drawLine(X(-3), Y(-4), X(0), Y(0.5f), k);
      s_spr.drawLine(X(0), Y(0.5f), X(3), Y(-4), k);
      s_spr.drawLine(X(3), Y(-4), X(6), Y(1), k);
      break;
    case 6:  // museum: pediment, columns, base
      s_spr.fillTriangle(X(-5.5f), Y(-2), X(5.5f), Y(-2), x, Y(-5.5f), k);
      for (int i = 0; i < 4; ++i) s_spr.fillRect(X(-4.5f + i * 2.8f), Y(-1), L(1.4f), L(4.5f), k);
      s_spr.fillRect(X(-5.5f), Y(4), L(11), L(1.4f), k);
      break;
    case 7:  // park: tree
      s_spr.fillCircle(X(-2.2f), Y(-0.5f), L(2.6f), k);
      s_spr.fillCircle(X(2.2f), Y(-0.5f), L(2.6f), k);
      s_spr.fillCircle(x, Y(-3), L(2.9f), k);
      s_spr.fillRect(X(-0.8f), Y(1.5f), L(1.7f), L(4), k);
      s_spr.fillRect(X(-3), Y(5), L(6), L(1.2f), k);
      break;
    case 8:  // place of worship: dome, finial and a minaret
      s_spr.fillCircle(X(-1), Y(1), L(3.5f), k);
      s_spr.fillRect(X(-4.5f), Y(1), L(7), L(4), k);
      s_spr.drawLine(X(-1), Y(-2.5f), X(-1), Y(-4.5f), k);
      s_spr.fillRect(X(3.8f), Y(-4), L(1.4f), L(9), k);
      s_spr.fillTriangle(X(3.6f), Y(-4), X(5.4f), Y(-4), X(4.5f), Y(-6.5f), k);
      break;
  }
}

/** Name of a tapped landmark: a small card just above its symbol. */
void drawPoiName(const char* name, int x, int y) {
  setFont(&ui::font::SansBold9);
  const int w = s_spr.textWidth(name) + S(12), h = S(20);
  int bx = std::max(S(2), std::min(kW - w - S(2), x - w / 2));
  int by = y - S(11) - h;
  if (by < kTopBarH + S(2)) by = y + S(12);  // no room above: below the symbol
  s_spr.fillRoundRect(bx - 1, by - 1, w + 2, h + 2, S(5), kBg);
  s_spr.fillRoundRect(bx, by, w, h, S(5), kPanel);
  s_spr.drawRoundRect(bx, by, w, h, S(5), kCream);
  s_spr.setTextColor(kText);
  s_spr.setTextDatum(textdatum_t::middle_center);
  s_spr.drawString(name, bx + w / 2, by + h / 2);
  s_spr.setTextDatum(textdatum_t::top_left);
}

void drawPois(const Cam& c, bool names) {
  loadPois();
  if (!names) memset(s_poi_vis, 0, sizeof(s_poi_vis));
  if (!names) {
    for (size_t i = 0; i < s_rwy_count; ++i) {
      const Runway& r = s_rwys[i];
      const float mx = 0.5f * (r.x0 + r.x1), my = 0.5f * (r.y0 + r.y1);
      if (mx * mx + my * my <= c.R * c.R * 0.95f) drawRunway(c, r);
    }
  }
  for (int pass = 0; pass < 2; ++pass) {  // airports first, then landmarks
    for (size_t i = 0; i < s_poi_count; ++i) {
      const Poi& p = s_pois[i];
      if (p.kind != pass || p.x * p.x + p.y * p.y > c.R * c.R * 0.95f) continue;
      if (fabsf(p.x) < 0.3f && fabsf(p.y) < 0.3f) continue;  // home has its own marker
      float sx, sy;
      if (!project(c, p.x, p.y, groundZ(c, p.x, p.y), &sx, &sy)) continue;
      const int x = ri(sx), y = ri(sy);
      if (p.kind == 0) {
        if (!names) {
          drawAirportBadge(x, y);
          continue;
        }
        setFont(&ui::font::SansBold9);
        const int w = s_spr.textWidth(p.name);
        const int by = y - kBadgeLift;
        const Box b{x + S(14), by - S(8), w + S(2), S(16)};
        if (claimLabel(b)) haloText(p.name, x + S(15), by - S(8), kText);
      } else if (p.kind == 1) {
        if (!services::display_prefs::get().places) continue;
        if (!names) {
          drawLandmarkIcon(x, y, p.cat);  // symbol only: the name shows when it is tapped
          s_poi_vis[i] = true;
          s_poi_sx[i] = static_cast<int16_t>(x);
          s_poi_sy[i] = static_cast<int16_t>(p.cat >= 1 && p.cat <= 8 ? y - kIconLift : y);
          continue;
        }
        if (static_cast<int>(i) == s_poi_open && millis() < s_poi_open_until && s_poi_vis[i]) {
          drawPoiName(p.name, s_poi_sx[i], s_poi_sy[i]);
        }
      }  // kind 2 (district / town names): not drawn, they crowded the map
    }
  }
  s_spr.setTextDatum(textdatum_t::top_left);
}

// ---------------------------------------------------------------- static layer cache
//
// Ground, map, rings and place markers only change when the camera (or the map, or a
// display option) changes. They are kept in a second 8-bit buffer and copied back in
// one go on frames where nothing moved: every pixel the ground stage writes goes out to
// PSRAM, so redrawing it each frame cost far more than one copy.
uint8_t* s_bg = nullptr;
struct BgKey {
  float pitch, cyaw, syaw, R, f, cx, cy, tscale;
  uint32_t mgen, tgen;
  uint8_t rings, miles, card, places;
  double lat, lon;
};
BgKey s_bg_key;
bool s_bg_valid = false;

BgKey bgKeyFor(const Cam& c) {
  BgKey k;
  memset(&k, 0, sizeof(k));
  k.pitch = c.pitch_deg; k.cyaw = c.cyaw; k.syaw = c.syaw; k.R = c.R;
  k.f = c.f; k.cx = c.cx; k.cy = c.cy; k.tscale = c.tscale;
  k.mgen = services::map::generation();
  k.tgen = services::terrain::generation();
  k.rings = ui::radar::showRings();
  k.miles = ui::radar::useMiles();
  k.card = s_selected[0] != '\0' && !infoInLabel();
  k.places = services::display_prefs::get().places;
  k.lat = services::location::lat();
  k.lon = services::location::lon();
  return k;
}

// ---------------------------------------------------------------- changed-tile push
//
// The screen is split into 32 x 24 tiles. Each frame a hash of every tile is taken;
// only tiles whose hash differs from what a panel buffer already holds are converted
// and written to it (three buffers, each with its own hash table). With the camera
// still that is the aircraft, their labels and a few changing texts, not the 768 KB
// of a whole frame. The tiles that differ from the cached static layer are the ones
// put back from it at the start of the next frame.
constexpr int kTileW = 32, kTileH = 24;
static_assert(kW % kTileW == 0 && kH % kTileH == 0, "tile grid must divide the screen");
constexpr int kTilesX = kW / kTileW, kTilesY = kH / kTileH, kTiles = kTilesX * kTilesY;
uint32_t* s_tile_hash = nullptr;  // current frame
uint32_t* s_bg_hash = nullptr;    // static layer cache
uint32_t* s_fb_hash[3] = {nullptr, nullptr, nullptr};
bool s_fb_valid[3] = {false, false, false};
uint8_t s_restore[kTiles];  // tile differs from the static layer
bool s_restore_known = false;
bool s_tiles_ok = false;

bool allocTiles() {
  if (s_tiles_ok) return true;
  auto alloc = []() {
#if defined(RADAR3D_HOST_TEST)
    return static_cast<uint32_t*>(malloc(kTiles * sizeof(uint32_t)));
#else
    return static_cast<uint32_t*>(heap_caps_malloc(kTiles * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
#endif
  };
  s_tile_hash = alloc();
  s_bg_hash = alloc();
  for (auto& h : s_fb_hash) h = alloc();
  s_tiles_ok = s_tile_hash && s_bg_hash && s_fb_hash[0] && s_fb_hash[1] && s_fb_hash[2];
  return s_tiles_ok;
}

void hashTiles(const uint8_t* f, uint32_t* out) {
  uint32_t acc[kTilesX];
  for (int ty = 0; ty < kTilesY; ++ty) {
    for (int tx = 0; tx < kTilesX; ++tx) acc[tx] = 2166136261u;
    for (int r = 0; r < kTileH; ++r) {
      const uint32_t* w = reinterpret_cast<const uint32_t*>(f + static_cast<size_t>(ty * kTileH + r) * kW);
      for (int tx = 0; tx < kTilesX; ++tx, w += kTileW / 4) {
        uint32_t h = acc[tx];
        // Multiply-and-rotate per word. (A plain rotate-xor hash gave every tile of one
        // colour the same value as an all-black tile when the colour had an even number
        // of set bits, so such tiles were never updated: leftover blocks on screen.)
        for (int k = 0; k < kTileW / 4; ++k) {
          h = (h ^ w[k]) * 0x9E3779B1u;
          h = (h << 13) | (h >> 19);
        }
        acc[tx] = h;
      }
    }
    for (int tx = 0; tx < kTilesX; ++tx) out[ty * kTilesX + tx] = acc[tx];
  }
}

/** Copy the flagged tiles (runs along each tile row) from src to dst. */
template <typename Pred>
void copyTileRuns(uint8_t* dst, const uint8_t* src, Pred flagged) {
  for (int ty = 0; ty < kTilesY; ++ty) {
    for (int tx = 0; tx < kTilesX;) {
      if (!flagged(ty * kTilesX + tx)) { ++tx; continue; }
      int te = tx;
      while (te < kTilesX && flagged(ty * kTilesX + te)) ++te;
      const size_t x0 = static_cast<size_t>(tx) * kTileW, wbytes = static_cast<size_t>(te - tx) * kTileW;
      for (int r = 0; r < kTileH; ++r) {
        const size_t off = static_cast<size_t>(ty * kTileH + r) * kW + x0;
        memcpy(dst + off, src + off, wbytes);
      }
      tx = te;
    }
  }
}

#if !defined(RADAR3D_HOST_TEST) || defined(RADAR3D_HOST_FBCHECK)
/** Convert the flagged tiles through the colour table into a panel buffer. */
template <typename Pred>
int pushTileRuns(uint16_t* fb, bool flip, Pred flagged) {
  static uint16_t line[kW];
  int pushed = 0;
  for (int ty = 0; ty < kTilesY; ++ty) {
    for (int tx = 0; tx < kTilesX;) {
      if (!flagged(ty * kTilesX + tx)) { ++tx; continue; }
      int te = tx;
      while (te < kTilesX && flagged(ty * kTilesX + te)) ++te;
      pushed += te - tx;
      const int xs = tx * kTileW, n = (te - tx) * kTileW;
      for (int r = 0; r < kTileH; ++r) {
        const int y = ty * kTileH + r;
        const uint8_t* src = s_band_mem + static_cast<size_t>(y) * kW + xs;
        if (!flip) {
          for (int x = 0; x < n; ++x) line[x] = s_lut565[src[x]];
          memcpy(fb + static_cast<size_t>(y) * kW + xs, line, n * sizeof(uint16_t));
        } else {
          for (int x = 0; x < n; ++x) line[n - 1 - x] = s_lut565[src[x]];
          memcpy(fb + static_cast<size_t>(kH - 1 - y) * kW + (kW - xs - n), line, n * sizeof(uint16_t));
        }
      }
      tx = te;
    }
  }
  return pushed;
}
#endif

void drawFrame() {
  if (!s_ready) return;
  {
    const unsigned long now = millis();
    // Another screen (forecast, settings, ...) was shown: the panel buffers and the
    // shared frame no longer hold what the tile tables say.
    if (s_last_frame_ms == 0 || now - s_last_frame_ms > 300) {
      s_fb_valid[0] = s_fb_valid[1] = s_fb_valid[2] = false;
      s_restore_known = false;
    }
    s_dt = s_last_frame_ms == 0 ? 0.05f : std::min(0.25f, (now - s_last_frame_ms) / 1000.0f);
    s_last_frame_ms = now;
#ifdef RADAR3D_HOST_TEST
    s_dt = 0.1f;  // the preview's clock does not run
#endif
  }
  if (s_prefs_dirty_ms != 0 && millis() - s_prefs_dirty_ms > 4000) {
    services::display_prefs::save(services::display_prefs::get());
    s_prefs_dirty_ms = 0;
  }
  applyPalette();
  unsigned long fetched_ms = 0;
  const size_t count =
      services::adsb::snapshotAircraft(s_planes, services::adsb::kMaxAircraft, &fetched_ms);
  followSelected(count, fetched_ms);
  animateYaw();
  const Cam c = makeCam();
  allocTiles();

  static float dist_km[services::adsb::kMaxAircraft];
  static float bearing[services::adsb::kMaxAircraft];

  // us per stage, summed for the serial report: 0 ground, 1 lines, 2 bg save,
  // 3 restore, 4 aircraft, 5 ui, 6 hash, 7 push
  static uint32_t st[8];
  static uint32_t n_cached = 0, n_moving = 0, n_tiles = 0;
  int fbi = 0;
#if !defined(RADAR3D_HOST_TEST)
  tft.beginHiddenFrame();
  fbi = tft.hiddenIndex();
#endif
  s_first_band = true;
  selectBand(0);
  uint32_t t = micros();
  const BgKey bk = bgKeyFor(c);
  const bool same = s_bg_valid && memcmp(&bk, &s_bg_key, sizeof(bk)) == 0;
  if (same) {
#if defined(RADAR3D_NO_RESTORE)
    s_restore_known = false;  // host check: full copy, for comparing with the tile path
#endif
    if (s_restore_known) {
      copyTileRuns(s_band_mem, s_bg, [](int i) { return s_restore[i] != 0; });
    } else {
      memcpy(s_band_mem, s_bg, static_cast<size_t>(kW) * kH);
    }
    ++n_cached;
    st[3] += micros() - t; t = micros();
  } else {
    s_rebuilt = false;
    drawSurface(c);
    st[0] += micros() - t;
    if (s_rebuilt) ++n_moving;
    t = micros();
    drawMap(c);
    drawRings(c);
    drawPois(c, false);
    st[1] += micros() - t; t = micros();
    if (s_bg == nullptr) {
      s_bg = static_cast<uint8_t*>(malloc(static_cast<size_t>(kW) * kH));  // large: lands in PSRAM
    }
    // Kept only once the camera has settled: while it turns every frame differs.
    // Kept only when the camera stood still since the previous frame too: saving costs
    // a copy and a hash (~25 ms), wasted if it moves on in the next frame.
    // Kept once the camera has reached its target (it stands still from here on).
    if (s_bg != nullptr && s_tiles_ok && fabsf(s_yaw_cur - s_yaw_target) < 0.01f && !s_dragging &&
        s_yaw_vel == 0.0f) {
      memcpy(s_bg, s_band_mem, static_cast<size_t>(kW) * kH);
      hashTiles(s_bg, s_bg_hash);
      s_bg_key = bk;
      s_bg_valid = true;
    } else {
      s_bg_valid = false;
    }
    st[2] += micros() - t; t = micros();
  }
  int card_idx = -1;
  drawAircraft(c, count, fetched_ms, dist_km, bearing, &card_idx);
  drawPois(c, true);
  st[4] += micros() - t; t = micros();
  size_t in_range = 0;
  for (size_t i = 0; i < count; ++i) {
    if (dist_km[i] <= c.R) ++in_range;
  }
  drawTopBar(in_range, fetched_ms);
  if (in_range == 0) drawEmptyNote(fetched_ms);
  drawControls();
  if (s_selected[0] == '\0') {
    drawLegendStrip();  // nothing selected: legend strip, no card
  } else if (!infoInLabel()) {
    drawInfoPanels(card_idx, dist_km, bearing);
  }
  st[5] += micros() - t; t = micros();

  // Static layer in place under this frame: find the changed tiles.
  const bool tiles = s_bg_valid && s_tiles_ok;
  if (tiles) {
    hashTiles(s_band_mem, s_tile_hash);
    for (int i = 0; i < kTiles; ++i) s_restore[i] = s_tile_hash[i] != s_bg_hash[i];
    s_restore_known = true;
  } else {
    s_restore_known = false;
  }
  st[6] += micros() - t; t = micros();
#if defined(RADAR3D_HOST_FBCHECK)
  // Host check: three emulated panel buffers, used in turn, compared with the frame.
  static uint16_t* host_fb[3] = {nullptr, nullptr, nullptr};
  static int host_n = 0;
  for (auto& b : host_fb) if (!b) b = static_cast<uint16_t*>(calloc(kW * kH, 2));
  fbi = host_n++ % 3;
  {
    uint16_t* fb = host_fb[fbi];
    const bool flip = false;
#elif !defined(RADAR3D_HOST_TEST)
  {
    uint16_t* fb = tft.hiddenBuffer();
    const bool flip = tft.getRotation() == 2;
#endif
#if defined(RADAR3D_HOST_FBCHECK) || !defined(RADAR3D_HOST_TEST)
    if (tiles) {
      const bool known = s_fb_valid[fbi];
      uint32_t* fh = s_fb_hash[fbi];
      n_tiles += pushTileRuns(fb, flip, [&](int i) { return !known || fh[i] != s_tile_hash[i]; });
      memcpy(fh, s_tile_hash, kTiles * sizeof(uint32_t));
      s_fb_valid[fbi] = true;
    } else {
      n_tiles += pushTileRuns(fb, flip, [](int) { return true; });
      s_fb_valid[fbi] = false;
    }
#if defined(RADAR3D_HOST_FBCHECK)
    int bad = 0;
    for (int i = 0; i < kW * kH; ++i) bad += fb[i] != s_lut565[s_band_mem[i]];
    if (bad) fprintf(stderr, "FBCHECK: frame %d buffer %d: %d stale pixels\n", host_n, fbi, bad);
#endif
  }
  st[7] += micros() - t;
#endif
#ifdef RADAR3D_HOST_TEST
  extern void hostCaptureFrame(LGFX_Sprite & spr);  // desktop preview harness
  applyPaletteTo(s_frame, true);
  hostCaptureFrame(s_frame);
#else
  tft.presentHiddenFrame();
#endif
  {  // frame-rate report on the serial log every 30 s
    static unsigned long t0 = 0;
    static uint32_t n = 0;
    ++n;
    if (millis() - t0 >= 30000) {
      uint32_t panel_hz = 0, panel_waits = 0;
#if !defined(RADAR3D_HOST_TEST)
      {  // panel refresh rate and frames that waited for a free buffer, since the last report
        static uint32_t v0 = 0, w0 = 0;
        const uint32_t v = tft.vsyncCount(), w = tft.bufferWaits();
        if (t0 != 0) { panel_hz = (v - v0) * 1000 / std::max<unsigned long>(1, millis() - t0); panel_waits = w - w0; }
        v0 = v; w0 = w;
      }
#endif
      if (t0 != 0 && n) {
        const uint32_t nd = std::max<uint32_t>(1, n - n_cached);  // frames that drew the ground
        const uint32_t nc = std::max<uint32_t>(1, n_cached);
        Serial.printf("radar3d: %.1f fps; still %lu/%lu frames (camera moving %lu); "
                      "drawn frames ms: ground %lu, lines %lu, bg-save %lu | still frames ms: restore %lu | "
                      "all ms: aircraft %lu, ui %lu, hash %lu, push %lu (%lu tiles of %d) | "
                      "ground split ms: reproject %lu, relief %lu, fill %lu | panel %lu Hz, waits %lu\n",
                      n * 1000.0f / (millis() - t0), (unsigned long)n_cached, (unsigned long)n,
                      (unsigned long)n_moving, st[0] / nd / 1000, st[1] / nd / 1000, st[2] / nd / 1000,
                      st[3] / nc / 1000, st[4] / n / 1000, st[5] / n / 1000, st[6] / n / 1000,
                      st[7] / n / 1000, (unsigned long)(n_tiles / n), kTiles,
                      s_gt[0] / std::max<uint32_t>(1, n_moving) / 1000, s_gt[1] / nd / 1000, s_gt[2] / nd / 1000,
                      (unsigned long)panel_hz, (unsigned long)panel_waits);
      }
      t0 = millis();
      for (auto& v : st) v = 0;
      n = 0;
      n_cached = n_moving = n_tiles = 0;
      s_gt[0] = s_gt[1] = s_gt[2] = 0;
    }
  }
}

LGFX_Sprite* frame() { return s_ready ? &s_frame : nullptr; }

void releaseFrame() {}
bool reacquireFrame() { return true; }

#ifdef RADAR3D_HOST_TEST
void hostTurn(float deg) { s_follow = false; s_yaw_target = fmodf(s_yaw_target + deg + 360.0f, 360.0f); }
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
  if (y < kTopBarH + S(6)) {
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
    s_follow = true;
    return true;
  }
  {  // a landmark symbol: show its name for a few seconds (tap again to hide)
    int hit = -1;
    int hit_d2 = S(18) * S(18);
    for (size_t i = 0; i < s_poi_count; ++i) {
      if (!s_poi_vis[i]) continue;
      const int dx = s_poi_sx[i] - x, dy = s_poi_sy[i] - y, d2 = dx * dx + dy * dy;
      if (d2 <= hit_d2) { hit_d2 = d2; hit = static_cast<int>(i); }
    }
    if (hit >= 0) {
      const bool open = hit == s_poi_open && millis() < s_poi_open_until;
      s_poi_open = open ? -1 : hit;
      s_poi_open_until = millis() + 6000;
      return true;
    }
  }
  const bool on_info = s_selected[0] != '\0' && !infoInLabel() && y >= kInfoY &&
                       y < kInfoY + kInfoH && (x < S(4) + kInfoLW || x >= kInfoRX);
  if (on_info) return true;  // the panels themselves do nothing
  if (s_selected[0] != '\0') {
    s_selected[0] = '\0';  // tap on empty sky: back to "nearest"
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- touch gestures
constexpr float kDragDegPerPx = 0.35f;     // a full screen width turns ~280 deg
constexpr float kTiltDegPerPx = 0.05f;     // ~240 px from flattest to steepest

void dragBegin() {
  s_dragging = true;
  s_follow = false;  // turning by hand ends following the selected aircraft
  s_yaw_vel = 0.0f;
  s_yaw_target = s_yaw_cur;
}

void dragBy(int dx, int dy) {
  if (!s_dragging) dragBegin();
  // The map follows the finger (direction checked on the device: + dx here).
  s_yaw_cur = s_yaw_target = fmodf(s_yaw_cur + dx * kDragDegPerPx + 360.0f, 360.0f);
  // Dragging down looks more from above, up flatter.
  if (s_pitch_cur < 0.0f) s_pitch_cur = services::display_prefs::get().pitch_deg;
  s_pitch_cur = std::max<float>(kPitchMinDeg, std::min<float>(kPitchMaxDeg, s_pitch_cur + dy * kTiltDegPerPx));
}

void dragEnd(float vx_px_s) {
  if (!s_dragging) return;
  s_dragging = false;
  s_yaw_vel = std::max(-300.0f, std::min(300.0f, vx_px_s * kDragDegPerPx));
  if (fabsf(s_yaw_vel) < 20.0f) s_yaw_vel = 0.0f;  // a slow release just stops
  auto p = services::display_prefs::get();
  p.yaw_deg = static_cast<uint16_t>(lroundf(s_yaw_cur)) % 360;
  p.pitch_deg = static_cast<uint8_t>(lroundf(s_pitch_cur));
  s_pitch_cur = p.pitch_deg;  // settle on a whole degree (what is saved)
  services::display_prefs::setNoSave(p);
  s_prefs_dirty_ms = millis();  // written to flash once left alone for a few seconds
}

void pinchStep(int dir) { ui::radar::rangeStep(dir); }

}  // namespace ui::radar3d
