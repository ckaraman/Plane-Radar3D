#pragma once

namespace ui {

/**
 * On-screen settings (opened by holding a finger on the screen for
 * config::kTouchLongPressMs). Blocks until "Kapat" is tapped or 30 s pass
 * without a touch; keeps the USB setup channel alive meanwhile.
 * Without a saved touch calibration it runs the calibration first, because
 * the menu buttons need correct touch coordinates.
 */
void settingsMenuRun();

}  // namespace ui
