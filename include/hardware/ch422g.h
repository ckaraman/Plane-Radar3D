#pragma once

#include <cstdint>

/**
 * CH422G I/O expander on the Waveshare ESP32-S3-Touch-LCD-5 (I2C, shared with the
 * GT911 touch controller and the SHT30). It drives the LCD backlight and the LCD /
 * touch reset lines. Accessed through LovyanGFX's I2C driver on config::kI2cPort so
 * it does not fight the touch driver for the bus.
 *
 * Expander pins: EXIO1 touch reset, EXIO2 backlight, EXIO3 LCD reset, EXIO4 SD CS.
 */
namespace hw::ch422g {

constexpr uint8_t kTouchReset = 1;
constexpr uint8_t kBacklight = 2;
constexpr uint8_t kLcdReset = 3;
constexpr uint8_t kSdCs = 4;

/** Start the bus and put the expander in output mode (everything high: backlight on,
 *  resets released, SD card deselected). */
bool begin();
/** Drive one expander output. */
void write(uint8_t pin, bool high);
/**
 * Reset the LCD and the GT911. The touch INT pin is held low while the reset is
 * released, which puts the GT911 at I2C address 0x5D.
 */
void resetPanelAndTouch(int touch_int_pin);

}  // namespace hw::ch422g
