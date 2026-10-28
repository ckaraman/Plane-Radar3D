#include "ui/settings_menu.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/display_prefs.h"
#include "services/sht30.h"
#include "services/wifi_setup.h"
#include "ui/radar_range.h"
#include "ui/fonts_tr.h"
#include "ui/ui_scale.h"

namespace ui {

namespace {

enum Item {
  kCalibrate,
  kInvert,
  kBgr,
  kFlip,
  kLabels,
  kExag,
  kRange,
  kBrightness,
  kContrast,
  kInfo,
  kPlaces,
  kItemCount
};

// Buttons shown, in order. The 5" board has a factory-calibrated capacitive IPS panel
// (no calibration, inversion or red/blue options) and an on/off backlight.
#if defined(BOARD_WS5)
constexpr Item kVisible[] = {kFlip, kLabels, kExag, kRange, kContrast, kInfo, kPlaces};
#else
constexpr Item kVisible[] = {kCalibrate, kInvert, kBgr,        kFlip,     kLabels,
                             kExag,      kRange,  kBrightness, kContrast, kInfo};
#endif
constexpr int kVisibleCount = sizeof(kVisible) / sizeof(kVisible[0]);

using ui::S;
constexpr int kCols = 2;
constexpr int kRows = (kVisibleCount + kCols - 1) / kCols;
constexpr int kTitleH = S(44);
constexpr int kPad = S(8);
constexpr int kBtnW = (config::kDisplayWidth - kPad * 3) / 2;
constexpr int kBtnH = (config::kDisplayHeight - kTitleH - kPad * (kRows + 1)) / kRows;
constexpr int kCloseW = S(96);
constexpr unsigned long kIdleCloseMs = 30000;

uint16_t c(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

void itemText(int item, char* title, char* value, size_t n) {
  const auto& p = services::display_prefs::get();
  value[0] = '\0';
  switch (item) {
    case kCalibrate:
      snprintf(title, n, "Dokunmatik");
      snprintf(value, n, "Kalibre et");
      break;
    case kInvert:
      snprintf(title, n, "Renkleri ters çevir");
      snprintf(value, n, p.invert ? "Açık" : "Kapalı");
      break;
    case kBgr:
      snprintf(title, n, "Kırmızı / Mavi");
      snprintf(value, n, p.bgr ? "Yer değişti" : "Normal");
      break;
    case kFlip:
      snprintf(title, n, "Ekranı 180° çevir");
      snprintf(value, n, p.flip ? "Açık" : "Kapalı");
      break;
    case kLabels:
      snprintf(title, n, "Uçak etiketleri");
      snprintf(value, n, p.labels ? "Açık" : "Kapalı");
      break;
    case kExag:
      snprintf(title, n, "İrtifa abartma");
      snprintf(value, n, "%ux", p.exag);
      break;
    case kRange: {
      snprintf(title, n, "Menzil");
      ui::radar::formatRing3Label(value, n, ui::radar::rangeCurrent().outer_km,
                                  ui::radar::useMiles());
      break;
    }
    case kContrast:
      snprintf(title, n, "Renkler");
      snprintf(value, n, p.high_contrast ? "Yüksek kontrast" : "Normal");
      break;
    case kInfo:
      snprintf(title, n, "Uçak bilgisi");
      snprintf(value, n, p.info_style == 1 ? "Etikette" : "Köşelerde");
      break;
    case kPlaces:
      snprintf(title, n, "Önemli yerler");
      snprintf(value, n, p.places ? "Göster" : "Gizle");
      break;
    case kBrightness:
      snprintf(title, n, "Parlaklık");
      snprintf(value, n, "%%%d", static_cast<int>(lroundf(p.brightness * 100.0f / 255.0f)));
      break;
  }
}

/** Slot = position on screen (index into kVisible). */
void buttonRect(int slot, int* x, int* y) {
  const int col = slot % kCols;
  const int row = slot / kCols;
  *x = kPad + col * (kBtnW + kPad);
  *y = kTitleH + kPad + row * (kBtnH + kPad);
}

void drawButton(int slot, bool pressed) {
  int x, y;
  buttonRect(slot, &x, &y);
  const uint16_t bg = pressed ? c(35, 72, 92) : c(11, 22, 32);
  tft.fillRoundRect(x, y, kBtnW, kBtnH, S(8), bg);
  tft.drawRoundRect(x, y, kBtnW, kBtnH, S(8), c(35, 72, 92));
  char title[32], value[32];
  itemText(kVisible[slot], title, value, sizeof(title));
  // One row per button: name on the left, current value on the right.
  displayFontSetBitmap(tft, &ui::font::Sans9);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(c(175, 188, 198), bg);
  tft.drawString(title, x + S(12), y + kBtnH / 2);
  displayFontSetBitmap(tft, &ui::font::SansBold9);
  tft.setTextDatum(textdatum_t::middle_right);
  tft.setTextColor(c(110, 215, 255), bg);
  tft.drawString(value, x + kBtnW - S(12), y + kBtnH / 2);
}

void drawAll() {
  tft.fillScreen(c(5, 8, 13));
  tft.fillRect(0, 0, config::kDisplayWidth, kTitleH, c(11, 22, 32));
  displayFontSetBitmap(tft, &ui::font::SansBold12);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(c(230, 237, 242), c(11, 22, 32));
  tft.drawString("Ayarlar", S(12), kTitleH / 2);

  tft.setFont(&ui::font::Sans8);
  tft.setTextSize(1);
  tft.setTextColor(c(125, 139, 150), c(11, 22, 32));
  char info[64];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(info, sizeof(info), "v%s  %s", config::kFirmwareVersion,
             WiFi.localIP().toString().c_str());
  } else {
    snprintf(info, sizeof(info), "v%s  Wi-Fi yok", config::kFirmwareVersion);
  }
  tft.drawString(info, S(112), kTitleH / 2 - S(7));
  if (services::sht30::valid()) {
    snprintf(info, sizeof(info), "SHT30 %.1f°C  %%%d", services::sht30::temperatureC(),
             static_cast<int>(lroundf(services::sht30::humidity())));
  } else {
    snprintf(info, sizeof(info), "SHT30 okunamıyor");
  }
  tft.drawString(info, S(112), kTitleH / 2 + S(8));

  const int cx = config::kDisplayWidth - kCloseW - kPad;
  tft.fillRoundRect(cx, S(6), kCloseW, kTitleH - S(12), S(6), c(255, 159, 67));
  displayFontSetBitmap(tft, &ui::font::SansBold9);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(c(5, 8, 13), c(255, 159, 67));
  tft.drawString("Kapat", cx + kCloseW / 2, kTitleH / 2);

  for (int i = 0; i < kVisibleCount; ++i) {
    drawButton(i, false);
  }
  tft.setTextDatum(textdatum_t::top_left);
}

/** Wait for a full tap (press + release). Returns false on the idle timeout. */
bool waitTap(int* x, int* y) {
  const unsigned long start = millis();
  lgfx::touch_point_t tp;
  // Let go of the long press that opened the menu (or the previous tap).
  while (tft.getTouch(&tp) > 0) {
    wifiLoop();
    delay(15);
  }
  while (millis() - start < kIdleCloseMs) {
    wifiLoop();
    if (tft.getTouch(&tp) > 0) {
      *x = tp.x;
      *y = tp.y;
      while (tft.getTouch(&tp) > 0) {
        delay(15);
      }
      return true;
    }
    delay(15);
  }
  return false;
}

/** Slot under the point, or -1. */
int itemAt(int x, int y) {
  for (int i = 0; i < kVisibleCount; ++i) {
    int bx, by;
    buttonRect(i, &bx, &by);
    if (x >= bx && x < bx + kBtnW && y >= by && y < by + kBtnH) return i;
  }
  return -1;
}

}  // namespace

void settingsMenuRun() {
  if (!displayHasTouchCalibration()) {
    displayRunTouchCalibration();
  }
  drawAll();
  for (;;) {
    int x, y;
    if (!waitTap(&x, &y)) break;
    if (y < kTitleH && x >= config::kDisplayWidth - kCloseW - kPad * 2) break;
    const int slot = itemAt(x, y);
    if (slot < 0) continue;
    const Item item = kVisible[slot];
    drawButton(slot, true);
    auto p = services::display_prefs::get();
    bool redraw_all = false;
    switch (item) {
      case kCalibrate:
        displayRunTouchCalibration();
        redraw_all = true;
        break;
      case kInvert:
        p.invert = !p.invert;
        break;
      case kBgr:
        p.bgr = !p.bgr;
        break;
      case kFlip:
        p.flip = !p.flip;
        break;
      case kLabels:
        p.labels = !p.labels;
        break;
      case kExag:
        p.exag = static_cast<uint8_t>(p.exag % 5 + 1);
        break;
      case kRange:
        ui::radar::rangeNext();
        break;
      case kContrast:
        p.high_contrast = !p.high_contrast;
        break;
      case kInfo:
        p.info_style = p.info_style == 1 ? 0 : 1;
        break;
      case kPlaces:
        p.places = !p.places;
        break;
      case kBrightness:
        p.brightness = p.brightness >= 250 ? 60 : static_cast<uint8_t>(std::min(255, p.brightness + 65));
        break;
    }
    if (item != kCalibrate && item != kRange) {
      services::display_prefs::save(p);
    }
    if (item == kInvert || item == kBgr || item == kFlip || item == kBrightness) {
      displayApplyPrefs();
      redraw_all = item != kBrightness;
    }
    if (redraw_all) {
      drawAll();
    } else {
      drawButton(slot, false);
    }
  }
}

}  // namespace ui
