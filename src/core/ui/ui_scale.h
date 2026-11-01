#pragma once

namespace ui {

/**
 * Layout scale. The screens were designed for the 4" 480x320 panel; on the 5" 800x480
 * board every layout size (margins, boxes, icons, line offsets) is 1.5x, which keeps
 * the same physical size at the higher pixel density -- the fonts are generated 1.5x
 * for that board too (tools/fonts/make_tr_fonts.py). Positions anchored to the
 * right or bottom edge use the real screen size, so the extra width of the 5:3 panel
 * goes to the map, not to stretched widgets.
 */
#if defined(BOARD_WS5)
constexpr int kScaleNum = 3;
constexpr int kScaleDen = 2;
#endif

/** Scale a layout size given in 480x320 pixels (rounded to the nearest pixel). */
constexpr int S(int v) {
  return v >= 0 ? (v * kScaleNum * 2 + kScaleDen) / (kScaleDen * 2)
                : -((-v * kScaleNum * 2 + kScaleDen) / (kScaleDen * 2));
}
constexpr float Sf(float v) { return v * static_cast<float>(kScaleNum) / static_cast<float>(kScaleDen); }

}  // namespace ui
