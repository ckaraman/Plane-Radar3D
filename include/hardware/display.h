#pragma once

#include "hardware/lgfx_config.hpp"

extern LGFX tft;

/** Init panel, backlight and touch with the saved display prefs. */
void displayInit();
/** Re-apply flip / invert / red-blue order / brightness after the prefs changed. */
void displayApplyPrefs();
bool displayHasTouchCalibration();
/** Full-screen 4-corner touch calibration; saves the result. Blocks until done. */
void displayRunTouchCalibration();
