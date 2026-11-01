#include "hardware/console.h"

#include <HWCDC.h>
#include <Preferences.h>
#include <USB.h>
#include <USBCDC.h>

#include <algorithm>
#include <esp_private/periph_ctrl.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#if defined(WITH_DECK)
#include "services/hid_keys.h"
#endif
#include "services/serial_ota.h"

extern "C" bool tud_disconnect(void);  // TinyUSB (tusb.h)
extern "C" bool tud_connect(void);
extern "C" bool tud_cdc_n_connected(uint8_t itf);
extern "C" bool tud_mounted(void);
extern "C" bool tud_suspended(void);
extern "C" uint32_t tud_cdc_n_write_available(uint8_t itf);

ConsoleIO Console;

namespace console {

namespace {

/** Setup mode returns to keyboard mode after this long without a line from the page. */
constexpr unsigned long kSetupIdleMs = 3UL * 60UL * 1000UL;
constexpr size_t kRxBuffer = 4096;  // the page and the bridge send lines in bursts
// Keyboard mode: the helper sends one line at a time and waits for the answer; a smaller
// buffer leaves internal RAM to the radar's and the forecast's TLS connections.
constexpr size_t kRxBufferKeyboard = 2048;

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
#if defined(WITH_DECK)
  s_mode = storedSetup() ? Mode::Setup : Mode::Keyboard;
#else
  s_mode = Mode::Setup;  // no stream deck: always the USB-Serial/JTAG port
#endif
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
    s_cdc->setRxBufferSize(kRxBufferKeyboard);
    s_cdc->begin(115200);
    s_io = s_cdc;
#if defined(WITH_DECK)
    services::hid::begin();
#endif
    USB.PID(kKeyboardPid);
    USB.productName("Plane Radar 3D");
    USB.manufacturerName("Plane Radar");
    USB.begin();
  }
  s_last_activity_ms = millis();
}

Mode mode() { return s_mode; }

void switchTo(Mode m) {
#if !defined(WITH_DECK)
  m = Mode::Setup;  // there is no keyboard mode to go to
#endif
  Console.printf("PR:USBMODE {\"usb\":\"%s\",\"restart\":%s}\n", m == Mode::Setup ? "setup" : "kb",
                 m == s_mode ? "false" : "true");
  if (m == s_mode) return;
  storeSetup(m == Mode::Setup);
  restartDetached();
}

void noteActivity() { s_last_activity_ms = millis(); }

namespace {
unsigned long s_last_host_ms = 0;
bool s_host_seen = false;
int s_recoveries = 0;
// Since when everything written is dropped (0: the computer reads what the card says).
unsigned long s_tx_full_since = 0;

// Stage 10c: the link's troubles, for the timing report.
LinkStats s_link;
uint32_t s_dropped_total = 0;
unsigned long s_last_rx_ms = 0;
char s_lines[4][12] = {};  // starts of the PC's last lines, oldest first
char s_cur[12] = {};
int s_cur_n = 0;
char s_full_before[4][12] = {};  // s_lines when the drop going on began
uint16_t s_full_fifo = 0, s_full_rxwait = 0;
uint8_t s_full_core = 0;

void txFull() {
  if (s_tx_full_since != 0) return;
  s_tx_full_since = millis();
  memcpy(s_full_before, s_lines, sizeof(s_lines));
  s_full_fifo = static_cast<uint16_t>(std::min<uint32_t>(tud_cdc_n_write_available(0), 0xFFFF));
  s_full_rxwait = static_cast<uint16_t>(std::max(0, std::min(s_cdc->available(), 0xFFFF)));
  s_full_core = static_cast<uint8_t>(xPortGetCoreID());
}

void txFree() {
  if (s_tx_full_since == 0) return;
  const uint32_t ms = millis() - s_tx_full_since;
  if (ms > s_link.txfull_ms) {
    s_link.txfull_ms = ms;
    memcpy(s_link.before, s_full_before, sizeof(s_full_before));
    s_link.fifo = s_full_fifo;
    s_link.rxwait = s_full_rxwait;
    s_link.core = s_full_core;
  }
  s_tx_full_since = 0;
}

void noteRx(int c) {
  const unsigned long now = millis();
  if (s_last_rx_ms != 0 && now - s_last_rx_ms > s_link.rxgap_ms) s_link.rxgap_ms = now - s_last_rx_ms;
  s_last_rx_ms = now;
  if (c == '\n' || c == '\r') {
    if (s_cur_n == 0) return;
    memmove(s_lines[0], s_lines[1], sizeof(s_lines[0]) * 3);
    memcpy(s_lines[3], s_cur, sizeof(s_cur));
    memset(s_cur, 0, sizeof(s_cur));
    s_cur_n = 0;
  } else if (s_cur_n < static_cast<int>(sizeof(s_cur)) - 1 && c >= 32 && c < 127 && c != '"' && c != '\\') {
    s_cur[s_cur_n++] = static_cast<char>(c);
  }
}
}  // namespace

