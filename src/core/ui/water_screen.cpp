#include "ui/water_screen.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "hardware/frame_push.h"
#include "hardware/display.h"
#include "services/clock_service.h"
#include "services/display_prefs.h"
#include "services/water_store.h"
#include "ui/fonts_tr.h"
#include "ui/scaled_gfx.h"
#include "ui/ui_scale.h"
#include "ui/radar3d.h"
#include "ui/screen_switch.h"

#ifdef RADAR3D_HOST_TEST
namespace ui::radar3d {
void hostCaptureFrame(LGFX_Sprite& spr);  // desktop preview harness
}
#endif

namespace ui::water {

namespace {

namespace store = services::water;

// Laid out on the 480x320 design grid; ScaledGfx maps it to the panel (1.5x and
// centred). The top bar and the RADAR button use
// panel coordinates so they line up with the other screens.
using Canvas = ScaledGfx<LGFX_Sprite>;
constexpr int kW = Canvas::kDesignW;
constexpr int kH = Canvas::kDesignH;
constexpr int kTopBarH = 22;
constexpr int kFirstHour = 9;   // reminders 09:00 ...
constexpr int kLastHour = 18;   // ... 18:00
constexpr unsigned long kAskTimeoutMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kThanksMs = 1800;
constexpr uint16_t kAmounts[4] = {100, 200, 330, 500};
constexpr uint16_t kGoalStep = 250;
constexpr uint16_t kGoalMin = 500;
constexpr uint16_t kGoalMax = 5000;

enum Pal : uint8_t {
  cBg = 0,
  cPanel,
  cEdge,
  cText,
  cDim,
  cFaint,
  cWater,
  cWaterLight,
  cWaterDeep,
  cPupil,
  cCheek,
  cGreen,
  cOrange,
  cGhost,
  cCyan,
  cRed,
};
struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kPalette[16] = {
    {0, 0, 0},       {12, 26, 38},    {48, 86, 110},   {255, 255, 255},
    {175, 188, 198}, {105, 120, 132}, {60, 160, 255},  {160, 220, 255},
    {16, 52, 92},    {8, 16, 28},     {255, 130, 160}, {80, 235, 140},
    {255, 160, 60},  {50, 88, 120},   {110, 210, 255}, {255, 70, 60},
};

const char* const kDayShort[7] = {"PAZ", "PZT", "SAL", "ÇAR", "PER", "CUM", "CMT"};
const char* const kDayLong[7] = {"Pazar",    "Pazartesi", "Salı",     "Çarşamba",
                                 "Perşembe", "Cuma",      "Cumartesi"};
const char* const kMonth[12] = {"Ocak",   "Şubat",   "Mart",  "Nisan", "Mayıs", "Haziran",
                                "Temmuz", "Ağustos", "Eylül", "Ekim",  "Kasım", "Aralık"};

Canvas s_canvas;
Canvas* s_spr = nullptr;

/** Bind the drawing front end to the shared frame; false when there is none. */
bool bindFrame() {
  LGFX_Sprite* f = ui::radar3d::frame();
  if (f == nullptr) {
    s_spr = nullptr;
    return false;
  }
  s_canvas.bind(f);
  s_spr = &s_canvas;
  return true;
}

// Main screen state
int s_offset = 0;  // 0 = today, 1 = yesterday, ...
long s_drawn_key = -1;

struct Rect {
  int x, y, w, h;
  bool hit(int px, int py) const {
    return px >= x - 3 && px < x + w + 3 && py >= y - 3 && py < y + h + 3;
  }
};
constexpr int kBtnH = 42;           // same row as the RADAR button (design grid)
constexpr int kBtnY = kH - 4 - kBtnH;
constexpr Rect kBtnPrev{6, kBtnY, 44, kBtnH};
constexpr Rect kBtnNext{54, kBtnY, 44, kBtnH};
constexpr Rect kBtnAdd{102, kBtnY, 86, kBtnH};
constexpr Rect kBtnGoal{192, kBtnY, 86, kBtnH};
constexpr Rect kBtnUndo{282, kBtnY, 88, kBtnH};
Rect s_week_rects[7];

// Overlay state
enum class Mode { None, Ask, Add, Goal, Thanks };
Mode s_mode = Mode::None;
unsigned long s_mode_since = 0;
unsigned long s_last_anim = 0;
uint16_t s_goal_edit = store::kDefaultGoalMl;
uint16_t s_thanks_ml = 0;
int32_t s_asked_day = -1;
int s_asked_hour = -1;
Rect s_amount_rects[4];
Rect s_ov_btn1{0, 0, 0, 0}, s_ov_btn2{0, 0, 0, 0}, s_ov_minus{0, 0, 0, 0}, s_ov_plus{0, 0, 0, 0};

// ------------------------------------------------------------------ helpers

void font(const lgfx::IFont* f) {
  s_spr->setFont(f);
  s_spr->setTextSize(1);
}

void text(const char* t, int x, int y, textdatum_t d, const lgfx::IFont* f, uint8_t c) {
  font(f);
  s_spr->setTextDatum(d);
  s_spr->setTextColor(c);
  s_spr->drawString(t, x, y);
}

void panel(int x, int y, int w, int h, uint8_t edge = cEdge) {
  s_spr->fillRoundRect(x, y, w, h, 8, cPanel);
  s_spr->drawRoundRect(x, y, w, h, 8, edge);
}

void button(const Rect& r, const char* label, bool enabled = true, uint8_t edge = cEdge) {
  s_spr->fillRoundRect(r.x, r.y, r.w, r.h, 8, cPanel);
  s_spr->drawRoundRect(r.x, r.y, r.w, r.h, 8, enabled ? edge : cGhost);
  text(label, r.x + r.w / 2, r.y + r.h / 2 + 1, textdatum_t::middle_center, &ui::font::SansBold9,
       enabled ? cText : cFaint);
}

/** "1,45 L", "2 L", "850 ml" */
void fmtVolume(uint32_t ml, char* buf, size_t n) {
  if (ml < 1000) {
    snprintf(buf, n, "%u ml", static_cast<unsigned>(ml));
    return;
  }
  const unsigned l = ml / 1000, frac = (ml % 1000 + 5) / 10;  // 2 decimals
  if (frac == 0) {
    snprintf(buf, n, "%u L", l);
  } else if (frac % 10 == 0) {
    snprintf(buf, n, "%u,%u L", l, frac / 10);
  } else {
    snprintf(buf, n, "%u,%02u L", l, frac);
  }
}

void applyPalette() {
  for (int i = 0; i < 16; ++i) s_spr->setPaletteColor(i, kPalette[i].r, kPalette[i].g, kPalette[i].b);
}

void push() {
#ifdef RADAR3D_HOST_TEST
  ui::radar3d::hostCaptureFrame(*s_spr->raw());
#else
  pushFrame(*s_spr->raw());
#endif
}

void thickLine(float x0, float y0, float x1, float y1, float w, uint8_t c) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
  s_spr->fillTriangle(lroundf(x0 + nx), lroundf(y0 + ny), lroundf(x0 - nx), lroundf(y0 - ny),
                      lroundf(x1 - nx), lroundf(y1 - ny), c);
  s_spr->fillTriangle(lroundf(x0 + nx), lroundf(y0 + ny), lroundf(x1 - nx), lroundf(y1 - ny),
                      lroundf(x1 + nx), lroundf(y1 + ny), c);
}

// ------------------------------------------------------------------ avatar

enum class Mood { Normal, Happy, Thirsty };

/** Droplet = circle (centre cx,cy radius r) + tangent triangle up to a tip. */
void dropletShape(int cx, int cy, float r, uint8_t c) {
  const float d = r * 2.05f;  // centre -> tip
  const float a = acosf(r / d);
  const int tx = cx, ty = lroundf(cy - d);
  const int lx = lroundf(cx - r * sinf(a)), rx = lroundf(cx + r * sinf(a));
  const int py = lroundf(cy - r * cosf(a));
  s_spr->fillCircle(cx, cy, lroundf(r), c);
  s_spr->fillTriangle(tx, ty, lx, py, rx, py, c);
}

/**
 * The water-droplet avatar. level 0..1 fills it with water (progress to the goal),
 * blink closes the eyes, look shifts the pupils (-1..1).
 */
void drawAvatar(int cx, int cy, float r, float level, Mood mood, bool blink, float look) {
  level = std::max(0.0f, std::min(1.0f, level));
  dropletShape(cx, cy, r + 3, cWaterLight);  // outline
  dropletShape(cx, cy, r, cWaterDeep);
  // Water inside, up to the level (tip at cy - 2.05 r, bottom at cy + r).
  const float top = cy - r * 2.05f, bottom = cy + r;
  const int ly = lroundf(bottom - (bottom - top) * level);
  if (level > 0.0f) {
    s_spr->setClipRect(0, ly, kW, kH - ly);
    dropletShape(cx, cy, r, cWater);
    s_spr->clearClipRect();
    if (level < 0.98f) {
      // Surface line, as wide as the droplet at that height.
      const float a = acosf(1.0f / 2.05f);
      const float py = cy - r * cosf(a);
      float half;
      if (ly >= py) {
        half = sqrtf(std::max(0.0f, r * r - (ly - cy) * static_cast<float>(ly - cy)));
      } else {
        half = r * sinf(a) * (ly - top) / (py - top);
      }
      if (half > 2) s_spr->drawFastHLine(lroundf(cx - half) + 1, ly, lroundf(half * 2) - 1, cWaterLight);
    }
  }
  // Shine
  s_spr->fillEllipse(lroundf(cx - r * 0.55f), lroundf(cy - r * 0.55f), lroundf(r * 0.09f),
                     lroundf(r * 0.2f), cWaterLight);

  // Eyes
  const int ey = lroundf(cy - r * 0.12f);
  const int ex = lroundf(r * 0.36f);
  const int erx = std::max(3, static_cast<int>(lroundf(r * 0.17f)));
  const int ery = std::max(4, static_cast<int>(lroundf(r * 0.23f)));
  for (int side = -1; side <= 1; side += 2) {
    const int x = cx + side * ex;
    if (mood == Mood::Happy) {
      thickLine(x - erx, ey + 2, x, ey - ery / 2, std::max(2.0f, r * 0.07f), cPupil);
      thickLine(x, ey - ery / 2, x + erx, ey + 2, std::max(2.0f, r * 0.07f), cPupil);
    } else if (blink) {
      thickLine(x - erx, ey, x + erx, ey, std::max(2.0f, r * 0.07f), cPupil);
    } else {
      s_spr->fillEllipse(x, ey, erx, ery, cText);
      const int pr = std::max(2, static_cast<int>(lroundf(r * 0.1f)));
      const int px = x + lroundf(look * (erx - pr - 1));
      const int py = ey + (mood == Mood::Thirsty ? ery / 3 : 1);
      s_spr->fillCircle(px, py, pr, cPupil);
      s_spr->fillCircle(px - pr / 2, py - pr / 2, std::max(1, pr / 3), cText);
      if (mood == Mood::Thirsty) {  // sleepy lids, in the colour behind the eye
        const uint8_t behind = (level > 0.0f && ey - ery / 2 >= ly) ? cWater : cWaterDeep;
        s_spr->fillRect(x - erx - 1, ey - ery - 1, erx * 2 + 3, ery, behind);
        s_spr->drawFastHLine(x - erx, ey - 1, erx * 2 + 1, cPupil);
      }
    }
  }
  // Cheeks
  const int chy = ey + ery + lroundf(r * 0.06f);
  s_spr->fillEllipse(cx - ex - lroundf(r * 0.08f), chy, lroundf(r * 0.1f), lroundf(r * 0.06f), cCheek);
  s_spr->fillEllipse(cx + ex + lroundf(r * 0.08f), chy, lroundf(r * 0.1f), lroundf(r * 0.06f), cCheek);
  // Mouth
  const int my = lroundf(cy + r * 0.3f);
  if (mood == Mood::Thirsty) {
    s_spr->fillEllipse(cx, my + 2, lroundf(r * 0.07f), lroundf(r * 0.09f), cPupil);
  } else {
    const int mr = lroundf(r * (mood == Mood::Happy ? 0.2f : 0.15f));
    s_spr->fillArc(cx, my - mr / 2, mr, std::max(0, mr - std::max(2, static_cast<int>(r * 0.06f))), 20,
                   160, cPupil);
  }
}

Mood moodFor(uint32_t total, uint16_t goal) {
  if (total >= goal) return Mood::Happy;
  int32_t day;
  int hour;
  if (store::today(&day, &hour)) {
    uint16_t h[24];
    store::hours(day, h);
    int last = -1;
    for (int i = 0; i <= hour && i < 24; ++i)
      if (h[i] > 0) last = i;
    if (hour >= kFirstHour + 2 && (last < 0 || hour - last >= 3)) return Mood::Thirsty;
  }
  return Mood::Normal;
}

/** Blink every few seconds, look around now and then. */
void animState(bool* blink, float* look) {
  const unsigned long t = millis();
  *blink = (t % 3200) < 180;
  const int phase = (t / 1600) % 4;
  *look = phase == 1 ? -0.8f : (phase == 3 ? 0.8f : 0.0f);
}

/** Text in panel coordinates (top bar, RADAR button). */
void rawText(const char* t, int x, int y, textdatum_t d, const lgfx::IFont* f, uint8_t c) {
  LGFX_Sprite* g = s_spr->raw();
  g->setFont(f);
  g->setTextSize(1);
  g->setTextDatum(d);
  g->setTextColor(c);
  g->drawString(t, x, y);
}

void drawTopBar(const char* title, const char* center) {
  LGFX_Sprite* g = s_spr->raw();
  const int pw = config::kDisplayWidth, bh = S(kTopBarH), ty = bh / 2;
  g->fillRect(0, 0, pw, bh, cPanel);
  g->drawFastHLine(0, bh, pw, cEdge);
  rawText(title, S(10), ty, textdatum_t::middle_left, &ui::font::Sans8, cText);
  if (center) rawText(center, pw / 2, ty, textdatum_t::middle_center, &ui::font::Sans8, cDim);
  tm t;
  if (services::clock::now(&t)) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    rawText(buf, pw - S(8), ty, textdatum_t::middle_right, &ui::font::Sans8, cText);
  }
}

