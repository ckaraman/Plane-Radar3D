#include "services/display_prefs.h"

#include <Preferences.h>

#include "config.h"

namespace services::display_prefs {

namespace {

constexpr char kNs[] = "disp";
Prefs s_prefs;

}  // namespace

void init() {
  Preferences p;
  s_prefs.brightness = config::kDisplayBrightness;
  if (!p.begin(kNs, true)) {
    return;
  }
  s_prefs.flip = p.getBool("flip", false);
  s_prefs.invert = p.getBool("invert", false);
  s_prefs.bgr = p.getBool("bgr", false);
  s_prefs.labels = p.getBool("labels", true);
  s_prefs.exag = p.getUChar("exag", 3);
  if (s_prefs.exag < 1 || s_prefs.exag > 5) s_prefs.exag = 3;
  s_prefs.yaw_deg = p.getUShort("yaw", 20) % 360;
  s_prefs.brightness = p.getUChar("bright", config::kDisplayBrightness);
  if (s_prefs.brightness < 20) s_prefs.brightness = 20;
  p.end();
}

const Prefs& get() { return s_prefs; }

void save(const Prefs& np) {
  s_prefs = np;
  Preferences p;
  if (!p.begin(kNs, false)) {
    return;
  }
  p.putBool("flip", np.flip);
  p.putBool("invert", np.invert);
  p.putBool("bgr", np.bgr);
  p.putBool("labels", np.labels);
  p.putUChar("exag", np.exag);
  p.putUShort("yaw", np.yaw_deg % 360);
  p.putUChar("bright", np.brightness);
  p.end();
}

bool touchCalibration(uint16_t out[8]) {
  Preferences p;
  if (!p.begin(kNs, true)) {
    return false;
  }
  const size_t n = p.getBytes("tcal", out, sizeof(uint16_t) * 8);
  p.end();
  return n == sizeof(uint16_t) * 8;
}

void saveTouchCalibration(const uint16_t cal[8]) {
  Preferences p;
  if (!p.begin(kNs, false)) {
    return;
  }
  p.putBytes("tcal", cal, sizeof(uint16_t) * 8);
  p.end();
}

void clearTouchCalibration() {
  Preferences p;
  if (!p.begin(kNs, false)) {
    return;
  }
  p.remove("tcal");
  p.end();
}

}  // namespace services::display_prefs
