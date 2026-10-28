#if defined(BOARD_WS5)

#include "hardware/ch422g.h"
#include "hardware/console.h"

#include <Arduino.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

namespace hw::ch422g {

namespace {

// The CH422G has no register map: each "address" is a command.
constexpr int kAddrSet = 0x24;  // mode: bit0 = EXIO pins are outputs
constexpr int kAddrIo = 0x38;   // EXIO0..7 output levels
constexpr uint32_t kFreq = 400000;

uint8_t s_out = 0xFF;  // all high: backlight on, resets released, SD deselected
bool s_ready = false;

bool send(int addr, uint8_t value) {
  return lgfx::i2c::transactionWrite(config::kI2cPort, addr, &value, 1, kFreq).has_value();
}

}  // namespace

bool begin() {
  if (s_ready) return true;
  if (!lgfx::i2c::init(config::kI2cPort, config::kI2cPinSda, config::kI2cPinScl).has_value()) {
    return false;
  }
  s_ready = send(kAddrSet, 0x01) && send(kAddrIo, s_out);
  if (!s_ready) Console.println("ch422g: no answer on the I2C bus");
  return s_ready;
}

void write(uint8_t pin, bool high) {
  if (!s_ready && !begin()) return;
  if (high) {
    s_out |= static_cast<uint8_t>(1u << pin);
  } else {
    s_out &= static_cast<uint8_t>(~(1u << pin));
  }
  send(kAddrSet, 0x01);
  send(kAddrIo, s_out);
}

void resetPanelAndTouch(int touch_int_pin) {
  if (!begin()) return;
  pinMode(touch_int_pin, OUTPUT);
  digitalWrite(touch_int_pin, LOW);
  write(kTouchReset, false);
  write(kLcdReset, false);
  delay(20);
  write(kLcdReset, true);
  write(kTouchReset, true);  // INT low at this edge -> GT911 address 0x5D
  delay(10);
  pinMode(touch_int_pin, INPUT);
  delay(60);  // GT911 needs ~50 ms before it answers
}

}  // namespace hw::ch422g

#endif  // BOARD_WS5
