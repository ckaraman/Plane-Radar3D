#pragma once

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

}  // namespace ui::radar3d