void drawSwitchButton() {
  // Panel coordinates: exactly where the radar screen has its switch button.
  LGFX_Sprite* g = s_spr->raw();
  g->fillRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, S(8), cPanel);
  g->drawRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, S(8), cCyan);
  const int cx = kSwitchX + S(21), cy = kSwitchY + kSwitchH / 2;
  g->drawCircle(cx, cy, S(13), cCyan);
  g->drawCircle(cx, cy, S(7), cEdge);
  g->drawFastHLine(cx - S(13), cy, 2 * S(13) + 1, cEdge);
  g->drawFastVLine(cx, cy - S(13), 2 * S(13) + 1, cEdge);
  g->drawLine(cx, cy, cx + S(9), cy - S(9), cCyan);
  g->fillCircle(cx + S(5), cy - S(2), S(2), cOrange);
  rawText("RADAR", (kSwitchX + S(38) + kSwitchX + kSwitchW) / 2, cy + 1,
          textdatum_t::middle_center, &ui::font::SansBold8, cText);
}

void dayTitle(int32_t day, int offset, char* buf, size_t n) {
  int y, m, d, wd;
  store::dateOf(day, &y, &m, &d, &wd);
  if (offset == 0) {
    snprintf(buf, n, "Bugün, %d %s", d, kMonth[(m - 1) % 12]);
  } else if (offset == 1) {
    snprintf(buf, n, "Dün, %d %s", d, kMonth[(m - 1) % 12]);
  } else {
    snprintf(buf, n, "%d %s %s", d, kMonth[(m - 1) % 12], kDayLong[wd % 7]);
  }
}

