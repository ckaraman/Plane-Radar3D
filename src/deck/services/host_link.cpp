#include "services/host_link.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "config.h"
#include "hardware/console.h"
#include "services/map_store.h"
#include "services/queue_carry.h"

namespace services::host {

namespace {

// The helper counts as there for 30 s after its last line: the knob and the volume bar
// then keep to its value through a short gap (a long cover transfer, a busy moment)
// instead of jumping to the card's own guess and back.
constexpr unsigned long kActiveMs = 30000;
constexpr size_t kMaxCover = 300 * 1024;

unsigned long s_last_ms = 0;
bool s_seen = false;
int s_volume = 50;
bool s_muted = false;

NowPlaying s_now;
uint32_t s_now_gen = 1;

uint8_t* s_cover = nullptr;  // active JPEG (PSRAM)
size_t s_cover_len = 0;
int s_cover_w = 0, s_cover_h = 0;
uint32_t s_cover_id = 0;
uint32_t s_cover_gen = 1;
bool s_cover_held = false;      // holdCover: the active JPEG is being decoded
uint8_t* s_cover_old = nullptr;  // replaced while held, freed when let go

uint8_t* s_stage = nullptr;
size_t s_stage_len = 0, s_stage_got = 0;
uint32_t s_stage_crc = 0;
long s_stage_next = 1;  // chunk number expected next
int s_stage_w = 0, s_stage_h = 0;
uint32_t s_stage_id = 0;

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

uint32_t positionNowMs() {
  if (!s_now.valid) return 0;
  uint32_t p = s_now.pos_ms;
  if (s_now.playing) p += millis() - s_now.anchor_ms;
  if (s_now.duration_s > 0 && p > s_now.duration_s * 1000UL) p = s_now.duration_s * 1000UL;
  return p;
}
uint32_t positionNow() { return positionNowMs() / 1000; }

const uint8_t* cover(size_t* len, int* w, int* h) {
  if (len) *len = s_cover_len;
  if (w) *w = s_cover_w;
  if (h) *h = s_cover_h;
  return s_cover;
}
uint32_t coverId() { return s_cover_id; }
bool coverReceiving() { return s_stage != nullptr && millis() - s_last_ms < 1500; }
uint32_t coverGeneration() { return s_cover_gen; }
void holdCover(bool hold) {
  s_cover_held = hold;
  if (!hold && s_cover_old != nullptr) {
    heap_caps_free(s_cover_old);
    s_cover_old = nullptr;
  }
}

void handleVol(const char* json) {
  seen();
  console::noteHost();
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
    n.pos_ms = doc["pm"].is<uint32_t>() ? doc["pm"].as<uint32_t>() : n.position_s * 1000UL + 500;
    n.anchor_ms = millis();
    n.id = doc["id"] | 0;
    n.back_s = doc["back"] | 60;
  }
  const bool same_track = n.valid == s_now.valid && n.playing == s_now.playing && n.id == s_now.id &&
                          strcmp(n.title, s_now.title) == 0 && strcmp(n.artist, s_now.artist) == 0 &&
                          strcmp(n.album, s_now.album) == 0 && strcmp(n.app, s_now.app) == 0 &&
                          n.duration_s == s_now.duration_s;
  // The heartbeat repeats the position: within 1.5 s of ours the running clock is kept, so
  // the bar never steps back a little every few seconds. A real seek moves it.
  const long drift = static_cast<long>(n.pos_ms) - static_cast<long>(positionNowMs());
  const bool keep_clock = same_track && n.valid && labs(drift) <= 1500;
  if (keep_clock) {
    n.pos_ms = s_now.pos_ms;
    n.anchor_ms = s_now.anchor_ms;
  }
  s_now = n;
  if (!keep_clock) ++s_now_gen;
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
  s_stage_next = 1;
  s_stage_id = doc["id"] | 0u;
  s_stage_w = doc["w"] | 0;
  s_stage_h = doc["h"] | 0;
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
  char body[32];
  if (seq < s_stage_next) {  // already have it (its answer got lost): answer again
    snprintf(body, sizeof(body), "{\"seq\":%ld}", seq);
    reply("COVERACK", body);
    return;
  }
  if (seq > s_stage_next) {  // one went missing: from there again
    snprintf(body, sizeof(body), "{\"need\":%ld}", s_stage_next);
    reply("COVERNAK", body);
    return;
  }
  uint8_t bytes[480];
  const int n = b64Decode(data, bytes, sizeof(bytes));
  if (n <= 0 || s_stage_got + n > s_stage_len) {
    snprintf(body, sizeof(body), "{\"need\":%ld}", s_stage_next);  // garbled line
    reply("COVERNAK", body);
    return;
  }
  memcpy(s_stage + s_stage_got, bytes, n);
  s_stage_got += n;
  ++s_stage_next;
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
  if (s_cover_held && s_cover_old == nullptr) {
    s_cover_old = s_cover;  // a page build on core 0 still reads it: freed when it lets go
  } else if (s_cover != nullptr) {
    heap_caps_free(s_cover);
  }
  s_cover = s_stage;
  s_cover_len = s_stage_len;
  s_cover_w = s_stage_w;
  s_cover_h = s_stage_h;
  s_cover_id = s_stage_id;
  s_stage = nullptr;
  s_stage_len = s_stage_got = 0;
  ++s_cover_gen;
  reply("OK", "{\"cover\":true}");
}

// ------------------------------------------------------------------ queue
namespace {

// Rows in PSRAM, made on the first queue: internal RAM is what the TLS connections of
// the radar and the forecast need (a few KB less and they fail).
QueueRow* s_q = nullptr;
int s_qn = 0;
bool s_qvalid = false;
uint32_t s_qgen = 0;
uint32_t s_qversion = 1;
char s_qsrc[24] = "";

QueueRow* s_qs = nullptr;  // rows coming in
const QueueRow kNoRow{};

bool ensureRows() {
  if (s_q == nullptr) s_q = static_cast<QueueRow*>(heap_caps_calloc(kQueueMax, sizeof(QueueRow), MALLOC_CAP_SPIRAM));
  if (s_qs == nullptr) s_qs = static_cast<QueueRow*>(heap_caps_calloc(kQueueMax, sizeof(QueueRow), MALLOC_CAP_SPIRAM));
  return s_q != nullptr && s_qs != nullptr;
}
int s_qsn = 0, s_qs_want = 0;
uint32_t s_qs_gen = 0;
int16_t s_qs_from[kQueueMax];  // old row whose picture each row coming in keeps ("p")
bool s_qs_open = false;
char s_qs_src[24] = "";

// A row picture coming in (like the cover).
uint8_t* s_art = nullptr;
size_t s_art_len = 0, s_art_got = 0;
uint32_t s_art_crc = 0;
long s_art_next = 1;
int s_art_k = -1;
constexpr size_t kMaxArt = 48 * 1024;

void freeArts(QueueRow* rows, int n) {
  for (int i = 0; i < n; ++i) {
    if (rows[i].art != nullptr) heap_caps_free(const_cast<uint8_t*>(rows[i].art));
    rows[i].art = nullptr;
    rows[i].art_len = 0;
  }
}

void dropArt() {
  if (s_art != nullptr) heap_caps_free(s_art);
  s_art = nullptr;
  s_art_len = s_art_got = 0;
  s_art_k = -1;
}

void qack() { reply("QACK", "{}"); }

}  // namespace

bool queueValid() { return s_qvalid && s_qn > 0; }
int queueCount() { return s_qvalid ? s_qn : 0; }
const QueueRow& queueRow(int i) {
  if (s_q == nullptr || i < 0 || i >= kQueueMax) return kNoRow;
  return s_q[i];
}
const char* queueSource() { return s_qsrc; }
uint32_t queueVersion() { return s_qversion; }
int queueThumbPx() { return 40 * config::kDisplayWidth / 800; }

void sendQueuePlay(int row) {
  if (!queueValid() || row < 0 || row >= s_qn) return;
  JsonDocument doc;
  doc["x"] = s_q[row].index;
  doc["t"] = s_q[row].title;
  char buf[160];
  serializeJson(doc, buf, sizeof(buf));
  Console.printf("PR:QPLAY %s\n", buf);
}

void handleQueueBegin(const char* json) {
  seen();
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    reply("ERR", "{\"msg\":\"bad_json\"}");
    return;
  }
  if (!ensureRows()) {
    reply("ERR", "{\"msg\":\"queue_no_memory\"}");
    return;
  }
  s_qs_open = true;
  s_qsn = 0;
  s_qs_want = std::min<int>(doc["n"] | 0, kQueueMax);
  s_qs_gen = doc["gen"] | 0u;
  strlcpy(s_qs_src, doc["src"] | "", sizeof(s_qs_src));
  qack();
}

