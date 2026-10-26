#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace services::deck {

/**
 * The stream deck keys set up on the setup page ("Kontrol tuşları"): label, action and
 * an optional picture per key. Kept in flash (the data partition, well past the map) and
 * in PSRAM while running. Without a stored set the built-in default keys are used.
 *
 * Blob (little endian), as sent by the page or the Windows helper:
 *   version 1: 'D' 'K' 1 slot_count(kSlots) icon_px volume_steps (0 = 16)
 *              pictures icon_px * icon_px, drawn small above the label
 *   version 2: 'D' 'K' 2 slot_count icon_w icon_h volume_steps 0
 *              pictures icon_w * icon_h = the whole key (keyW() x keyH()), the label on top
 *   then slot_count * Slot (28 bytes, below)
 *   then for each slot with has_icon, in slot order: the picture as RGB565 pixels;
 *   kTransparent (magenta) pixels let the key colour show through.
 */
constexpr uint8_t kVersion = 2;  // newest understood (1 still accepted)
constexpr int kSlots = 12;
constexpr size_t kLabelBytes = 24;  // UTF-8, zero-terminated / padded
constexpr uint16_t kTransparent = 0xF81F;

/** Volume keys from silent to full on the computer: one full knob turn. macOS 16,
 *  Windows 50 (2 % per key); chosen on the setup page, 16 by default. */
int volumeSteps();

/** Icon edge in pixels for version 1 sets: about a third of a key at 1.5x (5") or 1.875x (5B). */
int iconPx();

/** Key size on the deck screen (ui/deck_screen.cpp lays the keys out with these). */
constexpr int keyW() { return (470 * config::kDisplayWidth / 800 - 3 * (10 * config::kDisplayWidth / 800)) / 4; }
constexpr int keyH() { return 120 * config::kDisplayWidth / 800; }

/** True when the pictures of the active set fill the whole key (version 2). */
bool fullKeyIcons();

enum class Kind : uint8_t {
  Empty = 0,
  Media = 1,  // code: services::hid::Media
  Key = 2,    // code: Arduino key code (KEY_F13..., 'a'..'z', '0'..'9'), mods: hid::kCtrl...
  Host = 3,   // carried out by the Windows helper (services/host_link.h): sends PR:KEY
};

struct Slot {
  Kind kind;
  uint8_t code;
  uint8_t mods;
  uint8_t has_icon;
  char label[kLabelBytes];
};
static_assert(sizeof(Slot) == 28, "slot layout is part of the page protocol");

/** Load the stored set (or the defaults). Call once at boot, after PSRAM is up. */
void init();

const Slot& slot(int i);
/** The slot's picture (iconPx() square, or keyW() x keyH() when fullKeyIcons()), or nullptr. */
const uint16_t* icon(int i);
/** Changes whenever the set is replaced (redraw the deck). */
uint32_t generation();

/** Upload from the page: begin, chunks in order, commit (validates, writes flash). */
bool beginStaging(size_t length, uint32_t crc32);
bool stageChunk(const uint8_t* bytes, size_t n);
bool commit();
void abortStaging();
/** Back to the built-in keys (flash and RAM). */
void clear();
/** True while a set from the page is in use. */
bool custom();

}  // namespace services::deck
