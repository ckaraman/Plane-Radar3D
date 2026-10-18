#include "ui/weather_display.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "services/clock_service.h"
#include "services/sht30.h"
#include "services/usb_feed.h"
#include "services/weather_service.h"
#include "ui/radar3d.h"
#include "ui/screen_switch.h"
#include "ui/weather_icons.h"

#ifdef RADAR3D_HOST_TEST
namespace ui::radar3d {
void hostCaptureFrame(LGFX_Sprite& spr);  // desktop preview harness
}
#endif

namespace ui {

namespace {

using services::weather::Day;
using services::weather::Forecast;

constexpr int kW = config::kDisplayWidth;
constexpr int kH = config::kDisplayHeight;
constexpr int kTopBarH = 22;

// Palette of the forecast screen (index = colour number in the 4-bit sprite).
enum Pal : uint8_t {
  wBg = 0,
  wPanel,
  wEdge,
  wText,
  wDim,
  wFaint,
  wSun,
  wMoon,
  wCloud,
  wCloudDark,
  wRain,
  wWarm,
  wCool,
  wBolt,
  wGreen,
  wRed,
};
struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kPalette[16] = {
    {0, 0, 0},       {12, 26, 38},    {48, 86, 110},   {255, 255, 255},
    {175, 188, 198}, {105, 120, 132}, {255, 200, 40},  {235, 228, 195},
    {220, 228, 238}, {135, 150, 170}, {80, 160, 255},  {255, 160, 60},
    {110, 210, 255}, {255, 228, 0},   {80, 235, 140},  {255, 70, 60},
};

const char* const kDayShort[7] = {"PAZ", "PZT", "SAL", "CAR", "PER", "CUM", "CMT"};
const char* const kDayLong[7] = {"Pazar",    "Pazartesi", "Sali",     "Carsamba",
                                 "Persembe", "Cuma",      "Cumartesi"};
const char* const kMonth[12] = {"Ocak",   "Subat",   "Mart",  "Nisan", "Mayis", "Haziran",
                                "Temmuz", "Agustos", "Eylul", "Ekim",  "Kasim", "Aralik"};
const char* const kCompass[8] = {"K", "KD", "D", "GD", "G", "GB", "B", "KB"};

LGFX_Sprite* s_spr = nullptr;
long s_drawn_key = -1;

const ui::weather::IconPalette<Pal> kIcons{wPanel, wSun,  wMoon, wCloud,    wCloudDark,
                                               wRain,  wText, wBolt, wCloudDark};

void font(const lgfx::IFont* f) {
  s_spr->setFont(f);
  s_spr->setTextSize(1);
}

/** Text with a small degree ring after it ("17°"); returns the total width. */
int drawDegrees(const char* num, int x, int y, textdatum_t datum, const lgfx::IFont* f,
                uint8_t color, int ring_r) {
  font(f);
  const int w = s_spr->textWidth(num);
  const int total = w + ring_r * 2 + 3;
  int left = x;
  if (datum == textdatum_t::middle_center) left = x - total / 2;
  if (datum == textdatum_t::middle_right) left = x - total;
  s_spr->setTextDatum(textdatum_t::middle_left);
  s_spr->setTextColor(color);
  s_spr->drawString(num, left, y);
  const int fh = s_spr->fontHeight();
  const int rx = left + w + ring_r + 2;
  const int ry = y - fh / 3 + ring_r / 2;
  s_spr->drawCircle(rx, ry, ring_r, color);
  if (ring_r >= 3) s_spr->drawCircle(rx, ry, ring_r - 1, color);
  return total;
}

void text(const char* t, int x, int y, textdatum_t datum, const lgfx::IFont* f, uint8_t color) {
  font(f);
  s_spr->setTextDatum(datum);
  s_spr->setTextColor(color);
  s_spr->drawString(t, x, y);
}

void panel(int x, int y, int w, int h) {
  s_spr->fillRoundRect(x, y, w, h, 8, wPanel);
  s_spr->drawRoundRect(x, y, w, h, 8, wEdge);
}

void drop(int cx, int cy, int r, uint8_t color) {
  s_spr->fillCircle(cx, cy + r / 2, r, color);
  s_spr->fillTriangle(cx - r, cy + r / 2, cx + r, cy + r / 2, cx, cy - r - r / 2, color);
}

void drawTopBar() {
  s_spr->fillRect(0, 0, kW, kTopBarH, wPanel);
  s_spr->drawFastHLine(0, kTopBarH, kW, wEdge);
  text("HAVA DURUMU", 10, 11, textdatum_t::middle_left, &fonts::Font2, wText);
  tm t;
  if (services::clock::now(&t)) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%d %s %s", t.tm_mday, kMonth[t.tm_mon % 12],
             kDayLong[t.tm_wday % 7]);
    text(buf, kW / 2, 11, textdatum_t::middle_center, &fonts::Font2, wDim);
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    text(buf, kW - 8, 11, textdatum_t::middle_right, &fonts::Font2, wText);
  }
}

