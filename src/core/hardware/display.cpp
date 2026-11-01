#include "hardware/display.h"

#include "hardware/display_font.h"
#if defined(BOARD_WS5)
#include "hardware/ch422g.h"
#endif
#include "services/display_prefs.h"
#include "ui/fonts_tr.h"

LGFX tft;

namespace {

#if defined(BOARD_WS5)
uint8_t rotationFor(bool flip) { return flip ? 2 : 0; }  // RGB panel is landscape-native
#endif

}  // namespace

void displayInit() {
  const auto& p = services::display_prefs::get();
#if defined(BOARD_WS5)
  hw::ch422g::resetPanelAndTouch(config::kTouchPinInt);
#endif
  tft.applyPanelColorOptions(p.invert, p.bgr);
  tft.init();
  tft.setRotation(rotationFor(p.flip));
  tft.setBrightness(p.brightness);
  tft.setTextWrap(false);
  tft.fillScreen(0);
  displayFontInit();
}

void displayApplyPrefs() {
  const auto& p = services::display_prefs::get();
  tft.applyPanelColorOptions(p.invert, p.bgr);
  tft.setRotation(rotationFor(p.flip));  // rewrites MADCTL (red/blue order)
  tft.setBrightness(p.brightness);
}

bool displayHasTouchCalibration() {
#if defined(BOARD_WS5)
  return true;
#endif
  uint16_t cal[8];
  return services::display_prefs::touchCalibration(cal);
}

void displayRunTouchCalibration() {
#if defined(BOARD_WS5)
  return;  // capacitive panel: nothing to calibrate
#endif
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&ui::font::SansBold12);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("Dokunmatik kalibrasyonu", tft.width() / 2, tft.height() / 2 - 24);
  tft.setFont(&ui::font::Sans9);
  tft.drawString("Köşelerde beliren okların ucuna sırayla dokunun", tft.width() / 2,
                 tft.height() / 2 + 12);
  tft.drawString("(kalem ya da tırnak ucu en iyisi)", tft.width() / 2, tft.height() / 2 + 36);
  delay(1500);

  uint16_t cal[8];
  tft.calibrateTouch(cal, TFT_ORANGE, TFT_BLACK, 24);
  services::display_prefs::saveTouchCalibration(cal);
  tft.setTouchCalibrate(cal);

  tft.fillScreen(TFT_BLACK);
  tft.setFont(&ui::font::SansBold12);
  tft.drawString("Kaydedildi", tft.width() / 2, tft.height() / 2);
  tft.setTextDatum(textdatum_t::top_left);
  delay(800);
}
