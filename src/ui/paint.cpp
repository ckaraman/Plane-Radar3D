#include "ui/paint.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui::paint {

namespace {

// 4 x 4 Bayer matrix, 0..15.
constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void blendPx(uint16_t* p, int x, int y, const Rgb& c, int a) {
  if (a <= 0) return;
  if (a >= 255) {
    *p = pack(c.r, c.g, c.b, x, y);
    return;
  }
  int r, g, b;
  unpack(*p, &r, &g, &b);
  r += ((c.r - r) * a) / 255;
  g += ((c.g - g) * a) / 255;
  b += ((c.b - b) * a) / 255;
  *p = pack(r, g, b, x, y);
}

/** Signed distance from (px, py) to a rounded rectangle (centre cx, cy; half size hw, hh). */
inline float sdRoundRect(float px, float py, float cx, float cy, float hw, float hh, float r) {
  const float qx = fabsf(px - cx) - (hw - r);
  const float qy = fabsf(py - cy) - (hh - r);
  const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  const float outside = sqrtf(ox * ox + oy * oy);
  const float inside = std::min(std::max(qx, qy), 0.0f);
  return outside + inside - r;
}

struct Stop {
  float t;
  float r, g, b;
};

void stopsAt(const Stop* s, int n, float t, float* r, float* g, float* b) {
  if (t <= s[0].t) {
    *r = s[0].r; *g = s[0].g; *b = s[0].b;
    return;
  }
  for (int i = 1; i < n; ++i) {
    if (t <= s[i].t) {
      const float u = (t - s[i - 1].t) / (s[i].t - s[i - 1].t);
      *r = s[i - 1].r + (s[i].r - s[i - 1].r) * u;
      *g = s[i - 1].g + (s[i].g - s[i - 1].g) * u;
      *b = s[i - 1].b + (s[i].b - s[i - 1].b) * u;
      return;
    }
  }
  *r = s[n - 1].r; *g = s[n - 1].g; *b = s[n - 1].b;
}

/** Smooth glow: alpha a0 at the centre, 0 at `reach` (in ellipse radii), eased. */
inline float glow(float x, float y, float cx, float cy, float rx, float ry, float reach) {
  const float dx = (x - cx) / rx, dy = (y - cy) / ry;
  const float d = sqrtf(dx * dx + dy * dy) / reach;
  if (d >= 1.0f) return 0.0f;
  const float u = 1.0f - d;
  return u * u * (3.0f - 2.0f * u);  // smoothstep: no visible edge where it ends
}

}  // namespace

uint16_t pack(int r, int g, int b, int x, int y) {
  const int d = kBayer[y & 3][x & 3];  // 0..15
  // 5-bit channels step by 8, 6-bit by 4: add a threshold below one step, then cut.
  int r5 = (clamp8(r) * 31 + (d * 255) / 16) / 255;
  int g6 = (clamp8(g) * 63 + (d * 255) / 16) / 255;
  int b5 = (clamp8(b) * 31 + (d * 255) / 16) / 255;
  if (r5 > 31) r5 = 31;
  if (g6 > 63) g6 = 63;
  if (b5 > 31) b5 = 31;
  const uint16_t v = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
  return static_cast<uint16_t>((v >> 8) | (v << 8));
}

void unpack(uint16_t p, int* r, int* g, int* b) {
  const uint16_t v = static_cast<uint16_t>((p >> 8) | (p << 8));
  const int r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
  *r = (r5 * 255 + 15) / 31;
  *g = (g6 * 255 + 31) / 63;
  *b = (b5 * 255 + 15) / 31;
}

Rgb ramp(Rgb c0, Rgb c1, Rgb c2, float t) {
  t = clampf(t, 0.0f, 1.0f);
  auto mix = [](Rgb a, Rgb b, float u) {
    return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * u), static_cast<uint8_t>(a.g + (b.g - a.g) * u),
               static_cast<uint8_t>(a.b + (b.b - a.b) * u)};
  };
  return t < 0.5f ? mix(c0, c1, t * 2.0f) : mix(c1, c2, (t - 0.5f) * 2.0f);
}

float shadeAlpha(const Shade& s, int y) {
  if (y < s.y0 || y >= s.y1) return 0.0f;
  float t = static_cast<float>(y - s.y0) / std::max(1, s.y1 - s.y0 - 1);
  t = t * t * (3.0f - 2.0f * t);
  return (s.a0 + (s.a1 - s.a0) * t) / 255.0f;
}

