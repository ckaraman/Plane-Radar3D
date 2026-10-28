#include "services/hid_keys.h"

#include <Arduino.h>

#if defined(BOARD_WS5) && !ARDUINO_USB_MODE
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDKeyboard.h>

namespace {

// Global objects: their constructors register the HID interface before the USB stack
// starts (console::begin() starts it in keyboard mode; in setup mode it never starts).
USBHIDKeyboard s_keyboard;
USBHIDConsumerControl s_media;

volatile bool s_mounted = false;

void onUsbEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT: s_mounted = true; break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT: s_mounted = false; break;
    default: break;
  }
}

}  // namespace

namespace services::hid {

void begin() {
  // Called by console::begin() in keyboard mode, before it starts USB.
  USB.onEvent(onUsbEvent);
  s_keyboard.begin();
  s_media.begin();
}

bool ready() { return s_mounted; }

void sendCombo(uint8_t modifiers, uint8_t key) {
  if (modifiers & kCtrl) s_keyboard.press(KEY_LEFT_CTRL);
  if (modifiers & kShift) s_keyboard.press(KEY_LEFT_SHIFT);
  if (modifiers & kAlt) s_keyboard.press(KEY_LEFT_ALT);
  if (modifiers & kGui) s_keyboard.press(KEY_LEFT_GUI);
  s_keyboard.press(key);
  delay(15);  // some apps miss a press and release in the same millisecond
  s_keyboard.releaseAll();
}

void sendMedia(Media m) {
  uint16_t code = 0;
  switch (m) {
    case Media::VolumeUp: code = CONSUMER_CONTROL_VOLUME_INCREMENT; break;
    case Media::VolumeDown: code = CONSUMER_CONTROL_VOLUME_DECREMENT; break;
    case Media::Mute: code = CONSUMER_CONTROL_MUTE; break;
    case Media::PlayPause: code = CONSUMER_CONTROL_PLAY_PAUSE; break;
    case Media::Next: code = CONSUMER_CONTROL_SCAN_NEXT; break;
    case Media::Previous: code = CONSUMER_CONTROL_SCAN_PREVIOUS; break;
  }
  s_media.press(code);
  delay(15);
  s_media.release();
}

}  // namespace services::hid

#else  // 4" board or USB-Serial/JTAG mode: no keyboard

namespace services::hid {
void begin() {}
bool ready() { return false; }
void sendCombo(uint8_t, uint8_t) {}
void sendMedia(Media) {}
}  // namespace services::hid

#endif
