#pragma once

namespace ui::deck {

/**
 * Stream deck screen (5" board): a grid of keys that send media keys and F13-F21
 * shortcuts to the computer over the USB cable (USB keyboard). Opened from the DECK
 * button on the radar; the RADAR button, or a while without touches, goes back.
 */
void enter();

/** Called at the redraw cadence: releases the pressed-key highlight, updates the USB
 *  status. Returns false when the screen has been idle long enough to close. */
bool tick();

/** Tap result. */
enum class Tap { None, Key, Back };
Tap tap(int x, int y);

}  // namespace ui::deck
