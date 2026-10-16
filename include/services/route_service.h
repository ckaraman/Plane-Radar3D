#pragma once

#include <cstddef>

namespace services::route {

/**
 * Where an aircraft flies from and to, looked up by callsign.
 *
 * ADS-B carries no route, so it comes from adsbdb.com (callsign -> scheduled
 * route). With Wi-Fi the device asks itself (fetchPending, on the network task);
 * in USB mode the PC bridge looks it up and sends it with the aircraft.
 * Routes are by callsign, so they can be missing or out of date for some flights.
 */
struct Route {
  char from_code[5];   // IATA (else ICAO) code, e.g. "IST"
  char to_code[5];
  char from_city[20];  // ASCII, e.g. "Istanbul"
  char to_city[20];
};

enum class Status { Unknown, Pending, Found, NotFound };

void init();

/** Card asks for a callsign. Found -> *out filled. Pending marks it for lookup. */
Status get(const char* callsign, Route* out);

/** Store a result (from the network task or the USB feed). */
void put(const char* callsign, const Route* route);  // nullptr = no route known

/** Network task: look up the most recently requested callsign over HTTPS. */
void fetchPending();

/** UTF-8 to plain ASCII for the bitmap fonts (Türkçe harfler -> en yakın ASCII). */
void foldAscii(const char* in, char* out, size_t out_len);

}  // namespace services::route