// ------------------------------------------------------------------ main screen

void drawHourly(int32_t day, int x, int y, int w, int h) {
  panel(x, y, w, h);
  text("SAATLİK", x + 10, y + 12, textdatum_t::middle_left, &ui::font::Sans8, cDim);
  // Legend
  const int lx = x + w - 136;
  s_spr->fillRect(lx, y + 8, 9, 9, cWater);
  text(s_offset == 0 ? "bugün" : "o gün", lx + 13, y + 12, textdatum_t::middle_left, &ui::font::Sans8, cDim);
  s_spr->fillRect(lx + 70, y + 8, 9, 9, cGhost);
  text(s_offset == 0 ? "dün" : "önceki", lx + 83, y + 12, textdatum_t::middle_left, &ui::font::Sans8, cDim);

  uint16_t a[24], b[24];
  const bool have = store::hours(day, a);
  store::hours(day - 1, b);
  constexpr int h0 = 7, h1 = 22;  // hours shown
  constexpr int n = h1 - h0 + 1;
  int maxv = 500;
  for (int i = h0; i <= h1; ++i) maxv = std::max<int>(maxv, std::max(a[i], b[i]));
  const int gx = x + 10, gw = w - 20, gy = y + 26, gh = h - 26 - 22;
  const float slot = gw / static_cast<float>(n);
  // Reminder hours band
  const int bx0 = gx + lroundf((kFirstHour - h0) * slot), bx1 = gx + lroundf((kLastHour - h0 + 1) * slot);
  s_spr->fillRect(bx0, gy + gh + 1, bx1 - bx0, 2, cEdge);
  for (int i = h0; i <= h1; ++i) {
    const int sx = gx + lroundf((i - h0) * slot);
    const int bw = std::max(4, static_cast<int>(slot) - 3);
    const int hb = b[i] * gh / maxv;
    if (hb > 0) s_spr->fillRect(sx + 1, gy + gh - hb, bw, hb, cGhost);
    const int ha = a[i] * gh / maxv;
    if (ha > 0) s_spr->fillRect(sx + 3, gy + gh - ha, bw - 4, ha, cWater);
    if ((i - h0) % 3 == 0 || i == h1) {
      char t[4];
      snprintf(t, sizeof(t), "%d", i);
      text(t, sx + bw / 2 + 1, gy + gh + 12, textdatum_t::middle_center, &ui::font::Sans8, cFaint);
    }
  }
  if (!have && store::total(day) > 0) {
    text("Saatlik ayrıntı yok", x + w / 2, gy + gh / 2, textdatum_t::middle_center,
         &ui::font::Sans8, cFaint);
  }
}