void handleQueueItem(const char* json) {
  seen();
  JsonDocument doc;
  if (!s_qs_open || deserializeJson(doc, json) != DeserializationError::Ok) {
    reply("ERR", "{\"msg\":\"bad_queue\"}");
    return;
  }
  const int k = doc["k"] | -1;
  if (k == s_qsn && k < kQueueMax) {  // a repeated line (lost answer) is just answered again
    QueueRow& r = s_qs[k];
    strlcpy(r.title, doc["t"] | "", sizeof(r.title));
    strlcpy(r.artist, doc["a"] | "", sizeof(r.artist));
    strlcpy(r.dur, doc["d"] | "", sizeof(r.dur));
    r.index = doc["x"] | 0;
    s_qs_from[k] = doc["p"] | -1;
    r.art = nullptr;
    r.art_len = 0;
    ++s_qsn;
  }
  qack();
}

void handleQueueEnd() {
  seen();
  if (s_qs_open) {
    dropArt();
    carryArts(s_q, s_qn, s_qs, s_qsn, s_qs_from);  // pictures the new list keeps
    freeArts(s_q, s_qn);
    memcpy(s_q, s_qs, sizeof(QueueRow) * kQueueMax);
    s_qn = s_qsn;
    s_qgen = s_qs_gen;
    strlcpy(s_qsrc, s_qs_src, sizeof(s_qsrc));
    s_qvalid = s_qn > 0;
    s_qs_open = false;
    ++s_qversion;
  }
  qack();
}