uint16_t packShaded(float r, float g, float b, int x, int y, const Shade* shades, int n) {
  for (int i = 0; i < n; ++i) {
    const float a = shadeAlpha(shades[i], y);
    if (a <= 0.0f) continue;
    r += (shades[i].col.r - r) * a;
    g += (shades[i].col.g - g) * a;
    b += (shades[i].col.b - b) * a;
  }
  return pack(static_cast<int>(r + 0.5f), static_cast<int>(g + 0.5f), static_cast<int>(b + 0.5f), x, y);
}

void background(const Canvas& c, const Shade* shades, int n) {
  // linear-gradient(160deg, #111634 0, #161a3d 40%, #1a1739 70%, #0f1730 100%)
  static const Stop kStops[] = {{0.0f, 0x11, 0x16, 0x34},
                                {0.4f, 0x16, 0x1a, 0x3d},
                                {0.7f, 0x1a, 0x17, 0x39},
                                {1.0f, 0x0f, 0x17, 0x30}};
  const float k = static_cast<float>(c.w) / 800.0f;  // design size 800 x 480
  // 160deg: towards the bottom, a little to the right.
  const float dx = sinf(160.0f * static_cast<float>(M_PI) / 180.0f);
  const float dy = -cosf(160.0f * static_cast<float>(M_PI) / 180.0f);
  const float half = 0.5f * (fabsf(c.w * dx) + fabsf(c.h * dy));
  for (int y = 0; y < c.h; ++y) {
    uint16_t* row = c.px + y * c.w;
    for (int x = 0; x < c.w; ++x) {
      const float t = 0.5f + ((x - c.w * 0.5f) * dx + (y - c.h * 0.5f) * dy) / (2.0f * half);
      float r, g, b;
      stopsAt(kStops, 4, t, &r, &g, &b);
      // Peach glow bottom left (rgba(236,150,128,.22)), lavender top right (rgba(126,110,236,.38)).
      const float a2 = 0.22f * glow(x, y, c.w * 0.04f, c.h * 1.0f, 600.0f * k, 420.0f * k, 0.70f);
      r += (236.0f - r) * a2; g += (150.0f - g) * a2; b += (128.0f - b) * a2;
      const float a1 = 0.38f * glow(x, y, c.w * 0.90f, c.h * 0.08f, 640.0f * k, 460.0f * k, 0.72f);
      r += (126.0f - r) * a1; g += (110.0f - g) * a1; b += (236.0f - b) * a1;
      row[x] = packShaded(r, g, b, x, y, shades, n);
    }
  }
}

void roundRect(const Canvas& c, int x, int y, int w, int h, int radius, Rgb fill, int alpha, Rgb border,
               int border_alpha) {
  const float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
  const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
  const int x0 = std::max(0, x - 1), y0 = std::max(0, y - 1);
  const int x1 = std::min(c.w, x + w + 1), y1 = std::min(c.h, y + h + 1);
  for (int py = y0; py < y1; ++py) {
    uint16_t* row = c.px + py * c.w;
    for (int px = x0; px < x1; ++px) {
      const float sd = sdRoundRect(px + 0.5f, py + 0.5f, cx, cy, hw, hh, r);
      const float cov = clampf(0.5f - sd, 0.0f, 1.0f);
      if (cov <= 0.0f) continue;
      blendPx(row + px, px, py, fill, static_cast<int>(alpha * cov + 0.5f));
      if (border_alpha > 0) {
        // 1 px band just inside the edge.
        const float band = clampf(0.5f - fabsf(sd + 0.5f) + 0.5f, 0.0f, 1.0f) * cov;
        if (band > 0.0f) blendPx(row + px, px, py, border, static_cast<int>(border_alpha * band + 0.5f));
      }
    }
  }
}

void circle(const Canvas& c, float cx, float cy, float r, Rgb col, int alpha) {
  const int x0 = std::max(0, static_cast<int>(cx - r - 1)), x1 = std::min(c.w, static_cast<int>(cx + r + 2));
  const int y0 = std::max(0, static_cast<int>(cy - r - 1)), y1 = std::min(c.h, static_cast<int>(cy + r + 2));
  for (int py = y0; py < y1; ++py) {
    uint16_t* row = c.px + py * c.w;
    const float dy = py + 0.5f - cy;
    for (int px = x0; px < x1; ++px) {
      const float dx = px + 0.5f - cx;
      const float cov = clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
      if (cov > 0.0f) blendPx(row + px, px, py, col, static_cast<int>(alpha * cov + 0.5f));
    }
  }
}