void drawWeek(int32_t day, int32_t today, int x, int y, int w, int h) {
  panel(x, y, w, h);
  text("SON 7 GÜN", x + 10, y + 12, textdatum_t::middle_left, &ui::font::Sans8, cDim);
  const uint16_t goal = store::goalMl();
  // The 7 days ending at the shown day.
  int maxv = goal;
  for (int i = 0; i < 7; ++i) maxv = std::max<int>(maxv, store::total(day - 6 + i));
  maxv = maxv * 11 / 10;
  const int gx = x + 10, gw = w - 20, gy = y + 26, gh = h - 26 - 20;
  const int slot = gw / 7;
  const int goal_y = gy + gh - goal * gh / maxv;
  for (int xx = gx; xx < gx + gw; xx += 6) s_spr->drawFastHLine(xx, goal_y, 3, cOrange);
  for (int i = 0; i < 7; ++i) {
    const int32_t d = day - 6 + i;
    const uint16_t t = store::total(d);
    const int sx = gx + i * slot;
    const int bw = slot - 10;
    const int hb = t * gh / maxv;
    s_week_rects[i] = Rect{sx, gy, slot, h - 26};
    if (d > today) continue;
    if (hb > 0) s_spr->fillRoundRect(sx + 5, gy + gh - hb, bw, hb, 3, t >= goal ? cGreen : cWater);
    if (d == day) s_spr->drawRoundRect(sx + 3, gy - 2, slot - 6, gh + 18, 5, cCyan);
    int yy, mm, dd, wd;
    store::dateOf(d, &yy, &mm, &dd, &wd);
    text(d == today ? "BGN" : kDayShort[wd % 7], sx + slot / 2, gy + gh + 10, textdatum_t::middle_center,
         &ui::font::Sans8, d == day ? cText : cFaint);
  }
}

