#pragma once

namespace ui::deck {

/**
 * Stream deck screen (5" board): a grid of keys that send media keys and F13-F21
 * shortcuts to the computer over the USB cable (USB keyboard). Opened from the DECK
 * button on the radar; only the RADAR button goes back (no time-out).
 */
void enter();

/** Called at the redraw cadence: releases the pressed-key highlight, updates the USB
 *  status. Always true: the deck closes only from its RADAR button. */
bool tick();

/** Tap result. */
enum class Tap { None, Key, Back };
Tap tap(int x, int y);

}  // namespace ui::deck
