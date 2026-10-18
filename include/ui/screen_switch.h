#pragma once

namespace ui {

/**
 * Screen buttons, bottom right on the radar, forecast and water screens.
 *
 *  - switch button (kSwitch*): radar -> "HAVA", forecast -> "RADAR", water -> "RADAR"
 *  - water button (kWaterBtn*): a small droplet avatar left of the switch button on
 *    the radar and forecast screens (top right on the clock): jumps to water tracking
 */
constexpr int kSwitchW = 100;
constexpr int kSwitchH = 42;
constexpr int kSwitchX = 480 - 4 - kSwitchW;
constexpr int kSwitchY = 320 - 4 - kSwitchH;

constexpr int kWaterBtnW = 50;
constexpr int kWaterBtnH = kSwitchH;
constexpr int kWaterBtnX = kSwitchX - 6 - kWaterBtnW;
constexpr int kWaterBtnY = kSwitchY;

inline bool hitBox(int x, int y, int bx, int by, int bw, int bh) {
  constexpr int m = 4;  // a little slack for the resistive panel
  return x >= bx - m && x < bx + bw + m && y >= by - m && y < by + bh + m;
}

inline bool switchButtonHit(int x, int y) {
  return hitBox(x, y, kSwitchX, kSwitchY, kSwitchW, kSwitchH);
}

inline bool waterButtonHit(int x, int y) {
  return hitBox(x, y, kWaterBtnX, kWaterBtnY, kWaterBtnW, kWaterBtnH);
}

/**
 * The water button: a rounded box with a tiny droplet avatar (eyes + smile).
 * Works on any LovyanGFX target; colours are whatever the target uses
 * (palette indices on the 4-bit sprite, RGB565 on the panel).
 */
template <typename Gfx, typename Color>
void drawWaterButton(Gfx& g, int x, int y, int w, int h, Color panel, Color edge, Color water,
                     Color white, Color dark) {
  g.fillRoundRect(x, y, w, h, 8, panel);
  g.drawRoundRect(x, y, w, h, 8, edge);
  const int cx = x + w / 2, cy = y + h / 2 + 5;
  constexpr int r = 11;
  g.fillCircle(cx, cy, r, water);
  g.fillTriangle(cx, cy - 2 * r - 2, cx - r + 1, cy - 5, cx + r - 1, cy - 5, water);
  // eyes
  g.fillEllipse(cx - 4, cy - 2, 2, 3, white);
  g.fillEllipse(cx + 4, cy - 2, 2, 3, white);
  g.fillCircle(cx - 4, cy - 1, 1, dark);
  g.fillCircle(cx + 4, cy - 1, 1, dark);
  // smile
  g.drawFastHLine(cx - 2, cy + 5, 5, dark);
  g.drawPixel(cx - 3, cy + 4, dark);
  g.drawPixel(cx + 3, cy + 4, dark);
}

}  // namespace ui