void drawMain() {
  applyPalette();
  s_spr->fillScreen(cBg);
  int32_t today = 0;
  const bool have_clock = store::today(&today, nullptr);
  const int32_t day = today - s_offset;
  char buf[48];
  if (have_clock) {
    dayTitle(day, s_offset, buf, sizeof(buf));
  } else {
    snprintf(buf, sizeof(buf), "Saat bekleniyor");
  }
  drawTopBar("SU TAKİBİ", buf);

  // Left: avatar filled to the goal, total, comparison.
  const uint16_t goal = store::goalMl();
  const uint32_t total = have_clock ? store::total(day) : 0;
  const uint32_t prev = have_clock ? store::total(day - 1) : 0;
  constexpr int lpX = 6, lpY = kTopBarH + 5, lpW = 176, lpH = kBtnY - 6 - lpY;
  panel(lpX, lpY, lpW, lpH);
  bool blink;
  float look;
  animState(&blink, &look);
  const Mood mood = s_offset == 0 ? moodFor(total, goal) : (total >= goal ? Mood::Happy : Mood::Normal);
  drawAvatar(lpX + lpW / 2, lpY + 118, 40, static_cast<float>(total) / goal, mood, blink, look);

  fmtVolume(total, buf, sizeof(buf));
  text(buf, lpX + lpW / 2, lpY + 182, textdatum_t::middle_center, &ui::font::SansBold18, cText);
  char g[16];
  fmtVolume(goal, g, sizeof(g));
  const int pct = goal ? static_cast<int>(total * 100 / goal) : 0;
  snprintf(buf, sizeof(buf), "Hedef %s - %%%d", g, pct);
  text(buf, lpX + lpW / 2, lpY + 208, textdatum_t::middle_center, &ui::font::Sans8,
       total >= goal ? cGreen : cDim);
  if (have_clock) {
    const int diff = static_cast<int>(total) - static_cast<int>(prev);
    if (diff == 0) {
      snprintf(buf, sizeof(buf), "Önceki günle aynı");
    } else {
      fmtVolume(static_cast<uint32_t>(diff < 0 ? -diff : diff), g, sizeof(g));
      snprintf(buf, sizeof(buf), "%s %s%s", s_offset == 0 ? "Düne göre" : "Önceki gün", diff > 0 ? "+" : "-", g);
    }
    text(buf, lpX + lpW / 2, lpY + 226, textdatum_t::middle_center, &ui::font::Sans8,
         diff >= 0 ? cGreen : cOrange);
  }

  // Right: hourly bars (vs the day before) and the last 7 days.
  constexpr int rpX = lpX + lpW + 6, rpW = kW - 6 - rpX;
  drawHourly(day, rpX, lpY, rpW, 124);
  drawWeek(day, today, rpX, lpY + 130, rpW, lpH - 130);

  // Bottom buttons
  button(kBtnPrev, "<", have_clock && s_offset < store::kTotalDays - 7);
  button(kBtnNext, ">", s_offset > 0);
  button(kBtnAdd, "+ SU", have_clock && s_offset == 0, cWater);
  button(kBtnGoal, "HEDEF");
  button(kBtnUndo, "GERİ AL", s_offset == 0);
  drawSwitchButton();
  push();
}

