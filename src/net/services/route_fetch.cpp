// Wi-Fi route lookup (system 1): adsbdb.com over HTTPS, stored in the cache in
// src/core/services/route_service.cpp.
#include "services/route_service.h"
#include "hardware/console.h"

#include "services/adsb_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include "services/tls_client.h"

#include <ArduinoJson.h>

#include <cstring>

namespace services::route {

namespace {

void copyCode(JsonObjectConst airport, char* out, size_t n) {
  const char* iata = airport["iata_code"] | "";
  const char* icao = airport["icao_code"] | "";
  strlcpy(out, iata[0] ? iata : icao, n);
}

}  // namespace

void fetchPending() {
  if (WiFi.status() != WL_CONNECTED) return;
  char callsign[9];
  pendingCallsign(callsign, sizeof(callsign));
  if (callsign[0] == '\0') return;

  String url = "https://api.adsbdb.com/v0/callsign/";
  url += callsign;
  // Only one TLS session fits in the heap: close the aircraft feed's kept-open one
  // (it reconnects on its next fetch).
  services::adsb::releaseConnection();
  services::tls::Client client;
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
    Console.printf("route: HTTP %d for %s (free heap %u, largest block %u)\n", code, callsign,
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
  Console.printf("route: %s %s -> %s\n", callsign, r.from_code, r.to_code);
}

}  // namespace services::route
