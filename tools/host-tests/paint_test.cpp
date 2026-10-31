// Host test for src/deck/ui/paint.cpp (stage 10b-2): the cached background and the fast
// cover page must look like the old per-pixel code. Build and run (PC or CI):
//   g++ -std=gnu++17 -O2 -Isrc/deck tools/host-tests/paint_test.cpp src/deck/ui/paint.cpp -o paint_test
//   ./paint_test
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ui/paint.h"

using namespace ui::paint;

namespace {

constexpr int kW = 800, kH = 480;

float smooth01(float t) {
  t = std::min(std::max(t, 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// The music page's cover loop as it was in deck_screen.cpp (buildMusicBase, 5in-0.42.0).
void refCoverPage(const Canvas& c, const uint8_t* pix, bool bgr, int cw, int ch, float fx, float fy, float rim,
                  Rgb rim_col, const Shade* shades, int n) {
  const int x0 = (kW - cw) / 2, y0 = (kH - ch) / 2;
  for (int y = 0; y < kH; ++y) {
    const int py = y - y0;
    const float ay = (py < 0 || py >= ch) ? 0.0f : smooth01(std::min(py + 0.5f, ch - py - 0.5f) / fy);
    for (int x = 0; x < kW; ++x) {
      const int px = x - x0;
      float a = 0.0f;
      if (ay > 0.0f && px >= 0 && px < cw) a = ay * smooth01(std::min(px + 0.5f, cw - px - 0.5f) / fx);
      const float ox = std::max({0.0f, static_cast<float>(x0 - x), static_cast<float>(x - (x0 + cw - 1))});
      const float oy = std::max({0.0f, static_cast<float>(y0 - y), static_cast<float>(y - (y0 + ch - 1))});
      const float near = 1.0f - smooth01(sqrtf(ox * ox + oy * oy) / rim);
      float r, g, b;
      backgroundRgb(kW, kH, x, y, &r, &g, &b);
      if (a > 0.0f || near > 0.0f) {
        const float dk = near * (1.0f - a) * 0.85f;
        r += (rim_col.r - r) * dk, g += (rim_col.g - g) * dk, b += (rim_col.b - b) * dk;
      }
      if (a > 0.0f) {
        const uint8_t* p = pix + (py * cw + px) * 3;
        const int pr = bgr ? p[2] : p[0], pg = p[1], pb = bgr ? p[0] : p[2];
        r += (pr - r) * a, g += (pg - g) * a, b += (pb - b) * a;
      }
      c.px[y * kW + x] = packShaded(r, g, b, x, y, shades, n);
    }
  }
}

// pack() as it was before its table (stage 10b-5): the table must give the same pixels.
uint16_t refPack(int r, int g, int b, int x, int y) {
  static const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const int d = kBayer[y & 3][x & 3];
  auto c8 = [](int v) { return std::min(std::max(v, 0), 255); };
  const int r5 = std::min(31, (c8(r) * 31 + (d * 255) / 16) / 255);
  const int g6 = std::min(63, (c8(g) * 63 + (d * 255) / 16) / 255);
  const int b5 = std::min(31, (c8(b) * 31 + (d * 255) / 16) / 255);
  const uint16_t v = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
  return static_cast<uint16_t>((v >> 8) | (v << 8));
}

int failures = 0;
size_t g_cache_bytes = 0;  // what bgCacheInit asked for

void* countingAlloc(size_t n) {
  g_cache_bytes += n;
  return std::malloc(n);
}

void compare(const char* what, const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, double ms_old,
             double ms_new) {
  int maxdiff = 0, differ = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const uint16_t va = static_cast<uint16_t>((a[i] >> 8) | (a[i] << 8));
    const uint16_t vb = static_cast<uint16_t>((b[i] >> 8) | (b[i] << 8));
    const int d = std::max({std::abs((va >> 11) - (vb >> 11)), std::abs(((va >> 5) & 63) - ((vb >> 5) & 63)),
                            std::abs((va & 31) - (vb & 31))});
    maxdiff = std::max(maxdiff, d);
    differ += d != 0;
  }
  const double pct = 100.0 * differ / a.size();
  const bool ok = maxdiff <= 1 && pct <= 1.0;
  std::printf("%s %s: maxdiff %d, differ %.3f%%, old %.1f ms, new %.1f ms\n", ok ? "ok  " : "FAIL", what, maxdiff, pct,
              ms_old, ms_new);
  failures += !ok;
}

template <typename F>
double timed(F f) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<uint8_t> fakeCover(int cw, int ch) {
  std::vector<uint8_t> v(cw * ch * 3);
  for (int y = 0; y < ch; ++y)
    for (int x = 0; x < cw; ++x) {
      uint8_t* p = &v[(y * cw + x) * 3];
      p[0] = static_cast<uint8_t>(x * 255 / cw), p[1] = static_cast<uint8_t>((x ^ y) & 255),
      p[2] = static_cast<uint8_t>(y * 255 / ch);
    }
  return v;
}

}  // namespace

int main() {
  const Rgb ink{10, 11, 22};
  Shade plain[2] = {{0, 90, ink, 140, 0}, {kH - 300, kH, ink, 0, 150}};
  Shade cover[2] = {{0, 90, ink, 140, 0}, {kH - 300, kH, ink, 0, 235}};
  std::vector<uint16_t> a(kW * kH), b(kW * kH);
  Canvas ca{a.data(), kW, kH}, cb{b.data(), kW, kH};

  int pack_bad = 0;
  for (int v = -8; v < 264; ++v)
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) pack_bad += pack(v, 255 - v, v * 7 % 300, x, y) != refPack(v, 255 - v, v * 7 % 300, x, y);
  std::printf("%s pack table: %d of %d differ\n", pack_bad == 0 ? "ok  " : "FAIL", pack_bad, 272 * 16);
  failures += pack_bad != 0;

  double ms_old = timed([&] { background(ca, plain, 2); });
  if (!bgCacheInit(kW, kH, countingAlloc)) {
    std::printf("FAIL bgCacheInit\n");
    return 1;
  }
  // The card's 8 MB PSRAM also holds the code, three frame buffers and the page sprites.
  std::printf("%s background copy: %zu bytes\n", g_cache_bytes <= 128 * 1024 ? "ok  " : "FAIL", g_cache_bytes);
  failures += g_cache_bytes > 128 * 1024;
  double ms_new = timed([&] { backgroundFast(cb, plain, 2); });
  compare("background with shades", a, b, ms_old, ms_new);

  ms_old = timed([&] { background(ca); });
  ms_new = timed([&] { backgroundFast(cb); });
  compare("background plain", a, b, ms_old, ms_new);

  const int sizes[][2] = {{480, 480}, {300, 300}, {kW, kH}, {480, 270}, {40, 40}};
  for (const auto& sz : sizes) {
    const int cw = sz[0], ch = sz[1];
    for (bool bgr : {true, false}) {
      const auto pic = fakeCover(cw, ch);
      const float fx = std::min(60.0f, cw / 5.0f), fy = std::min(60.0f, ch / 5.0f);
      ms_old = timed([&] { refCoverPage(ca, pic.data(), bgr, cw, ch, fx, fy, 150, {6, 7, 14}, cover, 2); });
      ms_new = timed([&] { coverPage(cb, pic.data(), bgr, cw, ch, fx, fy, 150, {6, 7, 14}, cover, 2); });
      char what[64];
      std::snprintf(what, sizeof(what), "cover page %dx%d %s", cw, ch, bgr ? "bgr" : "rgb");
      compare(what, a, b, ms_old, ms_new);
    }
  }

  float r, g, bl, r2, g2, b2;
  backgroundRgb(kW, kH, 123, 321, &r, &g, &bl);
  bgRgb(kW, kH, 123, 321, &r2, &g2, &b2);
  if (std::fabs(r - r2) > 0.5f || std::fabs(g - g2) > 0.5f || std::fabs(bl - b2) > 0.5f) {
    std::printf("FAIL bgRgb %f %f %f vs %f %f %f\n", r2, g2, b2, r, g, bl);
    ++failures;
  }
  if (failures) return 1;
  std::printf("paint ok\n");
  return 0;
}
