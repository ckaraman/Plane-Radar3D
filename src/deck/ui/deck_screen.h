#pragma once

namespace ui::deck {

/**
 * Stream deck screen (5" board): 12 keys set up on the setup page (picture, label,
 * media key or keyboard shortcut) and a round volume knob. Turning the knob with a finger
 * sends volume up / down, a tap on it mutes / unmutes. The keys and the knob go to the
 * computer as a USB keyboard (console keyboard mode). Opened from the deck button on the
 * radar; only the RADAR button goes back.
 */
void enter();

/** Called at the redraw cadence: key highlight, USB status, a new key set from the page.
 *  Always true: the deck closes only from its RADAR button. */
bool tick();

/** Raw touch while the deck is up, every poll: fingers = 0 when none. The deck handles
 *  presses, drags on the knob and releases itself. Back = RADAR released. */
enum class Touch { None, Back };
Touch touch(int fingers, int x, int y);

}  // namespace ui::deck