const LinkStats& link() { return s_link; }
void clearLink() { s_link = LinkStats{}; }
uint32_t droppedBytes() { return s_dropped_total; }

void noteHost() {
  s_last_host_ms = millis();
  s_host_seen = true;
}

int usbRecoveries() { return s_recoveries; }

void tick() {
  if (s_mode == Mode::Keyboard) {
    static bool s_was_dtr = false, s_was_mounted = false, s_was_suspended = false;
    const bool dtr = tud_cdc_n_connected(0), mounted = tud_mounted(), suspended = tud_suspended();
    if (s_was_dtr && !dtr) ++s_link.dtr_off;
    if (s_was_mounted && !mounted) ++s_link.unmount;
    if (!s_was_suspended && suspended) ++s_link.suspend;
    s_was_dtr = dtr, s_was_mounted = mounted, s_was_suspended = suspended;
    // The helper sends its volume every 2 s. Silent for 20 s while a program still holds
    // the port (DTR on): the link stalled. Unplugging and plugging in again by software
    // brings it back; the helper reopens the port by itself.
    // The other way round: the helper still writes but reads nothing for 20 s (stage 10b-3).
    // Counted only while a program holds the port (with none, every write is dropped) and
    // once until the helper speaks again.
    if (!tud_cdc_n_connected(0)) txFree();
    const unsigned long now = millis();
    const bool tx_stuck = s_host_seen && s_tx_full_since != 0 && now - s_tx_full_since > 20000;
    // Stage 10e: the PC still writes (its read thread waits for the card) but nothing the
    // card says gets through for 3 s: stalled, no need to wait the 20 s (taps were lost).
    const bool pc_writes = s_last_rx_ms != 0 && now - s_last_rx_ms < 3000;
    const bool tx_short = s_host_seen && s_tx_full_since != 0 && now - s_tx_full_since > 3000 && pc_writes;
    if (((s_host_seen && now - s_last_host_ms > 20000) || tx_stuck || tx_short) && tud_cdc_n_connected(0)) {
      if (tx_short && !tx_stuck) ++s_link.early;
      ++s_recoveries;
      s_host_seen = false;  // once, until the helper speaks again
      txFree();
      tud_disconnect();
      delay(400);
      tud_connect();
    }
    return;
  }
#if defined(WITH_DECK)
  if (services::serial_ota::active()) return;
  if (millis() - s_last_activity_ms >= kSetupIdleMs) {
    storeSetup(false);
    restartDetached();
  }
#endif
}

}  // namespace console

// --- Stream forwarding ---
int ConsoleIO::available() { return console::s_io ? console::s_io->available() : 0; }
int ConsoleIO::read() {
  if (console::s_io == nullptr) return -1;
  const int c = console::s_io->read();
  if (c >= 0 && console::s_cdc != nullptr) console::noteRx(c);
  return c;
}
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
    const unsigned long t_lock = millis();
    const int room = console::s_cdc->availableForWrite();  // waits up to 250 ms for its lock
    const uint32_t waited = millis() - t_lock;
    if (waited > console::s_link.lock_ms) console::s_link.lock_ms = static_cast<uint16_t>(std::min<uint32_t>(waited, 0xFFFF));
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
  if (done == size) {
    s_stalled_ms = 0;
    console::txFree();
  } else {
    console::s_link.dropped += size - done;
    console::s_dropped_total += size - done;
    console::txFull();
  }
  return size;  // dropped bytes count as written: callers must not retry
}