void arc(const Canvas& c, float cx, float cy, float r_out, float r_in, float a0, float a1, Rgb col, int alpha) {
  if (a1 <= a0) return;
  const int x0 = std::max(0, static_cast<int>(cx - r_out - 1)), x1 = std::min(c.w, static_cast<int>(cx + r_out + 2));
  const int y0 = std::max(0, static_cast<int>(cy - r_out - 1)), y1 = std::min(c.h, static_cast<int>(cy + r_out + 2));
  const float full = a1 - a0 >= 359.9f;
  for (int py = y0; py < y1; ++py) {
    uint16_t* row = c.px + py * c.w;
    const float dy = py + 0.5f - cy;
    for (int px = x0; px < x1; ++px) {
      const float dx = px + 0.5f - cx;
      const float d = sqrtf(dx * dx + dy * dy);
      float cov = clampf(r_out - d + 0.5f, 0.0f, 1.0f) * clampf(d - r_in + 0.5f, 0.0f, 1.0f);
      if (cov <= 0.0f) continue;
      if (!full) {
        float a = atan2f(dx, -dy) * 180.0f / static_cast<float>(M_PI);  // 0 = up, clockwise
        if (a < 0) a += 360.0f;
        // Soft ends: about one pixel of the arc length.
        const float px_deg = 57.3f / std::max(d, 1.0f);
        const float in0 = clampf((a - a0) / px_deg + 0.5f, 0.0f, 1.0f);
        const float in1 = clampf((a1 - a) / px_deg + 0.5f, 0.0f, 1.0f);
        cov *= std::min(in0, in1);
        if (cov <= 0.0f) continue;
      }
      blendPx(row + px, px, py, col, static_cast<int>(alpha * cov + 0.5f));
    }
  }
}

void gradientBar(const Canvas& c, int x, int y, int w, int h, Rgb c0, Rgb c1, Rgb c2) {
  if (w <= 0) return;
  const float r = h * 0.5f;
  const float cx = x + w * 0.5f, cy = y + h * 0.5f;
  const int x0 = std::max(0, x - 1), x1 = std::min(c.w, x + w + 1);
  const int y0 = std::max(0, y - 1), y1 = std::min(c.h, y + h + 1);
  for (int py = y0; py < y1; ++py) {
    uint16_t* row = c.px + py * c.w;
    for (int px = x0; px < x1; ++px) {
      const float sd = sdRoundRect(px + 0.5f, py + 0.5f, cx, cy, w * 0.5f, h * 0.5f, r);
      const float cov = clampf(0.5f - sd, 0.0f, 1.0f);
      if (cov <= 0.0f) continue;
      const Rgb col = ramp(c0, c1, c2, w > 1 ? static_cast<float>(px - x) / (w - 1) : 0.0f);
      blendPx(row + px, px, py, col, static_cast<int>(255 * cov + 0.5f));
    }
  }
}

void shade(const Canvas& c, int y0, int y1, Rgb col, int a0, int a1) {
  y0 = std::max(0, y0);
  y1 = std::min(c.h, y1);
  if (y1 <= y0) return;
  for (int py = y0; py < y1; ++py) {
    float t = static_cast<float>(py - y0) / std::max(1, y1 - y0 - 1);
    t = t * t * (3.0f - 2.0f * t);  // eased: no line where the shade starts
    const int a = static_cast<int>(a0 + (a1 - a0) * t + 0.5f);
    uint16_t* row = c.px + py * c.w;
    for (int px = 0; px < c.w; ++px) blendPx(row + px, px, py, col, a);
  }
}

void copyRect(const Canvas& dst, const Canvas& src, int x, int y, int w, int h) {
  x = std::max(0, x);
  y = std::max(0, y);
  w = std::min(w, dst.w - x);
  h = std::min(h, dst.h - y);
  for (int py = y; py < y + h; ++py) memcpy(dst.px + py * dst.w + x, src.px + py * src.w + x, w * 2);
}

}  // namespace ui::paint