// --- Outdoor, now -------------------------------------------------------------

void drawOutdoor(const Forecast& f, int x, int y, int w, int h) {
  panel(x, y, w, h);
  text("DISARIDA", x + 10, y + 12, textdatum_t::middle_left, &fonts::Font2, wDim);
  if (!f.now.valid) {
    text("--", x + w / 2, y + h / 2, textdatum_t::middle_center, &fonts::FreeSansBold18pt7b,
         wFaint);
    return;
  }
  weather::drawWeatherIcon(*s_spr, x + 50, y + 62, 34, f.now.cond, f.now.day, kIcons);

  char buf[24];
  snprintf(buf, sizeof(buf), "%d", f.now.temp_c);
  drawDegrees(buf, x + 96, y + 52, textdatum_t::middle_left, &fonts::FreeSansBold24pt7b, wText,
              5);
  text(services::weather::conditionLabel(f.now.cond), x + 98, y + 92,
       textdatum_t::middle_left, &fonts::FreeSansBold9pt7b, wDim);

  // Detail rows on the right of the card.
  const int lx = x + w - 130, vx = x + w - 10;
  int ry = y + 34;
  auto row = [&](const char* label, const char* value, bool degrees) {
    text(label, lx, ry, textdatum_t::middle_left, &fonts::Font2, wFaint);
    if (degrees) {
      drawDegrees(value, vx, ry, textdatum_t::middle_right, &fonts::Font2, wText, 2);
    } else {
      text(value, vx, ry, textdatum_t::middle_right, &fonts::Font2, wText);
    }
    ry += 20;
  };
  snprintf(buf, sizeof(buf), "%d", f.feels_c);
  row("Hissedilen", buf, true);
  if (f.humidity >= 0) {
    snprintf(buf, sizeof(buf), "%%%d", f.humidity);
    row("Nem", buf, false);
  }
  if (f.wind_kmh >= 0) {
    if (f.wind_dir >= 0) {
      snprintf(buf, sizeof(buf), "%d km/s %s", f.wind_kmh,
               kCompass[((f.wind_dir + 22) / 45) % 8]);
    } else {
      snprintf(buf, sizeof(buf), "%d km/s", f.wind_kmh);
    }
    row("Ruzgar", buf, false);
  }

  // Sunrise / sunset along the bottom of the card.
  if (f.sunrise_min >= 0 && f.sunset_min >= 0) {
    const int by = y + h - 16;
    s_spr->drawFastHLine(x + 10, by - 13, w - 20, wEdge);
    snprintf(buf, sizeof(buf), "Gun dogumu  %02d:%02d", f.sunrise_min / 60, f.sunrise_min % 60);
    s_spr->fillCircle(x + 18, by + 1, 4, wSun);
    s_spr->fillRect(x + 12, by + 2, 13, 4, wPanel);  // half sun on the horizon
    s_spr->drawFastHLine(x + 11, by + 2, 15, wSun);
    text(buf, x + 32, by, textdatum_t::middle_left, &fonts::Font2, wDim);
    snprintf(buf, sizeof(buf), "Gun batimi  %02d:%02d", f.sunset_min / 60, f.sunset_min % 60);
    const int sx = x + w / 2 + 12;
    s_spr->fillCircle(sx + 6, by + 1, 4, wWarm);
    s_spr->fillRect(sx, by + 2, 13, 4, wPanel);
    s_spr->drawFastHLine(sx - 1, by + 2, 15, wWarm);
    text(buf, sx + 20, by, textdatum_t::middle_left, &fonts::Font2, wDim);
  }
}

// --- Indoor (SHT30) -----------------------------------------------------------

