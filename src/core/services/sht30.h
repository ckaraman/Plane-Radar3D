#pragma once

namespace services::sht30 {

/** Start I2C and probe the sensor. Safe to call when nothing is connected. */
void init();

/** Non-blocking: triggers a measurement every config::kSht30IntervalMs and
 *  collects it ~20 ms later. Call from loop(). */
void tick();

/** True once a reading with a valid checksum has arrived (and is < 60 s old). */
bool valid();
float temperatureC();
float humidity();

}  // namespace services::sht30
