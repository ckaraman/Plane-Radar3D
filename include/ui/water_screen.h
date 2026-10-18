#pragma once

namespace ui::water {

/**
 * Water tracking: the third screen (after radar and forecast) plus an hourly
 * reminder overlay with a water-droplet avatar. Drawn into the radar's frame
 * sprite with its own palette.
 */

// --- main water screen ---
void enter();
/** Redraws when something changed (cheap to call often). */
void tick();
/** Tap on the water screen (the screen switch button is handled by main). */
bool tap(int x, int y);

// --- overlay (hourly question, "+ SU" amount picker, goal setting) ---
/** True at the start of a reminder hour (09:00-18:00) that has not been asked yet
 *  and in which nothing was logged. */
bool reminderDue();
/** Flash the backlight and ask "Su içtin mi?". */
void openReminder();
bool overlayActive();
/** Animate / time out the overlay. Returns true while it is still open. */
bool overlayTick();
void overlayTap(int x, int y);

}  // namespace ui::water
