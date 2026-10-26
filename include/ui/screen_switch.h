#pragma once

#include "config.h"
#include "ui/ui_scale.h"

namespace ui {

/**
 * Screen buttons, bottom right on the radar, forecast and water screens.
 *
 *  - switch button (kSwitch*): radar -> "HAVA", forecast -> "RADAR", water -> "RADAR"
 *  - water button (kWaterBtn*): a small droplet avatar left of the switch button on
 *    the radar and forecast screens (top right on the clock): jumps to water tracking
 */
constexpr int kSwitchW = S(100);
constexpr int kSwitchH = S(42);
constexpr int kSwitchX = config::kDisplayWidth - S(4) - kSwitchW;
constexpr int kSwitchY = config::kDisplayHeight - S(4) - kSwitchH;

constexpr int kWaterBtnW = S(50);
constexpr int kWaterBtnH = kSwitchH;
constexpr int kWaterBtnX = kSwitchX - S(6) - kWaterBtnW;
constexpr int kWaterBtnY = kSwitchY;

inline bool hitBox(int x, int y, int bx, int by, int bw, int bh) {
  constexpr int m = S(4);  // a little slack for the touch panel
  return x >= bx - m && x < bx + bw + m && y >= by - m && y < by + bh + m;
}

inline bool switchButtonHit(int x, int y) {
  return hitBox(x, y, kSwitchX, kSwitchY, kSwitchW, kSwitchH);
}

inline bool waterButtonHit(int x, int y) {
  return hitBox(x, y, kWaterBtnX, kWaterBtnY, kWaterBtnW, kWaterBtnH);
}

#if defined(BOARD_WS5)
// 5" radar screen: no arrow keys (the map is turned by dragging), the water button
// sits bottom left.
constexpr int kRadarWaterX = S(4);
inline bool radarWaterButtonHit(int x, int y) {
  return hitBox(x, y, kRadarWaterX, kWaterBtnY, kWaterBtnW, kWaterBtnH);
}

// Stream deck button, left of HAVA on the radar: opens the deck screen.
constexpr int kDeckBtnW = S(56);
constexpr int kDeckBtnH = kSwitchH;
constexpr int kDeckBtnX = kSwitchX - S(6) - kDeckBtnW;
constexpr int kDeckBtnY = kSwitchY;
inline bool deckButtonHit(int x, int y) {
  return hitBox(x, y, kDeckBtnX, kDeckBtnY, kDeckBtnW, kDeckBtnH);
}

/** The deck button: a rounded box with a 3 x 2 grid of small keys. */
template <typename Gfx, typename Color>
void drawDeckButton(Gfx& g, Color panel, Color edge, Color keys) {
  g.fillRoundRect(kDeckBtnX, kDeckBtnY, kDeckBtnW, kDeckBtnH, S(8), panel);
  g.drawRoundRect(kDeckBtnX, kDeckBtnY, kDeckBtnW, kDeckBtnH, S(8), edge);
  constexpr int kw = S(9), kh = S(8), gap = S(3);
  const int x0 = kDeckBtnX + (kDeckBtnW - (3 * kw + 2 * gap)) / 2;
  const int y0 = kDeckBtnY + (kDeckBtnH - (2 * kh + gap)) / 2;
  for (int r = 0; r < 2; ++r) {
    for (int c = 0; c < 3; ++c) {
      g.fillRoundRect(x0 + c * (kw + gap), y0 + r * (kh + gap), kw, kh, S(2), keys);
    }
  }
}
#endif

/**
 * The water button: a rounded box with a tiny droplet avatar (eyes + smile).
 * Works on any LovyanGFX target; colours are whatever the target uses
 * (palette indices on the 4-bit sprite, RGB565 on the panel).
 */
template <typename Gfx, typename Color>
void drawWaterButton(Gfx& g, int x, int y, int w, int h, Color panel, Color edge, Color water,
                     Color white, Color dark) {
  g.fillRoundRect(x, y, w, h, S(8), panel);
  g.drawRoundRect(x, y, w, h, S(8), edge);
  const int cx = x + w / 2, cy = y + h / 2 + S(5);
  constexpr int r = S(11);
  g.fillCircle(cx, cy, r, water);
  g.fillTriangle(cx, cy - 2 * r - S(2), cx - r + 1, cy - S(5), cx + r - 1, cy - S(5), water);
  // eyes
  g.fillEllipse(cx - S(4), cy - S(2), S(2), S(3), white);
  g.fillEllipse(cx + S(4), cy - S(2), S(2), S(3), white);
  g.fillCircle(cx - S(4), cy - S(1), S(1), dark);
  g.fillCircle(cx + S(4), cy - S(1), S(1), dark);
  // smile
  g.drawFastHLine(cx - S(2), cy + S(5), S(5), dark);
  g.drawPixel(cx - S(3), cy + S(4), dark);
  g.drawPixel(cx + S(3), cy + S(4), dark);
  if (kScaleNum != kScaleDen) {  // bigger panel: a 2 px smile
    g.drawFastHLine(cx - S(2), cy + S(5) + 1, S(5), dark);
  }
}

}  // namespace ui
