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

namespace ui {

namespace {

constexpr uint16_t kBackground = 0x0000;
constexpr uint16_t kTimeColor = 0xFFFF;
constexpr int kCenterX = config::kDisplayWidth / 2;
constexpr int kTimeY = 120;
constexpr int kDateY = 210;
constexpr int kInfoY = 278;

int s_last_minute = -1;
int s_last_yday = -1;
int s_last_info_key = -99999;

// ASCII only: the bitmap fonts have no Turkish letters.
const char* const kWeekdaysEn[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* const kMonthsEn[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const char* const kWeekdaysTr[] = {"PAZAR", "PAZARTESI", "SALI", "CARSAMBA",
                                   "PERSEMBE", "CUMA", "CUMARTESI"};
const char* const kMonthsTr[] = {"OCAK",   "SUBAT", "MART",  "NISAN", "MAYIS", "HAZIRAN",
                                 "TEMMUZ", "AGUSTOS", "EYLUL", "EKIM", "KASIM", "ARALIK"};

uint16_t dimColor() { return tft.color565(125, 139, 150); }
uint16_t softColor() { return tft.color565(190, 200, 215); }

/** "23.4" + degree ring + "C", left aligned at x; returns the x after "C".
 *  With draw == false it only measures. */
int drawDegrees(const char* number, int x, int y, uint16_t color, bool draw) {
  tft.setTextColor(color, kBackground);
  tft.setTextDatum(textdatum_t::middle_left);
  if (draw) tft.drawString(number, x, y);
  x += tft.textWidth(number) + 3;
  if (draw) tft.drawCircle(x + 3, y - 7, 3, color);
  x += 9;
  if (draw) tft.drawString("C", x, y);
  return x + tft.textWidth("C");
}

int drawText(const char* text, int x, int y, uint16_t color, bool draw) {
  tft.setTextColor(color, kBackground);
  tft.setTextDatum(textdatum_t::middle_left);
  if (draw) tft.drawString(text, x, y);
  return x + tft.textWidth(text);
}

/** Bottom line: indoor (SHT30) and outdoor (Open-Meteo) temperature, centred. */
void drawInfoLine() {
  tft.fillRect(0, kInfoY - 22, config::kDisplayWidth, 44, kBackground);
  displayFontSetBitmap(tft, &fonts::FreeSansBold12pt7b);

  const bool indoor = services::sht30::valid();
  services::weather::Forecast f;
  const bool outdoor = services::weather::enabled() &&
                       services::weather::snapshot(&f, nullptr) && f.now.valid;
  if (!indoor && !outdoor) return;

  char t_in[12], h_in[8], t_out[8];
  snprintf(t_in, sizeof(t_in), "%.1f", services::sht30::temperatureC());
  snprintf(h_in, sizeof(h_in), "%%%d", static_cast<int>(lroundf(services::sht30::humidity())));
  if (outdoor) snprintf(t_out, sizeof(t_out), "%d", f.now.temp_c);

  auto line = [&](int x, bool draw) {
    if (indoor) {
      x = drawText("IC", x, kInfoY, dimColor(), draw) + 10;
      x = drawDegrees(t_in, x, kInfoY, softColor(), draw) + 16;
      x = drawText(h_in, x, kInfoY, softColor(), draw);
      if (outdoor) x += 40;
    }
    if (outdoor) {
      x = drawText("DIS", x, kInfoY, dimColor(), draw) + 10;
      x = drawDegrees(t_out, x, kInfoY, softColor(), draw);
    }
    return x;
  };
  const int width = line(0, false);
  line((config::kDisplayWidth - width) / 2, true);
}

int infoKey() {
  int key = 0;
  if (services::sht30::valid()) {
    key = static_cast<int>(lroundf(services::sht30::temperatureC() * 10.0f)) * 1000 +
          static_cast<int>(lroundf(services::sht30::humidity()));
  }
  services::weather::Forecast f;
  if (services::weather::snapshot(&f, nullptr) && f.now.valid) key += f.now.temp_c * 7;
  return key;
}

}  // namespace

void clockDisplayEnter() {
  s_last_minute = -1;
  s_last_yday = -1;
  s_last_info_key = -99999;
  tft.fillScreen(kBackground);
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

  displayFontSetBitmap(tft, &fonts::FreeSansBold18pt7b);
  tft.setTextPadding(460);
  tft.setTextColor(dimColor(), kBackground);
  tft.drawString(date_text, kCenterX, kDateY);

  tft.setTextPadding(0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace ui
