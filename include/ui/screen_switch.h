#pragma once

namespace ui {

/**
 * Screen switch button, bottom right on both the radar and the forecast screen
 * (radar -> "HAVA", forecast -> "RADAR"). Same box on both so a second tap
 * returns to where you were.
 */
constexpr int kSwitchW = 84;
constexpr int kSwitchH = 42;
constexpr int kSwitchX = 480 - 4 - kSwitchW;
constexpr int kSwitchY = 320 - 4 - kSwitchH;

inline bool switchButtonHit(int x, int y) {
  constexpr int m = 4;  // a little slack for the resistive panel
  return x >= kSwitchX - m && x < kSwitchX + kSwitchW + m && y >= kSwitchY - m &&
         y < kSwitchY + kSwitchH + m;
}

}  // namespace ui
