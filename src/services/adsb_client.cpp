#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

#include "config.h"
#include "services/http_body_framing.h"

namespace services::adsb {

namespace {

// Data sources, tried in turn when one fails (same readsb JSON, "ac" array).
struct Source {
  const char* name;
  const char* prefix;   // + lat
  const char* mid;      // + lon
  const char* suffix;   // + radius (nm)
};
constexpr Source kSources[] = {
    {"adsb.fi", "https://opendata.adsb.fi/api/v3/lat/", "/lon/", "/dist/"},
    {"airplanes.live", "https://api.airplanes.live/v2/point/", "/", "/"},
    {"adsb.lol", "https://api.adsb.lol/v2/point/", "/", "/"},
};
constexpr int kSourceCount = sizeof(kSources) / sizeof(kSources[0]);
int s_source = 0;
// Per source: does plain HTTP work? 0 = not tried yet, 1 = yes, 2 = no (redirects to
// HTTPS or refuses). Plain HTTP needs no TLS session (~40 KB of heap), so it is tried
// first; the answer is remembered until the next restart.
uint8_t s_plain_state[3] = {0, 0, 0};
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectTimeoutMs = 5000;  // TLS handshake needs room
constexpr unsigned long kRequestTimeoutMs = 6000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
char s_error[40] = "";  // why the last fetch failed (shown while no data has arrived)

void setError(const char* fmt, int value) {
  snprintf(s_error, sizeof(s_error), fmt, value);
}

/** Publish parsed aircraft to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  for (size_t i = 0; i < count; ++i) {
    s_aircraft[i] = src[i];
  }
  s_aircraft_count = count;
  s_last_update_ms = millis();  // base time for dead-reckoning
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  // One attempt: a failed connection (DNS, TCP or the TLS handshake) comes back as
  // -1 straight away. Retrying it in a loop only re-ran the expensive handshake and
  // then reported a misleading timeout (-11).
  http.setConnectTimeout(kConnectTimeoutMs);
  pollNetwork();
  return http.GET();
}

/**
 * Pumps the socket one byte at a time, in blocks, without buffering the whole
 * response.
 *
 * Each refill runs the network poll callback, which is why HTTPClient's own
 * body readers (getString/writeToStream) can't be used here -- they block
 * without giving the fetch task a chance to poll. That also means they can't
 * de-chunk for us, so the wire image comes out raw and BodyFramer unwraps it.
 *
 * Where the body ends is BodyFramer's business, not this class's: it stops
 * pulling at the right byte. Reading a little past that point into buffer_ is
 * harmless while the connection is torn down after every fetch.
 */
class PollingSocketSource {
 public:
  PollingSocketSource(HTTPClient& http, WiFiClient& stream,
                      unsigned long deadline)
      : http_(&http), stream_(&stream), deadline_(deadline) {}

  /** Next raw byte, or -1 once the socket closes or the deadline passes. */
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    while (millis() < deadline_) {
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        const int to_read = available > static_cast<int>(sizeof(buffer_))
                                ? static_cast<int>(sizeof(buffer_))
                                : available;
        const int read_bytes = stream_->readBytes(buffer_, to_read);
        if (read_bytes > 0) {
          len_ = static_cast<size_t>(read_bytes);
          return true;
        }
      }
      if (!http_->connected() && stream_->available() <= 0) {
        break;  // server closed and the socket is drained
      }
      delay(1);
    }
    return false;
  }

  HTTPClient* http_;
  WiFiClient* stream_;
  unsigned long deadline_;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
};

using BodyReader = services::http::BodyFramer<PollingSocketSource>;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    const int ft = static_cast<int>(lroundf(alt));
    if (ft >= 1000 || ft <= -1000) {
      // 1.2K ft / 35K ft: keeps the tag short; one decimal only when it is not .0
      const int tenths = static_cast<int>(lroundf(static_cast<float>(ft) / 100.0f));
      const int whole = tenths / 10;
      const int dec = (tenths < 0 ? -tenths : tenths) % 10;
      if (dec == 0) {
        snprintf(out, out_len, "%dK ft", whole);
      } else {
        snprintf(out, out_len, "%s%d.%dK ft", tenths < 0 && whole == 0 ? "-" : "", whole, dec);
      }
    } else {
      snprintf(out, out_len, "%d ft", ft);
    }
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

void publishExternal(const Aircraft* src, size_t count) {
  publish(src, count < kMaxAircraft ? count : kMaxAircraft);
}

size_t aircraftCount() { return s_aircraft_count; }

size_t countWithin(double lat0, double lon0, float radius_km) {
  const double kx = 111.0 * cos(lat0 * 0.01745329252), r2 = static_cast<double>(radius_km) * radius_km;
  if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
  size_t in = 0;
  for (size_t i = 0; i < s_aircraft_count; ++i) {
    const double dx = (s_aircraft[i].lon - lon0) * kx, dy = (s_aircraft[i].lat - lat0) * 111.0;
    if (dx * dx + dy * dy <= r2) ++in;
  }
  if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
  return in;
}

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

