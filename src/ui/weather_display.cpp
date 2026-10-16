#include "ui/weather_display.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/sht30.h"
#include "services/weather_service.h"
#include "ui/weather_icons.h"

namespace ui {

namespace {

using services::weather::Forecast;
using services::weather::Slot;

unsigned long s_drawn_fetch_ms = 0;
bool s_drawn_any = false;

// Red/blue order is handled by the panel setting (settings menu), so colours are plain RGB.
uint16_t col(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

int s_label_cx = config::kDisplayWidth / 2;  // column centre used by drawLabel

/** Big temperature ("18" + degree ring + "C") centred on cx. */
void drawTemperature(int cx, int cy, int temp_c, const lgfx::GFXfont* big,
                     const lgfx::GFXfont* small, int ring_r, uint16_t color, uint16_t bg) {
  char num[8];
  snprintf(num, sizeof(num), "%d", temp_c);
  displayFontSetBitmap(tft, big);
  const int num_w = tft.textWidth(num);
  const int ring_w = ring_r * 2 + 4;
  displayFontSetBitmap(tft, small);
  const int unit_w = tft.textWidth("C");
  const int total = num_w + ring_w + unit_w;
  int x = cx - total / 2;

  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(color, bg);
  displayFontSetBitmap(tft, big);
  tft.drawString(num, x, cy);
  x += num_w;

  const int ring_cy = cy - tft.fontHeight() / 3;
  tft.drawCircle(x + ring_r + 2, ring_cy, ring_r, color);
  tft.drawCircle(x + ring_r + 2, ring_cy, ring_r - 1, color);
  x += ring_w;

  displayFontSetBitmap(tft, small);
  tft.drawString("C", x, cy + 2);
}

void drawLabel(const char* text, int y, const lgfx::GFXfont* font, uint16_t color,
               uint16_t bg) {
  displayFontSetBitmap(tft, font);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(color, bg);
  tft.drawString(text, s_label_cx, y);
}

void drawSlot(const Slot& s, int icon_cx, int icon_cy, int icon_r, int temp_cx,
              const lgfx::GFXfont* big, const lgfx::GFXfont* small, int ring_r,
              int label_y, const weather::IconPalette<uint16_t>& pal, uint16_t white,
              uint16_t soft, uint16_t bg) {
  if (!s.valid) {
    drawLabel("--", icon_cy, big, soft, bg);
    return;
  }
  weather::drawWeatherIcon(tft, icon_cx, icon_cy, icon_r, s.cond, s.day, pal);
  drawTemperature(temp_cx, icon_cy, s.temp_c, big, small, ring_r, white, bg);
  drawLabel(services::weather::conditionLabel(s.cond), label_y, &fonts::FreeSansBold9pt7b,
            soft, bg);
}

void drawScreen(const Forecast& f) {
  const uint16_t bg = 0x0000;
  const uint16_t white = 0xFFFF;
  const uint16_t soft = col(190, 200, 215);
  const uint16_t dim = col(130, 140, 158);
  const uint16_t line = col(50, 58, 78);
  weather::IconPalette<uint16_t> pal{bg,
                                     col(255, 200, 40),
                                     col(225, 225, 200),
                                     col(215, 225, 235),
                                     col(130, 145, 165),
                                     col(70, 150, 255),
                                     col(240, 245, 255),
                                     col(255, 210, 0),
                                     col(170, 180, 195)};

  tft.fillScreen(bg);
  // Two columns on the 480x320 panel: now (left) and about two hours later (right).
  s_label_cx = 120;
  drawLabel("SIMDI", 40, &fonts::FreeSansBold12pt7b, dim, bg);
  drawSlot(f.now, 70, 130, 40, 172, &fonts::FreeSansBold24pt7b, &fonts::FreeSansBold12pt7b, 5,
           200, pal, white, soft, bg);
  s_label_cx = 360;
  drawLabel("2 SAAT SONRA", 40, &fonts::FreeSansBold12pt7b, dim, bg);
  drawSlot(f.later, 310, 130, 40, 412, &fonts::FreeSansBold24pt7b, &fonts::FreeSansBold12pt7b,
           5, 200, pal, white, soft, bg);
  tft.drawFastVLine(240, 30, 190, line);

  // Indoor reading from the SHT30 under the forecast.
  if (services::sht30::valid()) {
    char buf[40];
    snprintf(buf, sizeof(buf), "IC ORTAM  %.1f C   %%%d", services::sht30::temperatureC(),
             static_cast<int>(lroundf(services::sht30::humidity())));
    tft.drawFastHLine(60, 240, 360, line);
    s_label_cx = 240;
    drawLabel(buf, 274, &fonts::FreeSansBold12pt7b, soft, bg);
  }

  tft.setTextPadding(0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void weatherDisplayEnter() {
  Forecast f;
  unsigned long fetched = 0;
  if (!services::weather::snapshot(&f, &fetched)) {
    f = Forecast();
  }
  drawScreen(f);
  s_drawn_fetch_ms = fetched;
  s_drawn_any = true;
}

void weatherDisplayTick() {
  Forecast f;
  unsigned long fetched = 0;
  if (!services::weather::snapshot(&f, &fetched)) {
    return;
  }
  if (s_drawn_any && fetched == s_drawn_fetch_ms) {
    return;
  }
  drawScreen(f);
  s_drawn_fetch_ms = fetched;
  s_drawn_any = true;
}

}  // namespace ui
