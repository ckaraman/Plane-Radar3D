#pragma once

namespace ui {

/** Clear the panel and force the next tick to draw. Call when switching to the clock. */
void clockDisplayEnter();

/** Draw HH:MM + date. Cheap to call often: only redraws when the minute changes. */
void clockDisplayTick();

/** Tap on the clock: 1 = water button, 2 = forecast button, 0 = elsewhere. */
int clockDisplayHit(int x, int y);

}  // namespace ui