long mainKey() {
  long key = s_offset * 7919L + store::goalMl() * 31L;
  int32_t day;
  if (store::today(&day, nullptr)) key += store::total(day - s_offset) * 131L + day;
  tm t;
  if (services::clock::now(&t)) key += (t.tm_hour * 60 + t.tm_min) * 524287L;
  bool blink;
  float look;
  animState(&blink, &look);
  key += (blink ? 3 : 0) + lroundf(look * 10) * 17;
  return key & 0x7fffffffL;
}

// ------------------------------------------------------------------ overlay

void drawBubble(int x, int y, int w, int h, int tail_y) {
  s_spr->fillRoundRect(x, y, w, h, 14, cPanel);
  s_spr->drawRoundRect(x, y, w, h, 14, cWaterLight);
  s_spr->fillTriangle(x + 1, tail_y - 9, x + 1, tail_y + 9, x - 16, tail_y + 4, cPanel);
  s_spr->drawLine(x, tail_y - 9, x - 16, tail_y + 4, cWaterLight);
  s_spr->drawLine(x, tail_y + 9, x - 16, tail_y + 4, cWaterLight);
}

void amountButtons(int y, int h) {
  constexpr int gap = 8;
  const int w = (kW - 12 - 3 * gap) / 4;
  for (int i = 0; i < 4; ++i) {
    Rect r{6 + i * (w + gap), y, w, h};
    s_amount_rects[i] = r;
    s_spr->fillRoundRect(r.x, r.y, r.w, r.h, 10, cPanel);
    s_spr->drawRoundRect(r.x, r.y, r.w, r.h, 10, cWater);
    s_spr->drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, cWater);
    // Glass, filled by size
    const int gx = r.x + 16, gy = r.y + 12, gw = 18, gh = r.h - 24;
    const int fill = gh * (i + 1) / 4;
    s_spr->fillRect(gx + 2, gy + gh - fill, gw - 4, fill, cWater);
    s_spr->drawLine(gx, gy, gx + 2, gy + gh, cWaterLight);
    s_spr->drawLine(gx + gw, gy, gx + gw - 2, gy + gh, cWaterLight);
    s_spr->drawFastHLine(gx + 2, gy + gh, gw - 3, cWaterLight);
    char b[12];
    snprintf(b, sizeof(b), "%u", kAmounts[i]);
    text(b, r.x + r.w / 2 + 14, r.y + r.h / 2 - 7, textdatum_t::middle_center, &ui::font::SansBold12,
         cText);
    text("ml", r.x + r.w / 2 + 14, r.y + r.h / 2 + 13, textdatum_t::middle_center, &ui::font::Sans8,
         cDim);
  }
}

