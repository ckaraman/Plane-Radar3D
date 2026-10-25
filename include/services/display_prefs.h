#pragma once

#include <cstdint>

namespace services::display_prefs {

/**
 * Per-device display and view settings, changed from the on-screen settings
 * menu (long press) and kept in NVS. Panel variants differ in color inversion
 * and red/blue order, so these are switches rather than build options.
 */
struct Prefs {
  bool flip = false;      // rotate the picture 180 degrees
  bool invert = false;    // panel color inversion
  bool bgr = false;       // swap red and blue
  bool labels = true;     // callsign / altitude tags next to aircraft
  uint8_t exag = 2;       // altitude exaggeration (1-5)
  uint16_t yaw_deg = 20;  // camera heading around the center (0-359)
  uint8_t brightness = 255;
  uint8_t pitch_deg = 22;    // camera tilt (10 = flat, 62 = from above)
  bool high_contrast = true; // brighter lines and text for glare / viewing angle
  uint8_t info_style = 0;    // selected aircraft: 0 = two corner panels, 1 = card at its label
  bool places = true;        // landmark symbols on the 5" map (stadium, tower, ...)
};

void init();
const Prefs& get();
void save(const Prefs& p);
/** Change the settings in RAM only (save() later writes them to flash). */
void setNoSave(const Prefs& p);

/** Touch calibration from LovyanGFX calibrateTouch (8 values). */
bool touchCalibration(uint16_t out[8]);
void saveTouchCalibration(const uint16_t cal[8]);
void clearTouchCalibration();

}  // namespace services::display_prefs