const char* sourceName() { return kSources[s_source].name; }

const char* lastError() { return s_error; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  const Source& src = kSources[s_source];
  String url = src.prefix;
  url += String(center_lat, 6);
  url += src.mid;
  url += String(center_lon, 6);
  url += src.suffix;
  url += String(dist_nm, 1);
  const bool try_plain = s_plain_state[s_source] != 2;
  if (try_plain) url = String("http") + String(url.c_str() + 5);  // "https://" -> "http://"
  // Any failure below moves on to the next source for the next cycle. A fallback
  // is only a stand-in: the networks have different receivers, so after about a
  // minute on one, go back and try adsb.fi (the main source, same as the other
  // radar) again instead of staying on the fallback for good.
  struct NextOnFail {
    bool ok = false;
    ~NextOnFail() {
      static int fallback_ok = 0;
      if (!ok) {
        s_source = (s_source + 1) % kSourceCount;
        fallback_ok = 0;
      } else if (s_source != 0 && ++fallback_ok >= 12) {
        s_source = 0;
        fallback_ok = 0;
      }
    }
  } next_on_fail;

  // Only these fields of each aircraft reach RAM.
  JsonDocument filter;
  for (const char* key :
       {"lat", "lon", "true_heading", "track", "gs", "alt_baro", "alt_geom", "seen_pos",
        "flight", "hex", "t", "baro_rate", "geom_rate"}) {
    filter[key] = true;
  }

  const unsigned heap_before = ESP.getFreeHeap();
  const unsigned block_before = ESP.getMaxAllocHeap();
  WiFiClient plain;
  WiFiClientSecure client;
  if (!try_plain) client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!(try_plain ? http.begin(plain, url) : http.begin(client, url))) {
    Serial.println("adsb: http.begin failed");
    setError("bağlantı kurulamadı", 0);
    return false;
  }

  // HTTPClient only records Transfer-Encoding in the collected headers when
  // it is asked for up front; _transferEncoding itself is private.
  static const char* kWantedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kWantedHeaders, 1);

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    char tls[64] = "";
    int tls_code = 0;
    if (!try_plain) tls_code = client.lastError(tls, sizeof(tls));
    Serial.printf("adsb: %s %s HTTP %d (heap before %u, largest block %u) %s\n", src.name,
                  try_plain ? "http" : "https", code, heap_before, block_before, tls);
    http.end();
    if (try_plain && s_plain_state[s_source] == 0) {
      // Plain HTTP not offered here (redirect, refusal...): use HTTPS for this source
      // from now on, and try it right away on the next cycle (no source switch).
      s_plain_state[s_source] = 2;
      next_on_fail.ok = true;
      return false;
    }
    char msg[40];
    if (code < 0 && tls_code != 0) {
      snprintf(msg, sizeof(msg), "TLS %d, bellek %u/%u KB", tls_code, block_before / 1024,
               heap_before / 1024);
    } else if (code < 0) {
      snprintf(msg, sizeof(msg), "bağlantı %d, bellek %u/%u KB", code, block_before / 1024,
               heap_before / 1024);
    } else {
      snprintf(msg, sizeof(msg), "sunucu yanıtı %d", code);
    }
    snprintf(s_error, sizeof(s_error), "%s", msg);
    return false;
  }
  if (try_plain) s_plain_state[s_source] = 1;

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no response stream");
    http.end();
    return false;
  }

  // On HTTP/1.1 the CDN answers with Transfer-Encoding: chunked, and
  // getStreamPtr() hands back the raw socket -- chunk sizes and all. BodyFramer
  // strips that framing back off.
  const services::http::BodyFraming framing =
      http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
          ? services::http::BodyFraming::kChunked
          : services::http::BodyFraming::kIdentity;

  PollingSocketSource source(http, *stream, millis() + kRequestTimeoutMs);
  BodyReader body(source, framing, http.getSize());

  // The response can list well over a hundred aircraft near a busy airport. Instead
  // of loading the whole "ac" array into one document (20-30 KB on top of the ~40 KB
  // TLS connection), walk the array and parse one aircraft at a time into a small
  // document: memory use no longer grows with the traffic.
  //
  // The feed lists aircraft in no particular order, so keep the NEAREST kMaxAircraft
  // (a full buffer swaps its farthest entry for a closer newcomer).
  Aircraft parsed[kMaxAircraft];
  float dist2[kMaxAircraft];
  size_t n = 0;
  size_t total = 0;
  const float kx = 111.0f * cosf(static_cast<float>(center_lat) * 0.01745329252f);

  auto nextNonSpace = [&]() {
    int ch;
    do {
      ch = body.read();
    } while (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
    return ch;
  };
  // Find "ac" followed by ':' and '['.
  bool found = false;
  {
    static const char kKey[] = "\"ac\"";
    size_t matched = 0;
    int ch;
    while (!found && (ch = body.read()) >= 0) {
      matched = (ch == kKey[matched]) ? matched + 1 : (ch == kKey[0] ? 1 : 0);
      if (matched == sizeof(kKey) - 1) {
        matched = 0;
        if (nextNonSpace() == ':' && nextNonSpace() == '[') found = true;
      }
    }
  }
  bool ok = found;
  bool reported = false;  // a specific reason was already set for this fetch
  if (!found) {
    reported = true;
    Serial.println(body.bytesRead() == 0 ? "adsb: empty response" : "adsb: no aircraft list");
    setError(body.bytesRead() == 0 ? "boş yanıt" : "beklenmeyen yanıt", 0);
  }
  JsonDocument doc;
  while (ok) {
    // Peek: ']' ends the list (empty sky), anything else starts an object.
    int ch = nextNonSpace();
    if (ch == ']') break;
    if (ch != '{') {
      ok = false;
      break;
    }
    // Re-feed the '{' we consumed, then let ArduinoJson read the rest of the object.
    struct Prefixed {
      BodyReader* r;
      bool sent = false;
      int read() {
        if (!sent) {
          sent = true;
          return '{';
        }
        return r->read();
      }
      size_t readBytes(char* buf, size_t len) {
        size_t k = 0;
        while (k < len) {
          const int c = read();
          if (c < 0) break;
          buf[k++] = static_cast<char>(c);
        }
        return k;
      }
    } in{&body};
    const DeserializationError err = deserializeJson(doc, in, DeserializationOption::Filter(filter));
    if (err) {
      Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
      setError(err == DeserializationError::NoMemory ? "bellek yetmedi" : "veri okunamadı", 0);
      reported = true;
      ok = false;
      break;
    }
    JsonObject plane = doc.as<JsonObject>();
    do {
      if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) break;
      if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) break;
      ++total;
      const float lat = plane["lat"].as<float>();
      const float lon = plane["lon"].as<float>();
      const float dx = (lon - static_cast<float>(center_lon)) * kx;
      const float dy = (lat - static_cast<float>(center_lat)) * 111.0f;
      const float d2 = dx * dx + dy * dy;
      size_t slot = n;
      if (n >= kMaxAircraft) {
        size_t far = 0;
        for (size_t k = 1; k < n; ++k) {
          if (dist2[k] > dist2[far]) far = k;
        }
        if (d2 >= dist2[far]) break;
        slot = far;
      } else {
        ++n;
      }
      dist2[slot] = d2;
      Aircraft& a = parsed[slot];
      a.lat = lat;
      a.lon = lon;
      a.nose_deg = pickNoseHeading(plane);
      a.track_deg = pickTrackHeading(plane);
      a.gs_knots = pickGroundSpeed(plane);
      // seen_pos: seconds since this position was measured. Use it as the
      // dead-reckoning age offset, capped so a very stale fix isn't flung far.
      float seen_pos = 0.0f;
      readJsonFloat(plane, "seen_pos", &seen_pos);
      if (seen_pos < 0.0f) seen_pos = 0.0f;
      if (seen_pos > 30.0f) seen_pos = 30.0f;
      a.pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);
      float alt = 0.0f;
      if (isOnGround(plane)) {
        a.alt_ft = 0;
      } else if (readJsonFloat(plane, "alt_baro", &alt) || readJsonFloat(plane, "alt_geom", &alt)) {
        a.alt_ft = static_cast<int32_t>(lroundf(alt));
      } else {
        a.alt_ft = 0;
      }
      float rate = 0.0f;
      if (readJsonFloat(plane, "baro_rate", &rate) || readJsonFloat(plane, "geom_rate", &rate)) {
        if (rate > 32000.0f) rate = 32000.0f;
        if (rate < -32000.0f) rate = -32000.0f;
        a.vrate_fpm = static_cast<int16_t>(lroundf(rate));
      } else {
        a.vrate_fpm = 0;
      }
      fillTagFields(&a, plane);
    } while (false);
    // Between objects: ',' continues, ']' ends.
    ch = nextNonSpace();
    if (ch == ']') break;
    if (ch != ',') ok = false;
  }
  // Read off the rest of the message, so the socket sits at its end.
  body.drain();
  http.end();
  if (!ok) {
    if (body.framingError()) Serial.println("adsb: malformed chunked body");
    if (!reported) setError("veri yarıda kesildi", 0);
    return false;
  }
  s_error[0] = '\0';

  publish(parsed, n);
  if (total > n) {
    Serial.printf("adsb: %u aircraft (nearest of %u)\n", static_cast<unsigned>(n),
                  static_cast<unsigned>(total));
  } else {
    Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  }
  next_on_fail.ok = true;
  return true;
}

}  // namespace services::adsb
