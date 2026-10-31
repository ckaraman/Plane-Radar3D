#pragma once

#include <cstdint>

/**
 * Timing report for system 2 (stage 10a): counters fed by the main loop, the cable reader
 * and the deck's JPEG decoding; every 5 s one line goes to the PC program, which shows it
 * and keeps it in perf.log. Values are for that 5 s and start again from zero:
 *   PR:PERF {"scr":"deck","page":"music","fps":12.5,"turn_max":340,"turns":180,"lines":64,
 *            "cover_n":1,"cover_ms":95,"dec_ms":60,"thumb_n":10,"thumb_ms":120,"rx_ms":1800,"heap":81234,
 *            "psram":1457936,"psram_blk":1179648,"q":59,"draw":[14,62,9],"front":[3,20,15,10,14],
 *            "qpanel":[0,59],"usb":{"txfull_ms":15020,"drop":2310,"rxgap_ms":16000,"dtr_off":0,
 *            "unmount":0,"susp":0,"fix":0,"before":["PR:Q {i:3,","PR:VOL {v:"]}}
 * Stage 10c: "q" frames drawn with the queue open, "draw" the slowest frame's parts in ms
 * (page base copied, moving parts, shown), "front" that frame's moving parts by kind (tabs,
 * text, bars, controls, next card / queue panel), "qpanel" the queue panel kept / drawn again,
 * "usb" (only after a problem) the link's troubles, see console::LinkStats. A report that
 * does not get through is not counted: the next one covers its time too.
 */
namespace services::perf {

void turn(uint32_t ms, bool cover_rx);  // one loop() turn took ms (a cover was arriving)
void frame();                           // updateScreen() ran
void line();                            // one line from the cable handled
void jpeg(bool thumb, uint32_t us);     // one drawJpg: the large cover or a queue thumb
void decode(uint32_t us);               // the drawJpg part of a cover build ("dec_ms")
// One page frame; parts_us: its moving parts by kind (5, as in "front").
void draw(uint32_t base_us, uint32_t front_us, uint32_t show_us, bool queue, const uint32_t* parts_us);
void queuePanel(bool kept);             // the queue panel copied from its kept picture or drawn

/** Sends the report when 5 s are up. scr / page: the screen shown ("" when no page). */
void tick(const char* scr, const char* page);

}  // namespace services::perf
