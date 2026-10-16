#pragma once

#include <cstddef>

namespace services::usb_feed {

/**
 * Aircraft data sent from a computer over the USB cable, for running the
 * display without Wi-Fi (no antenna yet, or a test bench). The PC page
 * (tools/usb-koprusu.html) downloads the traffic and sends, every few seconds:
 *
 *   PR:ABEGIN
 *   PR:A lat,lon,track_deg,gs_kt,alt_ft,vrate_fpm,callsign,type[,from,from_city,to,to_city]
 *        (one per aircraft; route fields optional, "-" in "from" = no route known)
 *   PR:AEND <unix_time>            -> PR:AOK {"n":count}
 *
 * The batch replaces the aircraft list and sets the clock from unix_time.
 */
void begin();
void add(const char* args);
void end(const char* args);

/** True while batches keep arriving (last one less than 20 s ago). */
bool active();

}  // namespace services::usb_feed
