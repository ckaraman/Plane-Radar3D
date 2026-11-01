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
/** A line from the Windows / Mac helper (its volume heartbeat comes every 2 s). */
void noteHost();
/** Setup mode: back to keyboard mode after a while without lines. Keyboard mode: when
 *  the helper's lines stop while it still holds the port, the USB link is reconnected
 *  (a stalled TinyUSB port comes back that way). Call from loop. */
void tick();
/** Times the USB link was reconnected so (since start). */
int usbRecoveries();

/** Keyboard mode: what went wrong on the USB link since clearLink() (stage 10c: what a
 *  stall looks like from the card, sent with the next timing report that gets through). */
struct LinkStats {
  uint32_t txfull_ms = 0;  // longest time everything written was dropped (the PC read nothing)
  uint32_t dropped = 0;    // bytes dropped so
  uint32_t rxgap_ms = 0;   // longest time without a byte from the PC
  uint16_t dtr_off = 0;    // the PC's program let go of the port
  uint16_t unmount = 0;    // USB went away (cable, PC)
  uint16_t suspend = 0;    // the PC suspended the bus
  char before[4][12] = {};  // starts of the PC's last lines when the longest drop began
  // Stage 10e-0, when the longest drop began: TinyUSB's own send buffer free (0: full, the
  // PC took nothing; more: the port was held by another write), bytes from the PC not read
  // yet, the core that wrote. lock_ms: longest wait for the port's lock (250: given up).
  uint16_t fifo = 0, rxwait = 0, lock_ms = 0;
  uint8_t core = 0;
  uint16_t early = 0;  // reconnects after 3 s of drops while the PC still wrote (stage 10e)
};
const LinkStats& link();
void clearLink();
uint32_t droppedBytes();  // since start: a report that grew this did not get through

}  // namespace console