void handleQueueClear() {
  seen();
  dropArt();
  freeArts(s_q, s_qn);
  s_qn = 0;
  s_qvalid = false;
  s_qs_open = false;
  ++s_qversion;
  qack();
}

void handleQueueArtBegin(const char* json) {
  seen();
  dropArt();
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    reply("ERR", "{\"msg\":\"bad_json\"}");
    return;
  }
  const size_t len = doc["len"] | 0u;
  const int k = doc["k"] | -1;
  if ((doc["gen"] | 0u) != s_qgen || k < 0 || k >= s_qn || len == 0 || len > kMaxArt) {
    reply("ERR", "{\"msg\":\"art_skip\"}");
    return;
  }
  s_art = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
  if (s_art == nullptr) {
    reply("ERR", "{\"msg\":\"art_no_memory\"}");
    return;
  }
  s_art_len = len;
  s_art_got = 0;
  s_art_crc = doc["crc"] | 0u;
  s_art_next = 1;
  s_art_k = k;
  reply("QARTACK", "{\"seq\":0}");
}

void handleQueueArtChunk(char* args) {
  seen();
  char* data = strchr(args, ' ');
  char body[32];
  if (data == nullptr || s_art == nullptr) {
    reply("ERR", "{\"msg\":\"bad_art_chunk\"}");
    return;
  }
  *data++ = '\0';
  const long seq = atol(args);
  if (seq < s_art_next) {
    snprintf(body, sizeof(body), "{\"seq\":%ld}", seq);
    reply("QARTACK", body);
    return;
  }
  uint8_t bytes[480];
  const int n = seq > s_art_next ? -1 : b64Decode(data, bytes, sizeof(bytes));
  if (n <= 0 || s_art_got + n > s_art_len) {
    snprintf(body, sizeof(body), "{\"need\":%ld}", s_art_next);
    reply("QARTNAK", body);
    return;
  }
  memcpy(s_art + s_art_got, bytes, n);
  s_art_got += n;
  ++s_art_next;
  snprintf(body, sizeof(body), "{\"seq\":%ld}", seq);
  reply("QARTACK", body);
}

void handleQueueArtEnd() {
  seen();
  if (s_art == nullptr || s_art_got != s_art_len || s_art_k < 0 || s_art_k >= s_qn ||
      (s_art_crc != 0 && services::map::crc32(s_art, s_art_len) != s_art_crc)) {
    dropArt();
    reply("ERR", "{\"msg\":\"bad_art\"}");
    return;
  }
  QueueRow& r = s_q[s_art_k];
  if (r.art != nullptr) heap_caps_free(const_cast<uint8_t*>(r.art));
  r.art = s_art;
  r.art_len = s_art_len;
  s_art = nullptr;
  s_art_len = s_art_got = 0;
  s_art_k = -1;
  ++s_qversion;
  reply("OK", "{}");
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
