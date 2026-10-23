#pragma once

#if defined(BOARD_WS5) && !defined(RADAR3D_HOST_TEST)
#include "hardware/display.h"
#endif

/**
 * Send a full-screen frame sprite to the panel.
 *
 * On the Waveshare 5" board the frame goes into the hidden one of two frame buffers
 * and is shown from the next refresh on, so a moving picture (camera following an
 * aircraft) never tears. Elsewhere: a plain push.
 */
template <typename Sprite>
inline void pushFrame(Sprite& spr) {
#if defined(BOARD_WS5) && !defined(RADAR3D_HOST_TEST)
  tft.beginHiddenFrame();
  spr.pushSprite(0, 0);
  tft.presentHiddenFrame();
#else
  spr.pushSprite(0, 0);
#endif
}
