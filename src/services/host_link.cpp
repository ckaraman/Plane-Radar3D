#include "services/host_link.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "hardware/console.h"
#include "services/map_store.h"

namespace services::host {

namespace {

constexpr unsigned long kActiveMs = 6000;
constexpr size_t kMaxCover = 300 * 1024;

unsigned long s_last_ms = 0;
bool s_seen = false;
int s_volume = 50;
bool s_muted = false;

NowPlaying s_now;
uint32_t s_now_gen = 1;

uint8_t* s_cover = nullptr;  // active JPEG (PSRAM)
size_t s_cover_len = 0;
uint32_t s_cover_gen = 1;

uint8_t* s_stage = nullptr;
size_t s_stage_len = 0, s_stage_got = 0;
uint32_t s_stage_crc = 0;

void seen() {
  s_last_ms = millis();
  s_seen = true;
}

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int b64Decode(const char* in, uint8_t* out, size_t cap) {
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (; *in && *in != '='; ++in) {
    const int v = b64Value(*in);
    if (v < 0) return -1;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return -1;
      out[n++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return static_cast<int>(n);
}

void reply(const char* tag, const char* body) { Console.printf("PR:%s %s\n", tag, body); }

void dropStage() {
  if (s_stage != nullptr) heap_caps_free(s_stage);
  s_stage = nullptr;
  s_stage_len = s_stage_got = 0;
}

}  // namespace

bool active() { return s_seen && millis() - s_last_ms < kActiveMs; }
int volume() { return s_volume; }
bool muted() { return s_muted; }
const NowPlaying& now() { return s_now; }
uint32_t nowGeneration() { return s_now_gen; }

uint32_t positionNow() {
  if (!s_now.valid) return 0;
  uint32_t p = s_now.position_s;
  if (s_now.playing) p += (millis() - s_now.position_ms) / 1000;
  if (s_now.duration_s > 0 && p > s_now.duration_s) p = s_now.duration_s;
  return p;
}

const uint8_t* cover(size_t* len) {
  if (len) *len = s_cover_len;
  return s_cover;
}
uint32_t coverGeneration() { return s_cover_gen; }

void handleVol(const char* json) {
  seen();
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;
  if (doc["v"].is<int>()) s_volume = constrain(doc["v"].as<int>(), 0, 100);
  if (doc["m"].is<bool>()) s_muted = doc["m"].as<bool>();
}

void handleNow(const char* json) {
  seen();
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;
  NowPlaying n;
  if (doc["none"].as<bool>()) {
    n.valid = false;
  } else {
    n.valid = true;
    n.playing = doc["play"].as<bool>();
    strlcpy(n.title, doc["t"] | "", sizeof(n.title));
    strlcpy(n.artist, doc["a"] | "", sizeof(n.artist));
    strlcpy(n.album, doc["al"] | "", sizeof(n.album));
    strlcpy(n.app, doc["app"] | "", sizeof(n.app));
    n.duration_s = doc["d"] | 0;
    n.position_s = doc["p"] | 0;
    n.position_ms = millis();
    n.id = doc["id"] | 0;
    n.back_s = doc["back"] | 60;
  }
  const bool changed = n.valid != s_now.valid || n.playing != s_now.playing || n.id != s_now.id ||
                       strcmp(n.title, s_now.title) != 0 || strcmp(n.artist, s_now.artist) != 0 ||
                       n.duration_s != s_now.duration_s ||
                       labs(static_cast<long>(n.position_s) - static_cast<long>(positionNow())) > 2;
  s_now = n;
  if (changed) ++s_now_gen;
}

void handleCoverBegin(const char* json) {
  seen();
  JsonDocument doc;
  dropStage();
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc["len"].is<uint32_t>()) {
    reply("ERR", "{\"msg\":\"bad_json\"}");
    return;
  }
  const size_t len = doc["len"].as<uint32_t>();
  if (len == 0 || len > kMaxCover) {
    reply("ERR", "{\"msg\":\"cover_too_big\"}");
    return;
  }
  s_stage = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
  if (s_stage == nullptr) {
    reply("ERR", "{\"msg\":\"cover_no_memory\"}");
    return;
  }
  s_stage_len = len;
  s_stage_crc = doc["crc"] | 0u;
  reply("COVERACK", "{\"seq\":0}");
}

void handleCoverChunk(char* args) {
  seen();
  char* data = strchr(args, ' ');
  if (data == nullptr || s_stage == nullptr) {
    reply("ERR", "{\"msg\":\"bad_cover_chunk\"}");
    return;
  }
  *data++ = '\0';
  const long seq = atol(args);
  uint8_t bytes[480];
  const int n = b64Decode(data, bytes, sizeof(bytes));
  if (n <= 0 || s_stage_got + n > s_stage_len) {
    dropStage();
    reply("ERR", "{\"msg\":\"bad_cover_chunk\"}");
    return;
  }
  memcpy(s_stage + s_stage_got, bytes, n);
  s_stage_got += n;
  char body[32];
  snprintf(body, sizeof(body), "{\"seq\":%ld}", seq);
  reply("COVERACK", body);
}

void handleCoverEnd() {
  seen();
  if (s_stage == nullptr || s_stage_got != s_stage_len ||
      (s_stage_crc != 0 && services::map::crc32(s_stage, s_stage_len) != s_stage_crc)) {
    dropStage();
    reply("ERR", "{\"msg\":\"bad_cover\"}");
    return;
  }
  if (s_cover != nullptr) heap_caps_free(s_cover);
  s_cover = s_stage;
  s_cover_len = s_stage_len;
  s_stage = nullptr;
  s_stage_len = s_stage_got = 0;
  ++s_cover_gen;
  reply("OK", "{\"cover\":true}");
}

void sendKey(int slot) { Console.printf("PR:KEY {\"i\":%d}\n", slot); }
void sendVolume(int level) {
  s_volume = constrain(level, 0, 100);
  s_muted = false;
  Console.printf("PR:SETVOL {\"v\":%d}\n", s_volume);
}
void sendMute() {
  s_muted = !s_muted;
  Console.println("PR:MUTE {}");
}

}  // namespace services::host
