#include "services/route_service.h"

#include "services/adsb_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace services::route {

namespace {

constexpr size_t kCacheSize = 32;
constexpr unsigned long kNotFoundRetryMs = 30UL * 60UL * 1000UL;  // ask again after 30 min

struct Entry {
  char callsign[9];
  bool found;
  unsigned long stored_ms;
  Route route;
};

Entry s_cache[kCacheSize];
size_t s_next = 0;  // ring buffer position (oldest entry is replaced)
char s_pending[9] = "";
SemaphoreHandle_t s_mutex = nullptr;

void lock() {
  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}
void unlock() {
  if (s_mutex) xSemaphoreGive(s_mutex);
}

Entry* findLocked(const char* callsign) {
  for (Entry& e : s_cache) {
    if (e.callsign[0] != '\0' && strcmp(e.callsign, callsign) == 0) return &e;
  }
  return nullptr;
}

/** Decode one UTF-8 code point; returns bytes used (0 at end). */
int decodeUtf8(const unsigned char* s, uint32_t* cp) {
  if (s[0] == 0) return 0;
  if (s[0] < 0x80) { *cp = s[0]; return 1; }
  if ((s[0] & 0xE0) == 0xC0 && s[1]) { *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
  if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
    *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return 3;
  }
  *cp = '?';
  int n = 1;
  while (s[n] && (s[n] & 0xC0) == 0x80) ++n;
  return n;
}

char foldCodePoint(uint32_t cp) {
  if (cp < 0x80) return static_cast<char>(cp);
  switch (cp) {
    case 0x011E: return 'G';  case 0x011F: return 'g';  // Ğ ğ
    case 0x0130: return 'I';  case 0x0131: return 'i';  // İ ı
    case 0x015E: return 'S';  case 0x015F: return 's';  // Ş ş
    case 0x00C7: return 'C';  case 0x00E7: return 'c';  // Ç ç
    case 0x00D6: return 'O';  case 0x00F6: return 'o';  // Ö ö
    case 0x00DC: return 'U';  case 0x00FC: return 'u';  // Ü ü
    case 0x0141: return 'L';  case 0x0142: return 'l';
    case 0x00D1: return 'N';  case 0x00F1: return 'n';
    case 0x00DF: return 's';
    default: break;
  }
  if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
  if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';
  if (cp >= 0x00C8 && cp <= 0x00CB) return 'E';
  if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
  if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
  if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
  if ((cp >= 0x00D2 && cp <= 0x00D5) || cp == 0x00D8) return 'O';
  if ((cp >= 0x00F2 && cp <= 0x00F5) || cp == 0x00F8) return 'o';
  if (cp >= 0x00D9 && cp <= 0x00DB) return 'U';
  if (cp >= 0x00F9 && cp <= 0x00FB) return 'u';
  return 0;  // drop
}

void copyCode(JsonObjectConst airport, char* out, size_t n) {
  const char* iata = airport["iata_code"] | "";
  const char* icao = airport["icao_code"] | "";
  strlcpy(out, iata[0] ? iata : icao, n);
}

}  // namespace

bool isTurkishLetter(uint32_t cp) {
  switch (cp) {
    case 0x00C7: case 0x00E7: case 0x00D6: case 0x00F6: case 0x00DC: case 0x00FC:
    case 0x011E: case 0x011F: case 0x0130: case 0x0131: case 0x015E: case 0x015F:
      return true;
    default:
      return false;
  }
}

void foldAscii(const char* in, char* out, size_t out_len) {
  if (out_len == 0) return;
  size_t o = 0;
  const unsigned char* s = reinterpret_cast<const unsigned char*>(in ? in : "");
  while (o + 1 < out_len) {
    uint32_t cp = 0;
    const int used = decodeUtf8(s, &cp);
    if (used == 0) break;
    s += used;
    if (isTurkishLetter(cp)) {  // the display fonts have these: keep them (2 bytes)
      if (o + 2 >= out_len) break;
      out[o++] = static_cast<char>(0xC0 | (cp >> 6));
      out[o++] = static_cast<char>(0x80 | (cp & 0x3F));
      continue;
    }
    const char c = foldCodePoint(cp);
    if (c == 0 || c == ',') continue;
    out[o++] = c;
  }
  out[o] = '\0';
}

