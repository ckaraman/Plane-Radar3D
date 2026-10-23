#include "services/sht30.h"

#include <Arduino.h>
#if defined(BOARD_WS5)
#define LGFX_USE_V1
#include <LovyanGFX.hpp>  // shared I2C bus: the touch driver's I2C port
#else
#include <Wire.h>
#endif

#include "config.h"

namespace services::sht30 {

namespace {

constexpr unsigned long kMeasureWaitMs = 20;  // high repeatability: max 15.5 ms
constexpr unsigned long kStaleMs = 60000;

enum class State { Idle, Waiting };
State s_state = State::Idle;
unsigned long s_last_trigger_ms = 0;
unsigned long s_last_ok_ms = 0;
bool s_have = false;
float s_temp = 0.0f;
float s_hum = 0.0f;
uint8_t s_fail_streak = 0;

uint8_t crc8(const uint8_t* data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool trigger() {
  // Single shot, high repeatability, no clock stretching.
#if defined(BOARD_WS5)
  const uint8_t cmd[2] = {0x24, 0x00};
  return lgfx::i2c::transactionWrite(config::kI2cPort, config::kSht30Address, cmd, 2, 100000)
      .has_value();
#else
  Wire.beginTransmission(config::kSht30Address);
  Wire.write(0x24);
  Wire.write(0x00);
  return Wire.endTransmission() == 0;
#endif
}

bool collect() {
  uint8_t buf[6];
#if defined(BOARD_WS5)
  if (!lgfx::i2c::transactionRead(config::kI2cPort, config::kSht30Address, buf, 6, 100000)
           .has_value()) {
    return false;
  }
#else
  if (Wire.requestFrom(static_cast<int>(config::kSht30Address), 6) != 6) {
    return false;
  }
  for (uint8_t& b : buf) {
    b = static_cast<uint8_t>(Wire.read());
  }
#endif
  if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
    return false;
  }
  const uint16_t rt = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
  const uint16_t rh = static_cast<uint16_t>((buf[3] << 8) | buf[4]);
  s_temp = -45.0f + 175.0f * static_cast<float>(rt) / 65535.0f + config::kSht30TempOffsetC;
  s_hum = 100.0f * static_cast<float>(rh) / 65535.0f;
  if (s_hum < 0.0f) s_hum = 0.0f;
  if (s_hum > 100.0f) s_hum = 100.0f;
  return true;
}

}  // namespace

void init() {
#if !defined(BOARD_WS5)  // the 5" board's bus is started by the display (CH422G / touch)
  Wire.begin(config::kI2cPinSda, config::kI2cPinScl, 100000);
  Wire.setTimeOut(20);
#endif
  // First measurement right away so the status bar fills in quickly.
  s_last_trigger_ms = millis() - config::kSht30IntervalMs;
}

void tick() {
  const unsigned long now = millis();
  if (s_state == State::Idle) {
    if (now - s_last_trigger_ms >= config::kSht30IntervalMs) {
      s_last_trigger_ms = now;
      if (trigger()) {
        s_state = State::Waiting;
      } else if (s_fail_streak < 255) {
        if (++s_fail_streak == 3) {
          Serial.println("sht30: no answer on I2C (check SDA/SCL wiring)");
        }
      }
    }
    return;
  }
  if (now - s_last_trigger_ms < kMeasureWaitMs) {
    return;
  }
  s_state = State::Idle;
  if (collect()) {
    s_have = true;
    s_last_ok_ms = now;
    s_fail_streak = 0;
  }
}

bool valid() { return s_have && millis() - s_last_ok_ms < kStaleMs; }
float temperatureC() { return s_temp; }
float humidity() { return s_hum; }

}  // namespace services::sht30
