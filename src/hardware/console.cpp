#include "hardware/console.h"

#include <HWCDC.h>
#include <Preferences.h>
#include <USB.h>
#include <USBCDC.h>

#include <algorithm>
#include <esp_private/periph_ctrl.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "services/hid_keys.h"
#include "services/serial_ota.h"

extern "C" bool tud_disconnect(void);  // TinyUSB (tusb.h)

ConsoleIO Console;

namespace console {

namespace {

/** Setup mode returns to keyboard mode after this long without a line from the page. */
constexpr unsigned long kSetupIdleMs = 3UL * 60UL * 1000UL;
constexpr size_t kRxBuffer = 4096;  // the page and the bridge send lines in bursts

// TinyUSB mode: own PID so the PC does not mix it up with the USB-Serial/JTAG port
// (0x303a:0x1001); both modes keep Espressif's VID, which the setup page looks for.
constexpr uint16_t kKeyboardPid = 0x8150;

Mode s_mode = Mode::Keyboard;
Stream* s_io = nullptr;
HWCDC* s_jtag = nullptr;
USBCDC* s_cdc = nullptr;
unsigned long s_last_activity_ms = 0;

bool storedSetup() {
  Preferences p;
  if (!p.begin("usbmode", true)) return false;
  const bool v = p.getBool("setup", false);
  p.end();
  return v;
}

void storeSetup(bool setup) {
  Preferences p;
  if (!p.begin("usbmode", false)) return;
  p.putBool("setup", setup);
  p.end();
}

/** The USB pads to the USB-Serial/JTAG port. The PHY selection sits in the RTC domain,
 *  which a software restart keeps: after TinyUSB had the PHY the JTAG port would stay
 *  off the bus (same steps as Arduino's switch to the ROM's USB download). */
void phyToJtag() {
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                      (RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE));
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
}

/** Drop off the bus for a moment so the PC sees the device leave, then restart. */
[[noreturn]] void restartDetached() {
  Console.flush();
  delay(150);  // let the reply go out
  if (s_mode == Mode::Keyboard) {
    tud_disconnect();
    delay(50);
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);
    phyToJtag();
    // D+/D- low: a clear disconnect, so the PC enumerates whatever comes back.
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    pinMode(19, OUTPUT_OPEN_DRAIN);
    pinMode(20, OUTPUT_OPEN_DRAIN);
    digitalWrite(19, LOW);
    digitalWrite(20, LOW);
  } else {
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  }
  delay(300);
  ESP.restart();
  for (;;) {
  }
}

}  // namespace

void begin() {
  s_mode = storedSetup() ? Mode::Setup : Mode::Keyboard;
  if (s_mode == Mode::Setup) {
    phyToJtag();  // in case TinyUSB had the PHY before the restart
    s_jtag = new HWCDC();
    s_jtag->setRxBufferSize(kRxBuffer);
    s_jtag->begin(115200);
    s_io = s_jtag;
  } else {
    // Constructed here, before USB.begin(): the CDC and HID interfaces register with
    // TinyUSB when created, and the USB descriptors are built when USB starts.
    s_cdc = new USBCDC(0);
    s_cdc->setRxBufferSize(kRxBuffer);
    s_cdc->begin(115200);
    s_io = s_cdc;
    services::hid::begin();
    USB.PID(kKeyboardPid);
    USB.productName("Plane Radar 3D");
    USB.manufacturerName("Plane Radar");
    USB.begin();
  }
  s_last_activity_ms = millis();
}

Mode mode() { return s_mode; }

void switchTo(Mode m) {
  Console.printf("PR:USBMODE {\"usb\":\"%s\",\"restart\":%s}\n", m == Mode::Setup ? "setup" : "kb",
                 m == s_mode ? "false" : "true");
  if (m == s_mode) return;
  storeSetup(m == Mode::Setup);
  restartDetached();
}

void noteActivity() { s_last_activity_ms = millis(); }

void tick() {
  if (s_mode != Mode::Setup || services::serial_ota::active()) return;
  if (millis() - s_last_activity_ms >= kSetupIdleMs) {
    storeSetup(false);
    restartDetached();
  }
}

}  // namespace console

// --- Stream forwarding ---
int ConsoleIO::available() { return console::s_io ? console::s_io->available() : 0; }
int ConsoleIO::read() { return console::s_io ? console::s_io->read() : -1; }
int ConsoleIO::peek() { return console::s_io ? console::s_io->peek() : -1; }
void ConsoleIO::flush() {
  if (console::s_io) console::s_io->flush();
}
size_t ConsoleIO::write(uint8_t c) { return write(&c, 1); }
size_t ConsoleIO::write(const uint8_t* buffer, size_t size) {
  if (console::s_io == nullptr) return 0;
  if (console::s_cdc == nullptr) return console::s_io->write(buffer, size);
  // Keyboard mode: USBCDC::write() spins for ever while the computer reads nothing (a
  // stalled TinyUSB port, a program that holds the port without reading): the whole card
  // would hang. Only what fits is written; after a short wait the rest is dropped.
  static unsigned long s_stalled_ms = 0;
  size_t done = 0;
  const unsigned long t0 = millis();
  while (done < size) {
    const int room = console::s_cdc->availableForWrite();
    if (room <= 0) {
      const bool recently = s_stalled_ms != 0 && millis() - s_stalled_ms < 1000;
      if (recently || millis() - t0 >= 30) {
        s_stalled_ms = millis();
        break;
      }
      delay(1);
      continue;
    }
    const size_t n = std::min(static_cast<size_t>(room), size - done);
    const size_t w = console::s_cdc->write(buffer + done, n);
    if (w == 0) break;
    done += w;
  }
  if (done == size) s_stalled_ms = 0;
  return size;  // dropped bytes count as written: callers must not retry
}