void drawIndoor(int x, int y, int w, int h) {
  panel(x, y, w, h);
  text("ICERIDE", x + 10, y + 12, textdatum_t::middle_left, &fonts::Font2, wDim);
  text("SHT30", x + w - 10, y + 12, textdatum_t::middle_right, &fonts::Font2, wFaint);
  if (!services::sht30::valid()) {
    text("Sensor", x + w / 2, y + h / 2 - 10, textdatum_t::middle_center,
         &fonts::FreeSansBold9pt7b, wFaint);
    text("okunamiyor", x + w / 2, y + h / 2 + 10, textdatum_t::middle_center,
         &fonts::FreeSansBold9pt7b, wFaint);
    return;
  }
  const float t = services::sht30::temperatureC();
  const int rh = static_cast<int>(lroundf(services::sht30::humidity()));
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", t);
  // Centre "23.4°C" in the card.
  font(&fonts::FreeSansBold12pt7b);
  const int cw = s_spr->textWidth("C");
  font(&fonts::FreeSansBold24pt7b);
  const int total = s_spr->textWidth(buf) + 13 + cw + 1;
  const int left = x + (w - total) / 2;
  const int tw = drawDegrees(buf, left, y + 48, textdatum_t::middle_left,
                             &fonts::FreeSansBold24pt7b, wText, 5);
  text("C", left + tw + 1, y + 52, textdatum_t::middle_left, &fonts::FreeSansBold12pt7b, wText);

  // Humidity with a drop and a small bar.
  snprintf(buf, sizeof(buf), "%%%d", rh);
  font(&fonts::FreeSansBold12pt7b);
  const int hw = s_spr->textWidth(buf) + 18;
  const int hx = x + (w - hw) / 2;
  drop(hx + 5, y + 82, 5, wRain);
  text(buf, hx + 18, y + 83, textdatum_t::middle_left, &fonts::FreeSansBold12pt7b, wText);
  const int bx = x + 16, bw = w - 32, byy = y + 98;
  s_spr->fillRoundRect(bx, byy, bw, 5, 2, wBg);
  s_spr->fillRoundRect(bx, byy, std::max(4, bw * std::min(rh, 100) / 100), 5, 2, wRain);

  // Comfort label.
  const char* label = "KONFORLU";
  uint8_t col = wGreen;
  if (t >= 27.0f) {
    label = "SICAK";
    col = wRed;
  } else if (t < 18.0f) {
    label = "SERIN";
    col = wCool;
  } else if (rh < 30) {
    label = "KURU";
    col = wWarm;
  } else if (rh > 65) {
    label = "NEMLI";
    col = wRain;
  }
  text(label, x + w / 2, y + h - 12, textdatum_t::middle_center, &fonts::FreeSansBold9pt7b, col);
}

// --- Days ---------------------------------------------------------------------

void drawDay(const Day& d, bool today, int x, int y, int w, int h) {
  panel(x, y, w, h);
  const int cx = x + w / 2;
  char buf[16];
  if (today) {
    snprintf(buf, sizeof(buf), "BUGUN");
  } else {
    snprintf(buf, sizeof(buf), "%s %d", kDayShort[d.wday % 7], d.mday);
  }
  text(buf, cx, y + 13, textdatum_t::middle_center, &fonts::FreeSansBold9pt7b,
       today ? wCool : wText);
  weather::drawWeatherIcon(*s_spr, cx, y + 44, 18, d.cond, true, kIcons);

  // "19° 12°": max bright, min dim, centred together.
  char mx[8], mn[8];
  snprintf(mx, sizeof(mx), "%d", d.max_c);
  snprintf(mn, sizeof(mn), "%d", d.min_c);
  font(&fonts::FreeSansBold12pt7b);
  const int w1 = s_spr->textWidth(mx) + 9;
  font(&fonts::FreeSansBold9pt7b);
  const int w2 = s_spr->textWidth(mn) + 7;
  const int left = cx - (w1 + 6 + w2) / 2;
  drawDegrees(mx, left, y + 76, textdatum_t::middle_left, &fonts::FreeSansBold12pt7b, wText, 3);
  drawDegrees(mn, left + w1 + 6, y + 78, textdatum_t::middle_left, &fonts::FreeSansBold9pt7b,
              wCool, 2);

  if (d.pop >= 0) {
    snprintf(buf, sizeof(buf), "%%%d", d.pop);
    font(&fonts::Font2);
    const int pw = s_spr->textWidth(buf) + 12;
    const int px = cx - pw / 2;
    drop(px + 3, y + h - 13, 3, d.pop >= 50 ? wRain : wFaint);
    text(buf, px + 12, y + h - 12, textdatum_t::middle_left, &fonts::Font2,
         d.pop >= 50 ? wRain : wDim);
  }
}

void drawSwitchButton() {
  s_spr->fillRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, 8, wPanel);
  s_spr->drawRoundRect(kSwitchX, kSwitchY, kSwitchW, kSwitchH, 8, wCool);
  // Small radar: two rings, crosshair and a blip.
  const int cx = kSwitchX + 21, cy = kSwitchY + kSwitchH / 2;
  s_spr->drawCircle(cx, cy, 13, wCool);
  s_spr->drawCircle(cx, cy, 7, wEdge);
  s_spr->drawFastHLine(cx - 13, cy, 27, wEdge);
  s_spr->drawFastVLine(cx, cy - 13, 27, wEdge);
  s_spr->drawLine(cx, cy, cx + 9, cy - 9, wCool);
  s_spr->fillCircle(cx + 5, cy - 2, 2, wWarm);
  text("RADAR", kSwitchX + 39, cy + 1, textdatum_t::middle_left, &fonts::Font2, wText);
}

