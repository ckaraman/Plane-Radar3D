#pragma once

namespace ui {

/**
 * Forecast screen (opened with the switch button on the radar): outdoor now,
 * indoor SHT30 reading and the 5-day forecast. Drawn into the radar's frame
 * sprite with its own 16-colour palette.
 */
void weatherDisplayEnter();

/** Cheap to call often: redraws only when something on the screen changed. */
void weatherDisplayTick();

/** Tap on the forecast screen: a day column shows that day in the big card
 *  (today = current conditions). Returns true when the screen changed. */
bool weatherDisplayTap(int x, int y);

}  // namespace ui