void drawOverlay() {
  applyPalette();
  s_spr->fillScreen(cBg);
  int32_t today = 0;
  int hour = 0;
  const bool have_clock = store::today(&today, &hour);
  const uint16_t goal = store::goalMl();
  const uint32_t total = have_clock ? store::total(today) : 0;
  char buf[64], v[16], g[16];
  bool blink;
  float look;
  animState(&blink, &look);

  if (s_mode == Mode::Goal) {
    drawTopBar("SU TAKİBİ", "Günlük hedef");
    drawAvatar(84, 150, 42, static_cast<float>(s_goal_edit) / kGoalMax, Mood::Normal, blink, look);
    drawBubble(160, 40, 308, 70, 76);
    text("Günlük hedef", 176, 62, textdatum_t::middle_left, &ui::font::SansBold12, cText);
    text("Değiştirene kadar bu hedef kalır.", 176, 92, textdatum_t::middle_left, &ui::font::Sans8,
         cDim);
    s_ov_minus = Rect{170, 128, 64, 64};
    s_ov_plus = Rect{404, 128, 64, 64};
    button(s_ov_minus, "-", s_goal_edit > kGoalMin, cWater);
    button(s_ov_plus, "+", s_goal_edit < kGoalMax, cWater);
    text("-", s_ov_minus.x + 32, s_ov_minus.y + 31, textdatum_t::middle_center, &ui::font::SansBold24,
         cText);
    text("+", s_ov_plus.x + 32, s_ov_plus.y + 33, textdatum_t::middle_center, &ui::font::SansBold24,
         cText);
    fmtVolume(s_goal_edit, v, sizeof(v));
    text(v, (234 + 404) / 2, 160, textdatum_t::middle_center, &ui::font::SansBold24, cText);
    text("250 ml adımlarla", (234 + 404) / 2, 196, textdatum_t::middle_center, &ui::font::Sans8,
         cFaint);
    s_ov_btn1 = Rect{170, kBtnY, 140, kBtnH};
    s_ov_btn2 = Rect{326, kBtnY, 140, kBtnH};
    button(s_ov_btn1, "Vazgeç");
    button(s_ov_btn2, "Kaydet", true, cGreen);
    push();
    return;
  }

  if (s_mode == Mode::Thanks) {
    drawTopBar("SU TAKİBİ", nullptr);
    drawAvatar(150, 196, 54, static_cast<float>(total) / goal, Mood::Happy, false, 0);
    drawBubble(240, 70, 220, 96, 120);
    text("Afiyet olsun!", 258, 100, textdatum_t::middle_left, &ui::font::SansBold12, cText);
    fmtVolume(s_thanks_ml, v, sizeof(v));
    fmtVolume(total, g, sizeof(g));
    snprintf(buf, sizeof(buf), "+%s  -  bugün %s", v, g);
    text(buf, 258, 134, textdatum_t::middle_left, &ui::font::Sans8, cDim);
    push();
    return;
  }

  // Ask (hourly reminder) or Add ("+ SU" button)
  drawTopBar("SU TAKİBİ", nullptr);
  drawAvatar(86, 140, 46, static_cast<float>(total) / goal,
             s_mode == Mode::Ask ? moodFor(total, goal) : Mood::Normal, blink, look);
  drawBubble(166, 36, 302, 110, 96);
  if (s_mode == Mode::Ask) {
    text("Su içtin mi?", 184, 72, textdatum_t::middle_left, &ui::font::SansBold18, cText);
    snprintf(buf, sizeof(buf), "Saat %02d:00", hour);
  } else {
    text("Ne kadar içtin?", 184, 72, textdatum_t::middle_left, &ui::font::SansBold18, cText);
    buf[0] = '\0';
  }
  fmtVolume(total, v, sizeof(v));
  fmtVolume(goal, g, sizeof(g));
  char line[64];
  snprintf(line, sizeof(line), "%s%sBugün %s / %s", buf, buf[0] ? "  -  " : "", v, g);
  text(line, 184, 116, textdatum_t::middle_left, &ui::font::Sans8, cDim);

  amountButtons(196, 64);
  s_ov_btn1 = Rect{kW / 2 - 90, kBtnY + 4, 180, kBtnH - 4};
  button(s_ov_btn1, s_mode == Mode::Ask ? "İçmedim" : "Vazgeç");
  s_ov_btn2 = Rect{0, 0, 0, 0};
  push();
}

void closeOverlay() { s_mode = Mode::None; }

void startMode(Mode m) {
  s_mode = m;
  s_mode_since = millis();
  s_last_anim = 0;
  if (bindFrame()) drawOverlay();
}

}  // namespace

// ------------------------------------------------------------------ public

void enter() {
  s_offset = 0;
  if (!bindFrame()) return;
  drawMain();
  s_drawn_key = mainKey();
}

