#include "services/serial_ota.h"

#include <Arduino.h>
#include <Update.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>

#include "hardware/display.h"
#include "ui/fonts_tr.h"
#include "ui/ui_scale.h"

namespace services::serial_ota {

namespace {

constexpr unsigned long kStallTimeoutMs = 15000;

bool s_active = false;
size_t s_len = 0;
size_t s_received = 0;
uint32_t s_expected_crc = 0;
uint32_t s_crc = 0xFFFFFFFFu;
uint32_t s_next_seq = 1;
unsigned long s_last_ms = 0;
int s_drawn_pct = -1;

void reply(const char* tag, const char* body) {
  char line[96];
  const int n = snprintf(line, sizeof(line), "PR:%s %s\n", tag, body);
  if (n > 0) Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
}

void replyAck(uint32_t seq) {
  char body[32];
  snprintf(body, sizeof(body), "{\"seq\":%lu}", static_cast<unsigned long>(seq));
  reply("OTAACK", body);
}

void replyError(const char* code) {
  char body[64];
  snprintf(body, sizeof(body), "{\"msg\":\"%s\"}", code);
  reply("ERR", body);
}

void crcUpdate(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    s_crc ^= p[i];
    for (int b = 0; b < 8; ++b) s_crc = (s_crc >> 1) ^ (0xEDB88320u & (0u - (s_crc & 1u)));
  }
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

void drawScreen(int pct) {
  if (pct == s_drawn_pct) return;
  const bool first = s_drawn_pct < 0;
  s_drawn_pct = pct;
  const int w = tft.width(), h = tft.height();
  if (first) {
    tft.fillScreen(TFT_BLACK);
    tft.setFont(&ui::font::SansBold18);
    tft.setTextSize(1);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Yazılım güncelleniyor", w / 2, h / 2 - ui::S(50));
    tft.setFont(&ui::font::Sans9);
    tft.setTextColor(tft.color565(125, 139, 150), TFT_BLACK);
    tft.drawString("USB kablosunu çıkarmayın", w / 2, h / 2 + ui::S(60));
    tft.drawRect(ui::S(60), h / 2 - ui::S(12), w - ui::S(120), ui::S(24), tft.color565(35, 72, 92));
  }
  const int bw = (w - ui::S(120) - 4) * pct / 100;
  tft.fillRect(ui::S(60) + 2, h / 2 - ui::S(12) + 2, bw, ui::S(24) - 4, tft.color565(91, 200, 245));
  char t[8];
  snprintf(t, sizeof(t), "%%%d", pct);
  tft.setFont(&ui::font::SansBold12);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextPadding(ui::S(90));
  tft.drawString(t, w / 2, h / 2 + ui::S(30));
  tft.setTextPadding(0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

namespace {
/** Unsigned number after "key": in a flat JSON object; false when missing. Parsed by
 *  hand (no heap): with Wi-Fi busy the JSON pool allocation could fail. */
bool jsonUint(const char* json, const char* key, uint32_t* out) {
  char pat[16];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (p == nullptr) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') ++p;
  char* end = nullptr;
  const long long v = strtoll(p, &end, 10);
  if (end == p) return false;
  *out = static_cast<uint32_t>(v);  // a negative CRC from an old page wraps correctly
  return true;
}
}  // namespace

void begin(const char* json) {
  uint32_t len = 0, crc = 0;
  if (!jsonUint(json, "len", &len) || !jsonUint(json, "crc", &crc)) {
    Serial.printf("ota: bad begin line '%.60s'\n", json);
    replyError("bad_json");
    return;
  }
  if (s_active) Update.abort();
  s_len = len;
  s_expected_crc = crc;
  if (s_len < 4096 || !Update.begin(s_len, U_FLASH)) {
    replyError("ota_no_space");
    s_active = false;
    return;
  }
  s_active = true;
  s_received = 0;
  s_crc = 0xFFFFFFFFu;
  s_next_seq = 1;
  s_last_ms = millis();
  s_drawn_pct = -1;
  drawScreen(0);
  replyAck(0);
}

void replyNak() {
  char body[32];
  snprintf(body, sizeof(body), "{\"seq\":%lu}", static_cast<unsigned long>(s_next_seq));
  reply("OTANAK", body);  // "send again from this chunk"
}

// "PR:OTA <seq> <len> <base64>" (v2) or "PR:OTA <seq> <base64>" (v1, no length check).
void chunk(char* args) {
  if (!s_active) {
    replyError("ota_not_started");
    return;
  }
  char* p1 = strchr(args, ' ');
  if (p1 == nullptr) {
    replyNak();
    return;
  }
  *p1++ = '\0';
  const uint32_t seq = strtoul(args, nullptr, 10);
  long want_len = -1;
  char* data = p1;
  char* p2 = strchr(p1, ' ');
  if (p2 != nullptr) {
    *p2++ = '\0';
    want_len = strtol(p1, nullptr, 10);
    data = p2;
  }
  s_last_ms = millis();
  if (seq + 1 == s_next_seq) {
    replyAck(seq);  // a repeat of the last chunk: already written
    return;
  }
  if (seq != s_next_seq) {
    replyNak();
    return;
  }
  static uint8_t buf[600];
  const int n = b64Decode(data, buf, sizeof(buf));
  if (n <= 0 || (want_len >= 0 && n != want_len) || s_received + static_cast<size_t>(n) > s_len) {
    replyNak();  // damaged line (bytes lost on the cable): ask for it again
    return;
  }
  if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
    abort("ota_write");
    return;
  }
  crcUpdate(buf, static_cast<size_t>(n));
  s_received += static_cast<size_t>(n);
  ++s_next_seq;
  replyAck(seq);
}

void end() {
  if (!s_active) {
    replyError("ota_not_started");
    return;
  }
  if (s_received != s_len || (~s_crc) != s_expected_crc) {
    abort("ota_crc");
    return;
  }
  if (!Update.end(true)) {
    Serial.printf("ota: %s\n", Update.errorString());
    s_active = false;
    replyError("ota_image");
    return;
  }
  s_active = false;
  drawScreen(100);
  reply("OTADONE", "{}");
  Serial.flush();
  delay(600);
  ESP.restart();
}

void abort(const char* reason) {
  if (s_active) Update.abort();
  s_active = false;
  s_drawn_pct = -1;
  if (reason != nullptr) replyError(reason);
}

bool active() { return s_active; }

void tick() {
  if (!s_active) return;
  if (millis() - s_last_ms > kStallTimeoutMs) {
    Serial.println("ota: transfer stalled, cancelled");
    abort("ota_timeout");
    ESP.restart();  // back to the normal screens with a clean state
  }
  if (s_len > 0) drawScreen(static_cast<int>(s_received * 100 / s_len));
}

}  // namespace services::serial_ota
