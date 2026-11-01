#include "services/perf.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "hardware/console.h"

namespace services::perf {

namespace {

constexpr unsigned long kReportMs = 5000;

unsigned long s_start_ms = 0;
unsigned long s_rx_since = 0;  // a cover started arriving (0: none on its way)
uint32_t s_turn_max = 0, s_turns = 0, s_frames = 0, s_lines = 0, s_rx_ms = 0;
uint32_t s_cover_n = 0, s_cover_us = 0, s_dec_us = 0, s_thumb_n = 0, s_thumb_us = 0;
uint32_t s_q_frames = 0, s_draw_us[3] = {0, 0, 0}, s_front_us[5] = {0, 0, 0, 0, 0};
uint32_t s_card_us[5] = {0, 0, 0, 0, 0};
uint32_t s_qp_kept = 0, s_qp_drawn = 0;
unsigned long s_retry_ms = 0;  // the last report was dropped then: the next one waits 5 s

}  // namespace

void turn(uint32_t ms, bool cover_rx) {
  ++s_turns;
  if (ms > s_turn_max) s_turn_max = ms;
  // Wall time while a cover arrives (the loop's own delays included).
  const unsigned long now = millis();
  if (cover_rx && s_rx_since == 0) s_rx_since = now;
  if (!cover_rx && s_rx_since != 0) {
    s_rx_ms += now - s_rx_since;
    s_rx_since = 0;
  }
}

void frame() { ++s_frames; }

void line() { ++s_lines; }

void jpeg(bool thumb, uint32_t us) {
  if (thumb) {
    ++s_thumb_n;
    s_thumb_us += us;
  } else {
    ++s_cover_n;
    s_cover_us += us;
  }
}

void decode(uint32_t us) { s_dec_us += us; }

void draw(uint32_t base_us, uint32_t front_us, uint32_t show_us, bool queue, const uint32_t* parts_us,
          const uint32_t* card_us) {
  if (queue) ++s_q_frames;
  if (base_us + front_us + show_us <= s_draw_us[0] + s_draw_us[1] + s_draw_us[2]) return;
  s_draw_us[0] = base_us;
  s_draw_us[1] = front_us;
  s_draw_us[2] = show_us;
  for (int i = 0; i < 5; ++i) {
    s_front_us[i] = parts_us[i];
    s_card_us[i] = card_us[i];
  }
}

void queuePanel(bool kept) { ++(kept ? s_qp_kept : s_qp_drawn); }

void tick(const char* scr, const char* page) {
  const unsigned long now = millis();
  if (s_start_ms == 0) s_start_ms = now;
  const unsigned long span = now - s_start_ms;
  if (span < kReportMs) return;
  if (s_retry_ms != 0 && now - s_retry_ms < kReportMs) return;
  if (s_rx_since != 0) {  // still arriving: this report gets its part so far
    s_rx_ms += now - s_rx_since;
    s_rx_since = now;
  }
  JsonDocument doc;
  doc["scr"] = scr;
  doc["page"] = page;
  doc["fps"] = roundf(s_frames * 10000.0f / span) / 10.0f;
  doc["turn_max"] = s_turn_max;
  doc["turns"] = s_turns;
  doc["lines"] = s_lines;
  doc["cover_n"] = s_cover_n;
  doc["cover_ms"] = s_cover_us / 1000;
  doc["dec_ms"] = s_dec_us / 1000;
  doc["thumb_n"] = s_thumb_n;
  doc["thumb_ms"] = s_thumb_us / 1000;
  doc["rx_ms"] = s_rx_ms;
  doc["heap"] = ESP.getFreeHeap();
  doc["psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  doc["psram_blk"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  doc["q"] = s_q_frames;
  JsonArray d = doc["draw"].to<JsonArray>();
  for (uint32_t us : s_draw_us) d.add(us / 1000);
  JsonArray fp = doc["front"].to<JsonArray>();
  for (uint32_t us : s_front_us) fp.add(us / 1000);
  JsonArray cp = doc["card"].to<JsonArray>();
  for (uint32_t us : s_card_us) cp.add(us / 1000);
  JsonArray qp = doc["qpanel"].to<JsonArray>();
  qp.add(s_qp_kept);
  qp.add(s_qp_drawn);
  // Stage 10c: the USB link's troubles, once something went wrong.
  const console::LinkStats& l = console::link();
  const bool usb = l.txfull_ms || l.dropped || l.rxgap_ms >= 5000 || l.dtr_off || l.unmount || l.suspend;
  if (usb) {
    JsonObject u = doc["usb"].to<JsonObject>();
    u["txfull_ms"] = l.txfull_ms;
    u["drop"] = l.dropped;
    u["rxgap_ms"] = l.rxgap_ms;
    u["dtr_off"] = l.dtr_off;
    u["unmount"] = l.unmount;
    u["susp"] = l.suspend;
    u["fix"] = console::usbRecoveries();
    u["early"] = l.early;
    u["fifo"] = l.fifo;
    u["rxwait"] = l.rxwait;
    u["lock_ms"] = l.lock_ms;
    u["core"] = l.core;
    JsonArray b = u["before"].to<JsonArray>();
    for (const auto& line : l.before) {
      if (line[0]) b.add(line);
    }
  }
  char buf[1024];
  serializeJson(doc, buf, sizeof(buf));
  const uint32_t dropped = console::droppedBytes();
  Console.printf("PR:PERF %s\n", buf);
  if (console::droppedBytes() != dropped) {
    // Not through (a stall): the counters go on, so the next report that gets through
    // covers the whole stall.
    s_retry_ms = now;
    return;
  }
  s_retry_ms = 0;
  if (usb) console::clearLink();
  s_start_ms = now;
  s_turn_max = s_turns = s_frames = s_lines = s_rx_ms = 0;
  s_cover_n = s_cover_us = s_dec_us = s_thumb_n = s_thumb_us = 0;
  s_q_frames = s_qp_kept = s_qp_drawn = 0;
  for (uint32_t& us : s_draw_us) us = 0;
  for (uint32_t& us : s_front_us) us = 0;
  for (uint32_t& us : s_card_us) us = 0;
}

}  // namespace services::perf
