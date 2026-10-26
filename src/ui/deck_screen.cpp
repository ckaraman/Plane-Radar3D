#include "ui/deck_screen.h"

#if defined(BOARD_WS5)

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "config.h"
#include "hardware/console.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/deck_store.h"
#include "services/hid_keys.h"
#include "services/host_link.h"
#include "ui/fonts_tr.h"
#include "ui/paint.h"

namespace ui::deck {

namespace {

using services::hid::Media;
using paint::Rgb;
namespace store = services::deck;
namespace host = services::host;

// ------------------------------------------------------------------ layout
// The approved design is drawn at 800 x 480; the 5B panel (1024 x 600) scales it.
constexpr int kW = config::kDisplayWidth;
constexpr int kH = config::kDisplayHeight;
constexpr int D(int v) { return v * kW / 800; }

// Shared top bar: RADAR pill on the right, page dots in the middle.
constexpr int kRadarX = D(670), kRadarY = D(14), kRadarW = D(110), kRadarH = D(40);
constexpr int kDotsY = D(34);

// Keys page: 4 x 3 keys on the left, the volume knob on the right.
constexpr int kCols = 4, kRows = 3;
constexpr int kGridX = D(20), kGridY = D(68), kGap = D(10);
constexpr int kKeyW = (D(470) - 3 * D(10)) / 4;
constexpr int kKeyH = D(120);
constexpr int kKnobX = D(512), kKnobY = D(88), kKnobD = D(268);
constexpr int kKnobCx = kKnobX + kKnobD / 2, kKnobCy = kKnobY + kKnobD / 2;
constexpr float kRingR = kKnobD / 2.0f, kRingW = D(14);
static_assert(kCols * kRows == store::kSlots, "one key per stored slot");

// Music page: text, bars and controls at the bottom, over the cover.
constexpr int kMusicPad = D(28);
constexpr int kTitleY = D(296), kArtistY = D(338);
constexpr int kBarY = D(380), kBarH = D(5);
constexpr int kTimeW = D(46);
constexpr int kCtrlY = D(436);
constexpr int kPrevCx = D(314), kPlayCx = D(400), kNextCx = D(486);
constexpr int kSmallBtnR = D(26), kPlayR = D(34);
constexpr int kVolX = D(62), kVolW = D(186), kVolH = D(5);

// Colours (approved palette).
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kText{243, 244, 251};
constexpr Rgb kInk{8, 10, 24};
constexpr Rgb kPeach{255, 180, 140};
constexpr Rgb kPink{243, 166, 182};
constexpr Rgb kLavender{185, 166, 255};
constexpr Rgb kFaceA{38, 43, 87}, kFaceB{21, 25, 56};
constexpr Rgb kMutedRed{245, 120, 120};

uint16_t col565(Rgb c) { return tft.color565(c.r, c.g, c.b); }

// ------------------------------------------------------------------ state
enum class Page { Music, Keys };
Page s_page = Page::Keys;

LGFX_Sprite s_canvas(&tft);      // only for an upside-down screen
LGFX_Sprite s_keys_base(&tft);   // keys page without the moving parts
LGFX_Sprite s_music_base(&tft);  // cover (or background) with its shades
bool s_ready = false;
bool s_keys_dirty = true, s_music_dirty = true;
uint32_t s_keys_gen = 0, s_cover_gen = 0, s_now_gen = 0;

struct RingPx {
  uint16_t x, y;  // canvas position
  uint16_t ang;   // 0..36000 (0.01 degree, 0 = top, clockwise)
  uint8_t cov;    // radial coverage of the ring band
  uint8_t pad;
};
RingPx* s_ring = nullptr;
int s_ring_n = 0;

// Volume: from the helper when it runs, else counted locally from the keys sent.
int s_level = -1;  // 0..volumeSteps() without the helper
bool s_muted_local = false;
unsigned long s_level_changed_ms = 0;
bool s_level_dirty = false;
int s_shown_level = -1;  // last level drawn (percent)
bool s_shown_muted = false;

// Key press / touch.
int s_pressed = -1;
unsigned long s_pressed_ms = 0;
bool s_flash = false;
enum class Target { None, Key, Knob, Back, Prev, Play, Next, VolBar, Swipe };
Target s_target = Target::None;
bool s_down = false;
int s_key = -1;
int s_down_x = 0, s_down_y = 0, s_last_x = 0, s_last_y = 0;
unsigned long s_down_ms = 0;
float s_last_deg = 0, s_accum_deg = 0, s_total_deg = 0;
int s_max_move = 0;
int s_target_level = -1;  // knob / bar with the helper, not sent yet (percent)
unsigned long s_last_send_ms = 0;

// Music page.
bool s_was_playing = false;
uint32_t s_last_id = 0;
unsigned long s_stopped_ms = 0;
uint32_t s_shown_pos = 0xFFFFFFFF;

bool s_dirty = true;  // canvas needs to be put together and shown
unsigned long s_last_present_ms = 0;

paint::Canvas cv(LGFX_Sprite& s) { return {static_cast<uint16_t*>(s.getBuffer()), kW, kH}; }

// ------------------------------------------------------------------ volume
int steps() { return store::volumeSteps(); }

void loadLevel() {
  Preferences p;
  int v = steps() / 2;
  if (p.begin("deck", true)) {
    v = p.getInt("vol", v);
    p.end();
  }
  s_level = constrain(v, 0, steps());
}

void saveLevel() {
  Preferences p;
  if (p.begin("deck", false)) {
    p.putInt("vol", s_level);
    p.end();
  }
  s_level_dirty = false;
}

int levelPercent() {
  if (host::active()) return host::volume();
  return (constrain(s_level, 0, steps()) * 100 + steps() / 2) / steps();
}
bool mutedNow() { return host::active() ? host::muted() : s_muted_local; }
bool canSendHid() { return console::keyboardMode() && services::hid::ready(); }

/** One local volume step (no helper): a volume key, the count follows. */
void hidStep(int dir) {
  s_level = constrain(s_level + dir, 0, steps());
  s_level_changed_ms = millis();
  s_level_dirty = true;
  s_muted_local = false;
  if (canSendHid()) services::hid::sendMedia(dir > 0 ? Media::VolumeUp : Media::VolumeDown);
}

/** Absolute level wanted (percent): to the helper, else as volume keys. */
void wantLevel(int pct) {
  pct = constrain(pct, 0, 100);
  if (host::active()) {
    if (millis() - s_last_send_ms >= 40) {
      host::sendVolume(pct);  // also the level shown until the helper answers
      s_last_send_ms = millis();
      s_target_level = -1;
    } else {
      s_target_level = pct;  // tick() sends it
    }
    return;
  }
  const int want = (pct * steps() + 50) / 100;
  while (s_level < want) hidStep(+1);
  while (s_level > want) hidStep(-1);
}

void toggleMute() {
  if (host::active()) {
    host::sendMute();
  } else {
    s_muted_local = !s_muted_local;
    if (canSendHid()) services::hid::sendMedia(Media::Mute);
  }
}

// ------------------------------------------------------------------ drawing helpers
void keyRect(int i, int* x, int* y) {
  *x = kGridX + (i % kCols) * (kKeyW + kGap);
  *y = kGridY + (i / kCols) * (kKeyH + kGap);
}

int keyAt(int x, int y) {
  for (int i = 0; i < store::kSlots; ++i) {
    int kx, ky;
    keyRect(i, &kx, &ky);
    if (x >= kx && x < kx + kKeyW && y >= ky && y < ky + kKeyH) return i;
  }
  return -1;
}

bool inRadar(int x, int y) {
  return x >= kRadarX - D(10) && x < kRadarX + kRadarW + D(10) && y >= 0 && y < kRadarY + kRadarH + D(10);
}

bool inKnob(int x, int y) {
  const int dx = x - kKnobCx, dy = y - kKnobCy;
  return dx * dx + dy * dy <= (kKnobD / 2 + D(10)) * (kKnobD / 2 + D(10));
}

bool inCircle(int x, int y, int cx, int cy, int r) {
  const int dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

/** UTF-8 text cut to fit max_w with "..." (the fonts have no ellipsis glyph). */
template <typename Gfx>
void fitText(Gfx& s, const char* in, int max_w, char* out, size_t cap) {
  strlcpy(out, in, cap);
  if (s.textWidth(out) <= max_w) return;
  size_t n = strlen(out);
  while (n > 0) {
    do {
      --n;
    } while (n > 0 && (static_cast<uint8_t>(out[n]) & 0xC0) == 0x80);
    out[n] = '\0';
    char tmp[176];
    snprintf(tmp, sizeof(tmp), "%s...", out);
    if (s.textWidth(tmp) <= max_w) {
      strlcpy(out, tmp, cap);
      return;
    }
  }
}

template <typename Gfx>
void radarPill(Gfx& s, const paint::Canvas& c) {
  paint::roundRect(c, kRadarX, kRadarY, kRadarW, kRadarH, kRadarH / 2, kWhite, 22, kWhite, 34);
  const float cx = kRadarX + D(26), cy = kRadarY + kRadarH / 2.0f;
  paint::arc(c, cx, cy, D(10), D(8), 0, 360, kWhite, 255);
  paint::arc(c, cx, cy, D(5), D(3), 0, 360, kWhite, 255);
  s.drawLine(cx, cy, cx + D(6), cy - D(6), col565(kWhite));
  displayFontSetBitmap(s, &ui::font::SansBold8);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(col565(kWhite));
  s.drawString("RADAR", kRadarX + D(44), kRadarY + kRadarH / 2 + 1);
}

void pageDots(const paint::Canvas& c, bool music_active) {
  const int cx = kW / 2;
  const int wide = D(22), dot = D(7);
  // Left dot = music, right dot = keys; the active one is a pill.
  if (music_active) {
    paint::roundRect(c, cx - D(4) - wide, kDotsY - dot / 2, wide, dot, dot / 2, kWhite, 255);
    paint::circle(c, cx + D(4) + dot / 2.0f, kDotsY, dot / 2.0f, kWhite, 100);
  } else {
    paint::circle(c, cx - D(4) - dot / 2.0f, kDotsY, dot / 2.0f, kWhite, 100);
    paint::roundRect(c, cx + D(4), kDotsY - dot / 2, wide, dot, dot / 2, kWhite, 255);
  }
}

template <typename Gfx>
void speakerIcon(Gfx& g, int cx, int cy, int s, uint16_t c) {
  g.fillRect(cx - s, cy - s / 2, s * 2 / 3, s, c);
  g.fillTriangle(cx - s / 3, cy - s / 2, cx + s / 3, cy - s, cx + s / 3, cy + s, c);
  g.fillRect(cx - s / 3, cy - s / 2, s * 2 / 3 + 1, s, c);
}

template <typename Gfx>
void mediaIcon(Gfx& g, Media m, int cx, int cy, uint16_t c) {
  const int s = D(13);
  switch (m) {
    case Media::Previous:
      g.fillRoundRect(cx - s, cy - s, D(4), 2 * s, D(1), c);
      g.fillTriangle(cx - s + D(3), cy, cx + s, cy - s, cx + s, cy + s, c);
      break;
    case Media::Next:
      g.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + s - D(3), cy, c);
      g.fillRoundRect(cx + s - D(4), cy - s, D(4), 2 * s, D(1), c);
      break;
    case Media::PlayPause:
      g.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + D(3), cy, c);
      g.fillRoundRect(cx + D(6), cy - s, D(4), 2 * s, D(1), c);
      g.fillRoundRect(cx + D(13), cy - s, D(4), 2 * s, D(1), c);
      break;
    case Media::VolumeUp:
    case Media::VolumeDown:
    case Media::Mute: {
      speakerIcon(g, cx - D(6), cy, s, c);
      const int mx = cx + D(12);
      if (m == Media::Mute) {
        for (int t = -1; t <= 1; ++t) {
          g.drawLine(mx - D(5) + t, cy - D(5), mx + D(5) + t, cy + D(5), c);
          g.drawLine(mx - D(5) + t, cy + D(5), mx + D(5) + t, cy - D(5), c);
        }
      } else {
        g.fillRect(mx - D(6), cy - D(1), D(12), D(3), c);
        if (m == Media::VolumeUp) g.fillRect(mx - D(1), cy - D(6), D(3), D(12), c);
      }
      break;
    }
  }
}

/** A stored picture onto a sprite: magenta = see-through. */
void blitIcon(LGFX_Sprite& s, const uint16_t* pic, int px, int x, int y) {
  auto* dst = static_cast<uint16_t*>(s.getBuffer());
  for (int r = 0; r < px; ++r) {
    const int yy = y + r;
    if (yy < 0 || yy >= kH) continue;
    for (int q = 0; q < px; ++q) {
      const uint16_t v = pic[r * px + q];
      const int xx = x + q;
      if (v == store::kTransparent || xx < 0 || xx >= kW) continue;
      dst[yy * kW + xx] = static_cast<uint16_t>((v >> 8) | (v << 8));
    }
  }
}

void drawKeyContent(LGFX_Sprite& s, int i) {
  int x, y;
  keyRect(i, &x, &y);
  const store::Slot& k = store::slot(i);
  const int cx = x + kKeyW / 2;
  const uint16_t* pic = store::icon(i);
  const int px = store::iconPx();
  const bool has_label = k.label[0] != '\0';
  char t[48];
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(col565({217, 220, 239}));
  if (pic != nullptr) {
    const int iy = has_label ? y + (kKeyH - px - D(22)) / 2 : y + (kKeyH - px) / 2;
    blitIcon(s, pic, px, cx - px / 2, iy);
    if (has_label) {
      displayFontSetBitmap(s, &ui::font::Sans8);
      fitText(s, k.label, kKeyW - D(10), t, sizeof(t));
      s.drawString(t, cx, y + kKeyH - D(16));
    }
  } else if (k.kind == store::Kind::Media) {
    mediaIcon(s, static_cast<Media>(k.code), cx, y + kKeyH / 2 - D(10), col565({233, 234, 255}));
    displayFontSetBitmap(s, &ui::font::Sans8);
    fitText(s, k.label, kKeyW - D(10), t, sizeof(t));
    s.drawString(t, cx, y + kKeyH - D(20));
  } else if (has_label) {
    const lgfx::GFXfont* fonts[] = {&ui::font::SansBold12, &ui::font::SansBold9, &ui::font::Sans8};
    for (const auto* f : fonts) {
      displayFontSetBitmap(s, f);
      if (s.textWidth(k.label) <= kKeyW - D(12)) break;
    }
    fitText(s, k.label, kKeyW - D(10), t, sizeof(t));
    s.setTextColor(col565(kText));
    s.drawString(t, cx, y + kKeyH / 2);
  }
  s.setTextDatum(textdatum_t::top_left);
}

void buildRing() {
  if (s_ring != nullptr) return;
  std::vector<RingPx> v;
  const float ro = kRingR, ri = kRingR - kRingW;
  for (int y = kKnobY - 1; y <= kKnobY + kKnobD + 1; ++y) {
    for (int x = kKnobX - 1; x <= kKnobX + kKnobD + 1; ++x) {
      const float dx = x + 0.5f - kKnobCx, dy = y + 0.5f - kKnobCy;
      const float d = sqrtf(dx * dx + dy * dy);
      const float cov =
          std::min(std::max(ro - d + 0.5f, 0.0f), 1.0f) * std::min(std::max(d - ri + 0.5f, 0.0f), 1.0f);
      if (cov <= 0.0f) continue;
      float a = atan2f(dx, -dy) * 180.0f / static_cast<float>(M_PI);
      if (a < 0) a += 360.0f;
      v.push_back({static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(a * 100.0f),
                   static_cast<uint8_t>(cov * 255.0f + 0.5f), 0});
    }
  }
  s_ring = static_cast<RingPx*>(heap_caps_malloc(v.size() * sizeof(RingPx), MALLOC_CAP_SPIRAM));
  if (s_ring == nullptr) return;
  memcpy(s_ring, v.data(), v.size() * sizeof(RingPx));
  s_ring_n = static_cast<int>(v.size());
}

/** Knob face and the empty ring track (part of the keys base). */
void drawKnobBase(LGFX_Sprite& s) {
  const auto c = cv(s);
  paint::arc(c, kKnobCx, kKnobCy, kRingR, kRingR - kRingW, 0, 360, kWhite, 26);
  // Face: a soft radial shading, lighter towards the upper left.
  const float r = kRingR - kRingW, lx = kKnobCx - r * 0.2f, ly = kKnobCy - r * 0.36f;
  uint16_t* px = c.px;
  for (int y = static_cast<int>(kKnobCy - r - 1); y <= kKnobCy + r + 1; ++y) {
    if (y < 0 || y >= kH) continue;
    for (int x = static_cast<int>(kKnobCx - r - 1); x <= kKnobCx + r + 1; ++x) {
      const float dx = x + 0.5f - kKnobCx, dy = y + 0.5f - kKnobCy;
      const float cov = std::min(std::max(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f), 1.0f);
      if (cov <= 0.0f) continue;
      const float ex = x - lx, ey = y - ly;
      const float t = std::min(sqrtf(ex * ex + ey * ey) / (r * 1.35f), 1.0f);
      const float u = t * t * (3.0f - 2.0f * t);
      const float cr = kFaceA.r + (kFaceB.r - kFaceA.r) * u, cg = kFaceA.g + (kFaceB.g - kFaceA.g) * u,
                  cb = kFaceA.b + (kFaceB.b - kFaceA.b) * u;
      int r0, g0, b0;
      paint::unpack(px[y * kW + x], &r0, &g0, &b0);
      px[y * kW + x] = paint::pack(static_cast<int>(r0 + (cr - r0) * cov + 0.5f),
                                   static_cast<int>(g0 + (cg - g0) * cov + 0.5f),
                                   static_cast<int>(b0 + (cb - b0) * cov + 0.5f), x, y);
    }
  }
}

/** The moving part of the knob: filled arc, its end dot, the speaker. */
template <typename Gfx>
void drawKnobLevel(Gfx& s, const paint::Canvas& c, int pct, bool muted) {
  uint16_t* px = c.px;
  const int lvl = constrain(pct, 0, 100) * 360;  // 0.01 degree
  const float edge = 57.3f / kRingR * 100.0f;    // one pixel of arc, in 0.01 degree
  const Rgb dim{150, 156, 190};
  for (int i = 0; i < s_ring_n; ++i) {
    const RingPx& p = s_ring[i];
    if (p.ang > lvl + edge) continue;
    float cov = p.cov / 255.0f;
    if (p.ang > lvl - edge) cov *= std::min(std::max((lvl - p.ang) / edge + 0.5f, 0.0f), 1.0f);
    if (cov <= 0.0f) continue;
    const Rgb col =
        muted ? dim : paint::ramp(kPeach, kPink, kLavender, lvl > 0 ? static_cast<float>(p.ang) / lvl : 0.0f);
    uint16_t& d = px[p.y * kW + p.x];
    int r, g, b;
    paint::unpack(d, &r, &g, &b);
    d = paint::pack(static_cast<int>(r + (col.r - r) * cov), static_cast<int>(g + (col.g - g) * cov),
                    static_cast<int>(b + (col.b - b) * cov), p.x, p.y);
  }
  const float a = (lvl / 100.0f) * static_cast<float>(M_PI) / 180.0f;
  const float mr = kRingR - kRingW / 2.0f;
  const float ex = kKnobCx + sinf(a) * mr, ey = kKnobCy - cosf(a) * mr;
  paint::circle(c, ex, ey, D(15), muted ? dim : kLavender, 110);
  paint::circle(c, ex, ey, D(11), kWhite, 255);
  // Speaker in the middle, waves or a cross.
  const uint16_t sc = col565(muted ? kMutedRed : kText);
  speakerIcon(s, kKnobCx - D(12), kKnobCy, D(22), sc);
  const int mx = kKnobCx + D(16);
  if (muted) {
    for (int t = -2; t <= 2; ++t) {
      s.drawLine(mx - D(9) + t, kKnobCy - D(9), mx + D(9) + t, kKnobCy + D(9), sc);
      s.drawLine(mx - D(9) + t, kKnobCy + D(9), mx + D(9) + t, kKnobCy - D(9), sc);
    }
  } else {
    paint::arc(c, mx - D(8), kKnobCy, D(11), D(8), 50, 130, kText, 255);
    paint::arc(c, mx - D(8), kKnobCy, D(21), D(18), 50, 130, kText, 255);
  }
}

// ------------------------------------------------------------------ pages
void buildKeysBase() {
  auto& s = s_keys_base;
  const auto c = cv(s);
  paint::background(c);
  displayFontSetBitmap(s, &ui::font::SansBold8);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(col565({230, 232, 246}));
  s.drawString("KONTROL", D(20), kDotsY + 1);
  if (!console::keyboardMode()) {
    displayFontSetBitmap(s, &ui::font::Sans8);
    s.setTextColor(col565({245, 190, 120}));
    s.drawString("Kurulum modu: tuşlar kapalı", D(130), kDotsY + 1);
  }
  pageDots(c, false);
  radarPill(s, c);
  for (int i = 0; i < store::kSlots; ++i) {
    int x, y;
    keyRect(i, &x, &y);
    const store::Slot& k = store::slot(i);
    if (k.kind == store::Kind::Empty && !k.label[0] && !store::icon(i)) {
      paint::roundRect(c, x, y, kKeyW, kKeyH, D(18), kWhite, 0, kWhite, 40);
      continue;
    }
    paint::roundRect(c, x, y, kKeyW, kKeyH, D(18), kWhite, 15, kWhite, 26);
    drawKeyContent(s, i);
  }
  drawKnobBase(s);
  displayFontSetBitmap(s, &ui::font::Sans8);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(col565({154, 161, 189}));
  s.drawString("Sola kaydır: müzik", D(20), kH - D(16));
  s.setTextDatum(textdatum_t::top_left);
  s_keys_gen = store::generation();
  s_keys_dirty = false;
}

void buildMusicBase() {
  auto& s = s_music_base;
  const auto c = cv(s);
  size_t len = 0;
  const uint8_t* jpg = host::cover(&len);
  bool have_cover = false;
  // Shades: top for the pills, bottom for the text and controls; applied before packing.
  paint::Shade shades[2] = {{0, D(90), kInk, 140, 0}, {kH - D(300), kH, kInk, 0, 150}};
  if (jpg != nullptr && len > 0 && host::now().valid) {
    // Decoded at 24 bits, shaded, then dithered down: no steps in the dark fade.
    LGFX_Sprite full(&tft);
    full.setPsram(true);
    full.setColorDepth(24);
    if (full.createSprite(kW, kH) != nullptr) {
      full.fillScreen(0);
      have_cover = full.drawJpg(jpg, len, 0, 0, kW, kH);
      if (have_cover) {
        shades[1].a1 = 235;
        for (int y = 0; y < kH; ++y) {
          for (int x = 0; x < kW; ++x) {
            const auto p = full.readPixelRGB(x, y);
            c.px[y * kW + x] = paint::packShaded(p.R8(), p.G8(), p.B8(), x, y, shades, 2);
          }
        }
      }
      full.deleteSprite();
    } else {
      s.fillScreen(0);  // not enough memory for the 24-bit pass: straight to 16 bits
      have_cover = s.drawJpg(jpg, len, 0, 0, kW, kH);
      if (have_cover) {
        paint::shade(c, 0, D(90), kInk, 140, 0);
        paint::shade(c, kH - D(300), kH, kInk, 0, 235);
      }
    }
  }
  if (!have_cover) paint::background(c, shades, 2);
  s_cover_gen = host::coverGeneration();
  s_music_dirty = false;
}

void fmtTime(char* out, size_t cap, uint32_t s) {
  snprintf(out, cap, "%lu:%02lu", static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
}

template <typename Gfx>
void drawMusicFront(Gfx& s, const paint::Canvas& c) {
  const host::NowPlaying& n = host::now();
  // Top: "now playing" pill, dots, RADAR.
  if (n.valid) {
    char label[64];
    snprintf(label, sizeof(label), n.app[0] ? "%s · %s" : "%s", n.playing ? "ŞİMDİ ÇALIYOR" : "DURAKLATILDI",
             n.app);
    displayFontSetBitmap(s, &ui::font::Sans8);
    const int tw = s.textWidth(label);
    paint::roundRect(c, D(20), D(18), tw + D(40), D(32), D(16), kWhite, 30, kWhite, 40);
    paint::circle(c, D(37), D(34), D(4), n.playing ? kPeach : Rgb{180, 184, 210}, 255);
    s.setTextDatum(textdatum_t::middle_left);
    s.setTextColor(col565(kWhite));
    s.drawString(label, D(50), D(34) + 1);
  }
  pageDots(c, true);
  radarPill(s, c);

  if (!n.valid) {
    displayFontSetBitmap(s, &ui::font::SansBold12);
    s.setTextDatum(textdatum_t::middle_center);
    s.setTextColor(col565(kText));
    s.drawString("Şu an müzik çalmıyor", kW / 2, kH / 2 - D(20));
    displayFontSetBitmap(s, &ui::font::Sans8);
    s.setTextColor(col565({174, 180, 204}));
    s.drawString(host::active() ? "Bilgisayarda müzik başlayınca burada görünür"
                                : "Şarkı bilgisi Windows yardımcı programıyla gelir",
                 kW / 2, kH / 2 + D(16));
    s.setTextDatum(textdatum_t::middle_right);
    s.drawString("Sağa kaydır: tuşlar", kW - kMusicPad, kCtrlY);
    s.setTextDatum(textdatum_t::top_left);
    return;
  }

  const int tx = kMusicPad, tw = kW - 2 * kMusicPad;
  char buf[176];
  displayFontSetBitmap(s, &ui::font::SansBold18);
  s.setTextDatum(textdatum_t::top_left);
  s.setTextColor(col565(kText));
  fitText(s, n.title[0] ? n.title : "Bilinmeyen parça", tw, buf, sizeof(buf));
  s.drawString(buf, tx, kTitleY);
  displayFontSetBitmap(s, &ui::font::Sans12);
  s.setTextColor(col565({214, 217, 236}));
  char who[140];
  if (n.artist[0] && n.album[0]) {
    snprintf(who, sizeof(who), "%s · %s", n.artist, n.album);
  } else {
    strlcpy(who, n.artist[0] ? n.artist : n.album, sizeof(who));
  }
  fitText(s, who, tw, buf, sizeof(buf));
  s.drawString(buf, tx, kArtistY);

  // Progress: times either side of a gradient bar.
  const uint32_t pos = host::positionNow();
  s_shown_pos = pos;
  char t0[12], t1[12];
  fmtTime(t0, sizeof(t0), pos);
  fmtTime(t1, sizeof(t1), n.duration_s);
  displayFontSetBitmap(s, &ui::font::Sans8);
  s.setTextColor(col565({195, 200, 222}));
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString(t0, tx, kBarY + kBarH / 2 + 1);
  s.setTextDatum(textdatum_t::middle_right);
  if (n.duration_s) s.drawString(t1, tx + tw, kBarY + kBarH / 2 + 1);
  const int bx = tx + kTimeW, bw = tw - 2 * kTimeW;
  paint::roundRect(c, bx, kBarY, bw, kBarH, kBarH / 2, kWhite, 46);
  const float f = n.duration_s ? std::min(1.0f, static_cast<float>(pos) / n.duration_s) : 0.0f;
  paint::gradientBar(c, bx, kBarY, std::max(1, static_cast<int>(bw * f)), kBarH, kPeach, kPink, kLavender);
  paint::circle(c, bx + bw * f, kBarY + kBarH / 2.0f, D(7), kWhite, 255);

  // Controls.
  paint::circle(c, kPrevCx, kCtrlY, kSmallBtnR, kWhite, 26);
  paint::circle(c, kNextCx, kCtrlY, kSmallBtnR, kWhite, 26);
  paint::circle(c, kPlayCx, kCtrlY, kPlayR, kWhite, 255);
  mediaIcon(s, Media::Previous, kPrevCx, kCtrlY, col565(kWhite));
  mediaIcon(s, Media::Next, kNextCx, kCtrlY, col565(kWhite));
  const uint16_t ink = col565({21, 24, 58});
  if (n.playing) {
    s.fillRoundRect(kPlayCx - D(10), kCtrlY - D(12), D(7), D(24), D(2), ink);
    s.fillRoundRect(kPlayCx + D(3), kCtrlY - D(12), D(7), D(24), D(2), ink);
  } else {
    s.fillTriangle(kPlayCx - D(8), kCtrlY - D(13), kPlayCx - D(8), kCtrlY + D(13), kPlayCx + D(13), kCtrlY, ink);
  }

  // Volume bar, left of the controls.
  const bool muted = mutedNow();
  const int pct = levelPercent();
  speakerIcon(s, kMusicPad + D(12), kCtrlY, D(10), col565(muted ? kMutedRed : kWhite));
  paint::roundRect(c, kVolX, kCtrlY - kVolH / 2, kVolW, kVolH, kVolH / 2, kWhite, 46);
  if (!muted && pct > 0) {
    paint::gradientBar(c, kVolX, kCtrlY - kVolH / 2, std::max(1, kVolW * pct / 100), kVolH, kPink, kPink,
                       kLavender);
  }
  s_shown_level = pct;
  s_shown_muted = muted;
  displayFontSetBitmap(s, &ui::font::Sans8);
  s.setTextColor(col565({174, 180, 204}));
  s.setTextDatum(textdatum_t::middle_right);
  s.drawString("Sağa kaydır: tuşlar", kW - kMusicPad, kCtrlY);
  s.setTextDatum(textdatum_t::top_left);
}

void ensureBuffers() {
  if (s_ready) return;
  for (LGFX_Sprite* s : {&s_keys_base, &s_music_base}) {
    if (s->getBuffer() != nullptr) continue;
    s->setPsram(true);
    s->setColorDepth(16);
    if (s->createSprite(kW, kH) == nullptr) {
      Console.println("deck: not enough PSRAM for the screens");
      return;
    }
  }
  buildRing();
  s_ready = true;
}

/** The moving parts on top of a page's base, into canvas c (drawn on with g). */
template <typename Gfx>
void composeInto(Gfx& g, const paint::Canvas& c) {
  if (s_page == Page::Keys) {
    memcpy(c.px, s_keys_base.getBuffer(), kW * kH * 2);
    if (s_pressed >= 0) {
      int x, y;
      keyRect(s_pressed, &x, &y);
      paint::roundRect(c, x, y, kKeyW, kKeyH, D(18), kLavender, 60, {214, 202, 255}, 150);
    }
    const bool muted = mutedNow();
    const int pct = levelPercent();
    drawKnobLevel(g, c, pct, muted);
    s_shown_level = pct;
    s_shown_muted = muted;
  } else {
    memcpy(c.px, s_music_base.getBuffer(), kW * kH * 2);
    drawMusicFront(g, c);
  }
}

/** Put the current page together and show it. */
void present() {
  if (!s_ready) return;
  if (s_page == Page::Keys) {
    if (s_keys_dirty || s_keys_gen != store::generation()) buildKeysBase();
  } else if (s_music_dirty || s_cover_gen != host::coverGeneration()) {
    buildMusicBase();
  }
  if (tft.getRotation() != 2) {
    // Straight into the hidden frame buffer (same pixel format), then shown.
    tft.beginHiddenFrame();
    composeInto(tft, paint::Canvas{tft.hiddenBuffer(), kW, kH});
    tft.presentHiddenFrame();
  } else {
    // Screen mounted upside down: put together upright, copy turned.
    if (s_canvas.getBuffer() == nullptr) {
      s_canvas.setPsram(true);
      s_canvas.setColorDepth(16);
      if (s_canvas.createSprite(kW, kH) == nullptr) return;
    }
    composeInto(s_canvas, cv(s_canvas));
    tft.beginHiddenFrame();
    uint16_t* fb = tft.hiddenBuffer();
    const auto* src = static_cast<const uint16_t*>(s_canvas.getBuffer());
    for (int i = 0, n = kW * kH; i < n; ++i) fb[n - 1 - i] = src[i];
    tft.presentHiddenFrame();
  }
  s_dirty = false;
  s_last_present_ms = millis();
}

/** Show soon, at most ~30 frames a second (a knob turn can ask for many). */
void presentSoon() {
  s_dirty = true;
  if (millis() - s_last_present_ms >= 33) present();
}

void setPage(Page p) {
  if (p == s_page) return;
  s_page = p;
  s_pressed = -1;
  s_flash = false;
  present();
}

// ------------------------------------------------------------------ actions
void fireKey(int i) {
  const store::Slot& k = store::slot(i);
  Console.printf("Deck: %s\n", k.label);
  if (k.kind == store::Kind::Host) {
    host::sendKey(i);
    return;
  }
  if (!canSendHid()) return;
  if (k.kind == store::Kind::Media) {
    services::hid::sendMedia(static_cast<Media>(k.code));
  } else if (k.kind == store::Kind::Key && k.code != 0) {
    services::hid::sendCombo(k.mods, k.code);
  }
}

void media(Media m) {
  if (canSendHid()) services::hid::sendMedia(m);
}

float angleAt(int x, int y) {
  return atan2f(static_cast<float>(x - kKnobCx), static_cast<float>(kKnobCy - y)) * 180.0f /
         static_cast<float>(M_PI);
}

}  // namespace

void enter() {
  if (s_level < 0) loadLevel();
  ensureBuffers();
  s_pressed = -1;
  s_flash = false;
  s_down = false;
  s_target = Target::None;
  s_keys_dirty = true;  // USB mode or keys may have changed meanwhile
  const auto& n = host::now();
  s_page = (n.valid && n.playing) ? Page::Music : Page::Keys;
  s_was_playing = n.valid && n.playing;
  s_last_id = n.id;
  s_now_gen = host::nowGeneration();
  if (!s_ready) {
    tft.fillScreen(TFT_BLACK);
    return;
  }
  present();
}

bool tick() {
  if (!s_ready) return true;
  const unsigned long now = millis();
  if (s_flash && s_pressed >= 0 && now - s_pressed_ms >= 140) {
    s_pressed = -1;
    s_flash = false;
    s_dirty = true;
  }
  if (s_level_dirty && now - s_level_changed_ms >= 3000) saveLevel();
  if (s_target_level >= 0 && host::active() && now - s_last_send_ms >= 40) {
    host::sendVolume(s_target_level);
    s_last_send_ms = now;
    s_target_level = -1;
  }

  // What is playing: to the music page when music starts, back to the keys after a stop.
  const auto& n = host::now();
  if (s_now_gen != host::nowGeneration()) {
    s_now_gen = host::nowGeneration();
    const bool playing = n.valid && n.playing;
    if (playing && (!s_was_playing || n.id != s_last_id) && s_page == Page::Keys && !s_down) {
      s_page = Page::Music;
    }
    if (!playing && s_was_playing) s_stopped_ms = now;
    if (playing) s_stopped_ms = 0;
    s_was_playing = playing;
    s_last_id = n.id;
    s_dirty = true;
  }
  if (s_page == Page::Music && !(n.valid && n.playing) && s_stopped_ms != 0 && n.back_s >= 0 &&
      now - s_stopped_ms >= static_cast<unsigned long>(n.back_s) * 1000UL && !s_down) {
    s_stopped_ms = 0;
    s_page = Page::Keys;
    s_dirty = true;
  }
  if (s_page == Page::Music) {
    if (s_cover_gen != host::coverGeneration()) s_dirty = true;
    if (n.valid && host::positionNow() != s_shown_pos) s_dirty = true;
  }
  if (store::generation() != s_keys_gen && s_page == Page::Keys) s_dirty = true;
  if (levelPercent() != s_shown_level || mutedNow() != s_shown_muted) s_dirty = true;
  if (s_dirty) present();
  return true;
}

Touch touch(int fingers, int x, int y) {
  const unsigned long now = millis();
  if (fingers > 0) {
    s_last_x = x;
    s_last_y = y;
    if (!s_down) {
      s_down = true;
      s_down_x = x;
      s_down_y = y;
      s_down_ms = now;
      s_max_move = 0;
      s_key = -1;
      if (inRadar(x, y)) {
        s_target = Target::Back;
      } else if (s_page == Page::Keys) {
        if (inKnob(x, y)) {
          s_target = Target::Knob;
          s_last_deg = angleAt(x, y);
          s_accum_deg = s_total_deg = 0;
        } else if ((s_key = keyAt(x, y)) >= 0) {
          s_target = Target::Key;
          s_pressed = s_key;
          s_flash = false;
          presentSoon();
        } else {
          s_target = Target::None;
        }
      } else if (host::now().valid) {
        if (inCircle(x, y, kPrevCx, kCtrlY, kSmallBtnR + D(8))) {
          s_target = Target::Prev;
        } else if (inCircle(x, y, kPlayCx, kCtrlY, kPlayR + D(8))) {
          s_target = Target::Play;
        } else if (inCircle(x, y, kNextCx, kCtrlY, kSmallBtnR + D(8))) {
          s_target = Target::Next;
        } else if (x >= kVolX - D(30) && x < kVolX + kVolW + D(14) && abs(y - kCtrlY) < D(26)) {
          s_target = Target::VolBar;
          wantLevel((x - kVolX) * 100 / kVolW);
          presentSoon();
        } else {
          s_target = Target::None;
        }
      } else {
        s_target = Target::None;
      }
      return Touch::None;
    }
    const int dx = x - s_down_x, dy = y - s_down_y;
    const int move = abs(dx) + abs(dy);
    if (move > s_max_move) s_max_move = move;
    // A sideways slide anywhere but on the knob / volume bar turns the page.
    if (s_target != Target::Knob && s_target != Target::VolBar && s_target != Target::Swipe &&
        abs(dx) > D(40) && abs(dx) > 2 * abs(dy)) {
      if (s_target == Target::Key) {
        s_pressed = -1;
        presentSoon();
      }
      s_target = Target::Swipe;
    }
    if (s_target == Target::Knob) {
      const int kx = x - kKnobCx, ky = y - kKnobCy;
      if (kx * kx + ky * ky < (kKnobD / 8) * (kKnobD / 8)) return Touch::None;  // centre: no angle
      const float deg = angleAt(x, y);
      float d = deg - s_last_deg;
      if (d > 180.0f) d -= 360.0f;
      if (d < -180.0f) d += 360.0f;
      s_last_deg = deg;
      s_accum_deg += d;
      s_total_deg += fabsf(d);
      if (host::active()) {
        // With the helper: a full turn is 0..100 %, set directly.
        const int dp = static_cast<int>(s_accum_deg / 3.6f);
        if (dp != 0) {
          s_accum_deg -= dp * 3.6f;
          wantLevel((s_target_level >= 0 ? s_target_level : host::volume()) + dp);
          presentSoon();
        }
      } else {
        const float step = 360.0f / steps();
        bool changed = false;
        while (s_accum_deg >= step) {
          s_accum_deg -= step;
          hidStep(+1);
          changed = true;
        }
        while (s_accum_deg <= -step) {
          s_accum_deg += step;
          hidStep(-1);
          changed = true;
        }
        if (changed) presentSoon();
      }
    } else if (s_target == Target::VolBar) {
      wantLevel((x - kVolX) * 100 / kVolW);
      presentSoon();
    } else if (s_target == Target::Key && keyAt(x, y) != s_key && s_pressed == s_key) {
      s_pressed = -1;  // slid off the key: no press
      presentSoon();
    }
    return Touch::None;
  }

  if (!s_down) return Touch::None;
  s_down = false;
  x = s_last_x;
  y = s_last_y;
  const Target t = s_target;
  s_target = Target::None;
  switch (t) {
    case Target::Back:
      return inRadar(x, y) || s_max_move < D(20) ? Touch::Back : Touch::None;
    case Target::Swipe: {
      const int dx = x - s_down_x;
      if (dx > D(60)) {
        setPage(Page::Keys);  // finger to the right: keys
      } else if (dx < -D(60)) {
        setPage(Page::Music);  // to the left: music
      }
      return Touch::None;
    }
    case Target::Knob:
      if (s_total_deg < 8.0f && s_max_move < D(14) && now - s_down_ms < 700) {
        toggleMute();
        presentSoon();
      }
      return Touch::None;
    case Target::Key:
      if (s_pressed == s_key) {
        fireKey(s_key);
        s_pressed_ms = millis();
        s_flash = true;  // tick() lets go of the highlight
      }
      return Touch::None;
    case Target::Prev:
      media(Media::Previous);
      return Touch::None;
    case Target::Play:
      media(Media::PlayPause);
      return Touch::None;
    case Target::Next:
      media(Media::Next);
      return Touch::None;
    case Target::VolBar:
      if (s_target_level >= 0 && host::active()) {
        host::sendVolume(s_target_level);
        s_target_level = -1;
      }
      return Touch::None;
    case Target::None:
      return Touch::None;
  }
  return Touch::None;
}

}  // namespace ui::deck

#endif  // BOARD_WS5
