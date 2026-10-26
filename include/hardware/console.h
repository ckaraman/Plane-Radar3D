#pragma once

#include <Arduino.h>

/**
 * The USB serial port the setup page, the USB bridge and the logs use ("Console"),
 * on the 5" board's one native USB connector, which runs in one of two modes:
 *
 *  - Keyboard (default): TinyUSB, the board is a composite device, a serial port plus a
 *    USB keyboard / media keys (stream deck screen). Arduino's TinyUSB serial port loses
 *    data and stalls on long fast transfers, so this mode is for short lines only.
 *  - Setup: the chip's USB-Serial/JTAG port, as before the stream deck (no keyboard).
 *    Firmware updates and map / deck uploads run in this mode.
 *
 * The mode is chosen at boot from a flag in NVS. "PR:USBSETUP" from the page sets it and
 * restarts; the board returns to keyboard mode by itself after a few minutes without a
 * line from the page, on "PR:USBNORMAL", or on the next start after that.
 */
class ConsoleIO : public Stream {
 public:
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  using Print::write;
};

extern ConsoleIO Console;

namespace console {

enum class Mode { Keyboard, Setup };

/** Pick the mode and start USB (keyboard mode: serial port + HID). First thing in setup(). */
void begin();
Mode mode();
inline bool keyboardMode() { return mode() == Mode::Keyboard; }

/** Reply "PR:USBMODE {...}", save the new mode and restart into it (no return).
 *  Restarting into the mode already running only replies. */
void switchTo(Mode m);

/** A line arrived from the page: postpones the return to keyboard mode. */
void noteActivity();
/** Setup mode: back to keyboard mode after a while without lines (call from loop). */
void tick();

}  // namespace console
