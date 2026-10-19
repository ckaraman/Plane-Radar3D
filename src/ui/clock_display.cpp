#include "ui/clock_display.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock_service.h"
#include "services/sht30.h"
#include "services/weather_service.h"
#include "ui/fonts_tr.h"
#include "ui/screen_switch.h"

namespace ui {

namespace {

constexpr uint16_t kBackground = 0x0000;
constexpr uint16_t kTimeColor = 0xFFFF;
constexpr int kCenterX = config::kDisplayWidth / 2;
constexpr int kTimeY = 120;
constexpr int kDateY = 210;
constexpr int kInfoY = 280;
constexpr int kBtnY = 4;
constexpr int kWeatherX = ui::kSwitchX;
constexpr int kWaterX = ui::kWaterBtnX;

int s_last_minute = -1;
int s_last_yday = -1;
int s_last_info_key = -99999;

// UTF-8: the display fonts (ui/fonts_tr.h) have the Turkish letters.
const char* const kWeekdaysEn[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* const kMonthsEn[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const char* const kWeekdaysTr[] = {"PAZAR", "PAZARTESİ", "SALI", "ÇARŞAMBA",
                                   "PERŞEMBE", "CUMA", "CUMARTESİ"};
const char* const kMonthsTr[] = {"OCAK",   "ŞUBAT",   "MART",  "NİSAN", "MAYIS", "HAZİRAN",
                                 "TEMMUZ", "AĞUSTOS", "EYLÜL", "EKİM",  "KASIM", "ARALIK"};

uint16_t dimColor() { return tft.color565(125, 139, 150); }
uint16_t softColor() { return tft.color565(190, 200, 215); }

/** One info block: small label above "23.4°C  %48", centred on cx. */
void drawInfoBlock(int cx, const char* label, const char* temp, int humidity) {
  displayFontSetBitmap(tft, &ui::font::Sans9);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(dimColor(), kBackground);
  tft.drawString(label, cx, kInfoY - 16);
  char buf[24];
  if (humidity >= 0) {
    snprintf(buf, sizeof(buf), "%s°C   %%%d", temp, humidity);
  } else {
    snprintf(buf, sizeof(buf), "%s°C", temp);
  }
  displayFontSetBitmap(tft, &ui::font::SansBold12);
  tft.setTextColor(softColor(), kBackground);
  tft.drawString(buf, cx, kInfoY + 10);
}

/** Bottom: room (SHT30) and outdoor (Open-Meteo) temperature and humidity. */
void drawInfoLine() {
  tft.fillRect(0, kInfoY - 30, config::kDisplayWidth, 60, kBackground);
  const bool indoor = services::sht30::valid();
  services::weather::Forecast f;
  const bool outdoor = services::weather::enabled() &&
                       services::weather::snapshot(&f, nullptr) && f.now.valid;
  char t[12];
  const int left = outdoor ? config::kDisplayWidth / 4 : kCenterX;
  const int right = indoor ? config::kDisplayWidth * 3 / 4 : kCenterX;
  if (indoor) {
    snprintf(t, sizeof(t), "%.1f", services::sht30::temperatureC());
    drawInfoBlock(left, "İÇ ORTAM", t, static_cast<int>(lroundf(services::sht30::humidity())));
  }
  if (outdoor) {
    snprintf(t, sizeof(t), "%d", f.now.temp_c);
    drawInfoBlock(right, "DIŞ ORTAM", t, f.humidity);
  }
  if (indoor && outdoor) tft.drawFastVLine(kCenterX, kInfoY - 24, 48, tft.color565(50, 58, 78));
  tft.setTextDatum(textdatum_t::top_left);
}

/** Top right: water (droplet) and forecast buttons. */
void drawButtons() {
  const uint16_t panel = tft.color565(12, 26, 38), edge = tft.color565(48, 86, 110);
  const uint16_t cyan = tft.color565(110, 210, 255);
  ui::drawWaterButton(tft, kWaterX, kBtnY, ui::kWaterBtnW, ui::kWaterBtnH, panel, cyan,
                      tft.color565(60, 160, 255), static_cast<uint16_t>(0xFFFF),
                      static_cast<uint16_t>(0x0000));
  tft.fillRoundRect(kWeatherX, kBtnY, ui::kSwitchW, ui::kSwitchH, 8, panel);
  tft.drawRoundRect(kWeatherX, kBtnY, ui::kSwitchW, ui::kSwitchH, 8, edge);
  // sun + cloud
  const int cx = kWeatherX + 22, cy = kBtnY + ui::kSwitchH / 2;
  tft.fillCircle(cx + 4, cy - 4, 7, tft.color565(255, 170, 60));
  tft.fillCircle(cx - 4, cy + 4, 6, tft.color565(237, 230, 211));
  tft.fillCircle(cx + 4, cy + 2, 7, tft.color565(237, 230, 211));
  tft.fillRect(cx - 4, cy + 4, 14, 6, tft.color565(237, 230, 211));
  displayFontSetBitmap(tft, &ui::font::SansBold8);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(0xFFFF, panel);
  tft.drawString("HAVA", kWeatherX + 40 + (ui::kSwitchW - 40) / 2, cy + 1);
  tft.setTextDatum(textdatum_t::top_left);
}

int infoKey() {
  int key = 0;
  if (services::sht30::valid()) {
    key = static_cast<int>(lroundf(services::sht30::temperatureC() * 10.0f)) * 1000 +
          static_cast<int>(lroundf(services::sht30::humidity()));
  }
  services::weather::Forecast f;
  if (services::weather::snapshot(&f, nullptr) && f.now.valid) key += f.now.temp_c * 7 + f.humidity * 131;
  return key;
}

}  // namespace

void clockDisplayEnter() {
  s_last_minute = -1;
  s_last_yday = -1;
  s_last_info_key = -99999;
  tft.fillScreen(kBackground);
  drawButtons();
}

int clockDisplayHit(int x, int y) {
  if (hitBox(x, y, kWaterX, kBtnY, kWaterBtnW, kWaterBtnH)) return 1;
  if (hitBox(x, y, kWeatherX, kBtnY, kSwitchW, kSwitchH)) return 2;
  return 0;
}

void clockDisplayTick() {
  tm t;
  if (!services::clock::now(&t)) {
    return;
  }
  const int key = infoKey();
  if (key != s_last_info_key) {
    s_last_info_key = key;
    drawInfoLine();
  }
  if (t.tm_min == s_last_minute && t.tm_yday == s_last_yday) {
    return;
  }
  s_last_minute = t.tm_min;
  s_last_yday = t.tm_yday;

  char time_text[8];
  snprintf(time_text, sizeof(time_text), "%02d:%02d", t.tm_hour, t.tm_min);
  char date_text[40];
  if (config::kClockTurkishDate) {
    snprintf(date_text, sizeof(date_text), "%d %s %s", t.tm_mday, kMonthsTr[t.tm_mon % 12],
             kWeekdaysTr[t.tm_wday % 7]);
  } else {
    snprintf(date_text, sizeof(date_text), "%s %02d %s", kWeekdaysEn[t.tm_wday % 7], t.tm_mday,
             kMonthsEn[t.tm_mon % 12]);
  }

  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(&fonts::Font7);  // 7-segment digits
  tft.setTextSize(2.4f);
  tft.setTextPadding(440);
  tft.setTextColor(kTimeColor, kBackground);
  tft.drawString(time_text, kCenterX, kTimeY);

  displayFontSetBitmap(tft, &ui::font::SansBold18);
  tft.setTextPadding(460);
  tft.setTextColor(dimColor(), kBackground);
  tft.drawString(date_text, kCenterX, kDateY);

  tft.setTextPadding(0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace ui
