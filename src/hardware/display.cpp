#include "hardware/display.h"

#include "hardware/display_font.h"
#include "services/display_prefs.h"

LGFX tft;

namespace {

uint8_t rotationFor(bool flip) { return flip ? 3 : 1; }  // landscape, 480x320

}  // namespace

void displayInit() {
  const auto& p = services::display_prefs::get();
  tft.applyPanelColorOptions(p.invert, p.bgr);
  tft.init();
  tft.setRotation(rotationFor(p.flip));
  tft.setBrightness(p.brightness);
  tft.setTextWrap(false);
  tft.fillScreen(0);
  uint16_t cal[8];
  if (services::display_prefs::touchCalibration(cal)) {
    tft.setTouchCalibrate(cal);
  }
  displayFontInit();
}

void displayApplyPrefs() {
  const auto& p = services::display_prefs::get();
  tft.applyPanelColorOptions(p.invert, p.bgr);
  tft.setRotation(rotationFor(p.flip));  // rewrites MADCTL (red/blue order)
  tft.setBrightness(p.brightness);
}

bool displayHasTouchCalibration() {
  uint16_t cal[8];
  return services::display_prefs::touchCalibration(cal);
}

void displayRunTouchCalibration() {
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("Dokunmatik kalibrasyonu", tft.width() / 2, tft.height() / 2 - 24);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.drawString("Koselerde beliren oklarin ucuna sirayla dokunun", tft.width() / 2,
                 tft.height() / 2 + 12);
  tft.drawString("(kalem ya da tirnak ucu en iyisi)", tft.width() / 2, tft.height() / 2 + 36);
  delay(1500);

  uint16_t cal[8];
  tft.calibrateTouch(cal, TFT_ORANGE, TFT_BLACK, 24);
  services::display_prefs::saveTouchCalibration(cal);
  tft.setTouchCalibrate(cal);

  tft.fillScreen(TFT_BLACK);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.drawString("Kaydedildi", tft.width() / 2, tft.height() / 2);
  tft.setTextDatum(textdatum_t::top_left);
  delay(800);
}