void init() {
  if (s_mutex == nullptr) s_mutex = xSemaphoreCreateMutex();
  memset(s_cache, 0, sizeof(s_cache));
}

Status get(const char* callsign, Route* out) {
  if (callsign == nullptr || callsign[0] == '\0') return Status::NotFound;
  lock();
  Status st = Status::Unknown;
  Entry* e = findLocked(callsign);
  if (e != nullptr) {
    if (e->found) {
      if (out) *out = e->route;
      st = Status::Found;
    } else if (millis() - e->stored_ms < kNotFoundRetryMs) {
      st = Status::NotFound;
    }
  }
  if (st == Status::Unknown) {
    strlcpy(s_pending, callsign, sizeof(s_pending));
    st = Status::Pending;
  }
  unlock();
  return st;
}

void put(const char* callsign, const Route* route) {
  if (callsign == nullptr || callsign[0] == '\0') return;
  lock();
  Entry* e = findLocked(callsign);
  if (e == nullptr) {
    e = &s_cache[s_next];
    s_next = (s_next + 1) % kCacheSize;
    memset(e, 0, sizeof(*e));
    strlcpy(e->callsign, callsign, sizeof(e->callsign));
  }
  e->found = route != nullptr;
  e->stored_ms = millis();
  if (route) e->route = *route;
  if (strcmp(s_pending, callsign) == 0) s_pending[0] = '\0';
  unlock();
}

bool hasPending() {
  lock();
  const bool p = s_pending[0] != '\0';
  unlock();
  return p;
}

void fetchPending() {
  if (WiFi.status() != WL_CONNECTED) return;
  char callsign[9];
  lock();
  strlcpy(callsign, s_pending, sizeof(callsign));
  unlock();
  if (callsign[0] == '\0') return;

  String url = "https://api.adsbdb.com/v0/callsign/";
  url += callsign;
  // Only one TLS session fits in the heap: close the aircraft feed's kept-open one
  // (it reconnects on its next fetch).
  services::adsb::releaseConnection();
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8);
  HTTPClient http;
  if (!http.begin(client, url)) return;
  http.setConnectTimeout(5000);
  http.setTimeout(6000);
  http.setUserAgent("plane-radar-3d");
  http.useHTTP10(true);  // plain body: the JSON parser reads the stream directly
  const int code = http.GET();
  if (code == 404) {  // adsbdb answers 404 for callsigns it does not know
    http.end();
    put(callsign, nullptr);
    return;
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    Serial.printf("route: HTTP %d for %s (free heap %u, largest block %u)\n", code, callsign,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
    // Try again on the next cycles; after a few failures leave it (asked again in 30 min)
    // so the card does not say "searching" forever.
    static char s_fail_cs[9] = "";
    static int s_fails = 0;
    if (strcmp(s_fail_cs, callsign) != 0) {
      strlcpy(s_fail_cs, callsign, sizeof(s_fail_cs));
      s_fails = 0;
    }
    if (++s_fails >= 3) {
      s_fails = 0;
      put(callsign, nullptr);
    }
    return;
  }
  JsonDocument filter;
  JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
  for (const char* side : {"origin", "destination"}) {
    JsonObject a = fr[side].to<JsonObject>();
    a["iata_code"] = true;
    a["icao_code"] = true;
    a["municipality"] = true;
  }
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  JsonObjectConst route = doc["response"]["flightroute"];
  if (err || route.isNull()) {
    put(callsign, nullptr);
    return;
  }
  Route r{};
  copyCode(route["origin"], r.from_code, sizeof(r.from_code));
  copyCode(route["destination"], r.to_code, sizeof(r.to_code));
  foldAscii(route["origin"]["municipality"] | "", r.from_city, sizeof(r.from_city));
  foldAscii(route["destination"]["municipality"] | "", r.to_city, sizeof(r.to_city));
  put(callsign, (r.from_code[0] || r.to_code[0]) ? &r : nullptr);
  Serial.printf("route: %s %s -> %s\n", callsign, r.from_code, r.to_code);
}

}  // namespace services::route
