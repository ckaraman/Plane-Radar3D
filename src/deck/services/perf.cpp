#include "services/perf.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "hardware/console.h"

namespace services::perf {

namespace {

constexpr unsigned long kReportMs = 5000;

unsigned long s_start_ms = 0;
uint32_t s_turn_max = 0, s_turns = 0, s_frames = 0, s_lines = 0, s_rx_ms = 0;
uint32_t s_cover_n = 0, s_cover_us = 0, s_thumb_n = 0, s_thumb_us = 0;

}  // namespace

void turn(uint32_t ms, bool cover_rx) {
  ++s_turns;
  if (ms > s_turn_max) s_turn_max = ms;
  if (cover_rx) s_rx_ms += ms;
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

void tick(const char* scr, const char* page) {
  const unsigned long now = millis();
  if (s_start_ms == 0) s_start_ms = now;
  const unsigned long span = now - s_start_ms;
  if (span < kReportMs) return;
  JsonDocument doc;
  doc["scr"] = scr;
  doc["page"] = page;
  doc["fps"] = roundf(s_frames * 10000.0f / span) / 10.0f;
  doc["turn_max"] = s_turn_max;
  doc["turns"] = s_turns;
  doc["lines"] = s_lines;
  doc["cover_n"] = s_cover_n;
  doc["cover_ms"] = s_cover_us / 1000;
  doc["thumb_n"] = s_thumb_n;
  doc["thumb_ms"] = s_thumb_us / 1000;
  doc["rx_ms"] = s_rx_ms;
  doc["heap"] = ESP.getFreeHeap();
  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  Console.printf("PR:PERF %s\n", buf);
  s_start_ms = now;
  s_turn_max = s_turns = s_frames = s_lines = s_rx_ms = 0;
  s_cover_n = s_cover_us = s_thumb_n = s_thumb_us = 0;
}

}  // namespace services::perf
