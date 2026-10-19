#pragma once

#include "hardware/display.h"  // LGFX_Sprite

namespace ui::radar3d {

/**
 * Perspective ("fake 3D") radar for the 480x320 panel.
 *
 * Everything is drawn into one 4-bit (16 color) full-screen sprite, 76.8 KB,
 * then pushed to the panel in one go: no flicker, and it fits the ESP32's RAM
 * without PSRAM. Allocate it early (before Wi-Fi) so the block is available.
 */
bool init();

/** Render one frame (dead-reckoned aircraft) and push it to the panel. */
void drawFrame();

/** Touch tap in screen coordinates. Rotate buttons, aircraft selection,
 *  range change (tap the top bar). Returns true when something changed. */
bool handleTap(int x, int y);

/** The shared full-screen frame sprite (nullptr if it could not be allocated).
 *  The forecast screen draws into it with its own palette. */
LGFX_Sprite* frame();

/** Free the 76.8 KB frame for a moment (a TLS handshake needs the heap); the panel
 *  keeps showing the last picture. reacquireFrame() takes it back. */
void releaseFrame();
bool reacquireFrame();
/** Bytes the frame needs as one block. */
constexpr unsigned kFrameBytes = 480 * 320 / 2;

}  // namespace ui::radar3d