void drawStatusLine(bool have, unsigned long fetched_ms) {
  char buf[48];
  if (have) {
    const unsigned long mins = (millis() - fetched_ms) / 60000UL;
    if (mins == 0) {
      snprintf(buf, sizeof(buf), "Open-Meteo  -  az once guncellendi");
    } else {
      snprintf(buf, sizeof(buf), "Open-Meteo  -  %lu dk once guncellendi", mins);
    }
  } else {
    snprintf(buf, sizeof(buf), "Open-Meteo");
  }
  text(buf, 10, kSwitchY + kSwitchH / 2, textdatum_t::middle_left, &fonts::Font2, wFaint);
}

void drawNoData(int y) {
  const char* l1 = "Hava durumu bekleniyor...";
  const char* l2 = services::usb_feed::active()
                       ? "USB koprusunun yeni surumu gerekli (kurulum sayfasi, adim 4)"
                       : "Wi-Fi baglaninca birkac saniye icinde gelir";
  if (!services::weather::enabled()) {
    l1 = "Hava durumu kapali";
    l2 = "Kurulum sayfasindan acabilirsiniz";
  }
  panel(6, y, kW - 12, 104);
  text(l1, kW / 2, y + 38, textdatum_t::middle_center, &fonts::FreeSansBold12pt7b, wDim);
  text(l2, kW / 2, y + 70, textdatum_t::middle_center, &fonts::Font2, wFaint);
}

void applyPalette() {
  for (int i = 0; i < 16; ++i) s_spr->setPaletteColor(i, kPalette[i].r, kPalette[i].g, kPalette[i].b);
}

void drawScreen() {
  Forecast f;
  unsigned long fetched = 0;
  const bool have = services::weather::snapshot(&f, &fetched);

  applyPalette();
  s_spr->fillScreen(wBg);
  drawTopBar();

  constexpr int kTopY = kTopBarH + 5;
  constexpr int kTopH = 128;
  constexpr int kIndoorW = 160;
  drawOutdoor(have ? f : Forecast(), 6, kTopY, kW - 12 - kIndoorW - 6, kTopH);
  drawIndoor(kW - 6 - kIndoorW, kTopY, kIndoorW, kTopH);

  constexpr int kDaysY = kTopY + kTopH + 6;
  constexpr int kDaysH = kSwitchY - 6 - kDaysY;
  if (have && f.day_count > 0) {
    const int n = f.day_count;
    constexpr int gap = 6;
    const int w = (kW - 12 - (n - 1) * gap) / n;
    const int x0 = (kW - (n * w + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; ++i) drawDay(f.days[i], i == 0, x0 + i * (w + gap), kDaysY, w, kDaysH);
  } else {
    drawNoData(kDaysY);
  }

  drawStatusLine(have, fetched);
  drawSwitchButton();

#ifdef RADAR3D_HOST_TEST
  ui::radar3d::hostCaptureFrame(*s_spr);
#else
  s_spr->pushSprite(0, 0);
#endif
}

/** Everything the screen shows that can change; redraw when it does. */
long screenKey() {
  long key = 0;
  unsigned long fetched = 0;
  if (services::weather::snapshot(nullptr, &fetched)) {
    key = static_cast<long>(fetched % 100000UL) * 7 + static_cast<long>((millis() - fetched) / 60000UL);
  }
  tm t;
  if (services::clock::now(&t)) key += (t.tm_hour * 60 + t.tm_min) * 131071L;
  if (services::sht30::valid()) {
    key += lroundf(services::sht30::temperatureC() * 10.0f) * 1009L +
           lroundf(services::sht30::humidity()) * 17L;
  }
  key += services::weather::enabled() ? 3 : 0;
  key += services::usb_feed::active() ? 5 : 0;
  return key & 0x7fffffffL;
}

}  // namespace

void weatherDisplayEnter() {
  s_spr = ui::radar3d::frame();
  if (s_spr == nullptr) return;
  drawScreen();
  s_drawn_key = screenKey();
}

void weatherDisplayTick() {
  if (s_spr == nullptr) {
    weatherDisplayEnter();
    return;
  }
  const long key = screenKey();
  if (key == s_drawn_key) return;
  drawScreen();
  s_drawn_key = key;
}

}  // namespace ui
