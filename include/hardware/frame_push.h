#pragma once

#if defined(BOARD_WS5) && !defined(RADAR3D_HOST_TEST)
#include <Arduino.h>
#include "config.h"
#include "hardware/display.h"
#endif

/**
 * Send a full-screen frame sprite to the panel.
 *
 * On the Waveshare 5" board the RGB panel is refreshed continuously by DMA straight
 * out of the PSRAM frame buffer, and the CPU copying a whole frame into that buffer at
 * full speed competes with it: the panel then misses data and the picture drops out
 * for a moment. So the copy goes in horizontal bands with a short pause after each,
 * which keeps the CPU's share of the PSRAM bandwidth down. Elsewhere: one push.
 */
template <typename Sprite>
inline void pushFrame(Sprite& spr) {
#if defined(BOARD_WS5) && !defined(RADAR3D_HOST_TEST)
  constexpr int kBand = 40;  // rows per band (800 x 40 x 2 bytes = 64 KB)
  const int h = spr.height();
  for (int y = 0; y < h; y += kBand) {
    tft.setClipRect(0, y, spr.width(), kBand);
    spr.pushSprite(0, 0);
    vTaskDelay(1);
  }
  tft.clearClipRect();
#else
  spr.pushSprite(0, 0);
#endif
}
