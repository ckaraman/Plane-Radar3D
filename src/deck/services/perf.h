#pragma once

#include <cstdint>

/**
 * Timing report for system 2 (stage 10a): counters fed by the main loop, the cable reader
 * and the deck's JPEG decoding; every 5 s one line goes to the PC program, which shows it
 * and keeps it in perf.log. Values are for that 5 s and start again from zero:
 *   PR:PERF {"scr":"deck","page":"music","fps":12.5,"turn_max":340,"turns":180,"lines":64,
 *            "cover_n":1,"cover_ms":95,"dec_ms":60,"thumb_n":10,"thumb_ms":120,"rx_ms":1800,"heap":81234}
 */
namespace services::perf {

void turn(uint32_t ms, bool cover_rx);  // one loop() turn took ms (a cover was arriving)
void frame();                           // updateScreen() ran
void line();                            // one line from the cable handled
void jpeg(bool thumb, uint32_t us);     // one drawJpg: the large cover or a queue thumb
void decode(uint32_t us);               // the drawJpg part of a cover build ("dec_ms")

/** Sends the report when 5 s are up. scr / page: the screen shown ("" when no page). */
void tick(const char* scr, const char* page);

}  // namespace services::perf
