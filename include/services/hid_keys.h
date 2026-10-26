#pragma once

#include <cstdint>

namespace services::hid {

/**
 * The 5" board as a USB keyboard / media keys (stream deck screen).
 *
 * Only in the console's keyboard mode (hardware/console.h): the native USB port then runs
 * TinyUSB and the board is one composite device, a serial port plus a keyboard.
 * console::begin() calls begin() before it starts USB; in setup mode ready() stays false.
 */
void begin();

/** True once the PC has configured the USB device (cable in, not suspended). */
bool ready();

/** Modifier bits for sendCombo(). */
constexpr uint8_t kCtrl = 0x01;
constexpr uint8_t kShift = 0x02;
constexpr uint8_t kAlt = 0x04;
constexpr uint8_t kGui = 0x08;  // Windows / Cmd

/** Press and release one key with modifiers. key = Arduino keyboard code
 *  (KEY_F13..KEY_F24, 'a'..'z', ...). */
void sendCombo(uint8_t modifiers, uint8_t key);

/** Media keys (HID consumer page). */
enum class Media : uint16_t {
  VolumeUp,
  VolumeDown,
  Mute,
  PlayPause,
  Next,
  Previous,
};
void sendMedia(Media m);

}  // namespace services::hid