void tick() {
  if (s_spr == nullptr) {
    enter();
    return;
  }
  const long key = mainKey();
  if (key == s_drawn_key) return;
  drawMain();
  s_drawn_key = key;
}

bool tap(int px, int py) {
  const int x = Canvas::toDesignX(px), y = Canvas::toDesignY(py);  // layout is in design units
  int32_t today;
  const bool have_clock = store::today(&today, nullptr);
  bool changed = false;
  if (kBtnPrev.hit(x, y) && have_clock && s_offset < store::kTotalDays - 7) {
    ++s_offset;
    changed = true;
  } else if (kBtnNext.hit(x, y) && s_offset > 0) {
    --s_offset;
    changed = true;
  } else if (kBtnAdd.hit(x, y) && have_clock && s_offset == 0) {
    startMode(Mode::Add);
    return true;
  } else if (kBtnGoal.hit(x, y)) {
    s_goal_edit = store::goalMl();
    startMode(Mode::Goal);
    return true;
  } else if (kBtnUndo.hit(x, y) && s_offset == 0) {
    changed = store::undoLast();
  } else {
    // Tap a day in the 7-day chart to show it.
    for (int i = 0; i < 7; ++i) {
      if (s_week_rects[i].w > 0 && s_week_rects[i].hit(x, y)) {
        const int off = s_offset + (6 - i);
        if (off >= 0 && off != s_offset) {
          s_offset = off;
          changed = true;
        }
        break;
      }
    }
  }
  if (changed && s_spr) {
    drawMain();
    s_drawn_key = mainKey();
  }
  return changed;
}

bool reminderDue() {
  if (s_mode != Mode::None) return false;
  tm t;
  if (!services::clock::now(&t)) return false;
  if (t.tm_hour < kFirstHour || t.tm_hour > kLastHour || t.tm_min >= 30) return false;
  int32_t day;
  int hour;
  if (!store::today(&day, &hour)) return false;
  if (day == s_asked_day && hour == s_asked_hour) return false;
  uint16_t h[24];
  store::hours(day, h);
  if (h[hour] > 0) {  // already logged a drink this hour: no need to ask
    s_asked_day = day;
    s_asked_hour = hour;
    return false;
  }
  return true;
}

void openReminder() {
  store::today(&s_asked_day, &s_asked_hour);
#ifndef RADAR3D_HOST_TEST
  // Flash the backlight twice so the question is noticed.
  const uint8_t level = services::display_prefs::get().brightness;
  for (int i = 0; i < 2; ++i) {
    tft.setBrightness(0);
    delay(160);
    tft.setBrightness(level);
    delay(160);
  }
#endif
  startMode(Mode::Ask);
}

bool overlayActive() { return s_mode != Mode::None; }

bool overlayTick() {
  if (s_mode == Mode::None) return false;
  const unsigned long now = millis();
  if (s_mode == Mode::Thanks && now - s_mode_since >= kThanksMs) {
    closeOverlay();
    return false;
  }
  if (s_mode == Mode::Ask && now - s_mode_since >= kAskTimeoutMs) {
    closeOverlay();  // unanswered: counts as nothing logged for this hour
    return false;
  }
  if (now - s_last_anim >= 200) {  // avatar animation
    s_last_anim = now;
    drawOverlay();
  }
  return true;
}

void overlayTap(int px, int py) {
  const int x = Canvas::toDesignX(px), y = Canvas::toDesignY(py);
  if (s_mode == Mode::Ask || s_mode == Mode::Add) {
    for (int i = 0; i < 4; ++i) {
      if (s_amount_rects[i].hit(x, y)) {
        if (store::add(kAmounts[i])) {
          s_thanks_ml = kAmounts[i];
          startMode(Mode::Thanks);
        } else {
          closeOverlay();
        }
        return;
      }
    }
    if (s_ov_btn1.hit(x, y)) closeOverlay();
    return;
  }
  if (s_mode == Mode::Goal) {
    if (s_ov_minus.hit(x, y) && s_goal_edit > kGoalMin) {
      s_goal_edit -= kGoalStep;
      drawOverlay();
    } else if (s_ov_plus.hit(x, y) && s_goal_edit < kGoalMax) {
      s_goal_edit += kGoalStep;
      drawOverlay();
    } else if (s_ov_btn1.hit(x, y)) {
      closeOverlay();
    } else if (s_ov_btn2.hit(x, y)) {
      store::setGoalMl(s_goal_edit);
      closeOverlay();
    }
    return;
  }
  if (s_mode == Mode::Thanks) closeOverlay();
}

}  // namespace ui::water
