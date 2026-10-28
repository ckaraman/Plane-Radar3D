#pragma once

#include <cstddef>

#include "services/weather_parse.h"

namespace services::weather {

/** Load the on/off setting from NVS (default: on). */
void init();

bool enabled();

/** Setup page value: "T" = on, anything else = off. Persists. */
void saveFromPortal(const char* enabled_checkbox);

/** Restore defaults (called with the Wi-Fi/location reset). */
void clear();

/** Latest forecast (thread-safe copy). False until the first successful fetch. */
bool snapshot(Forecast* out, unsigned long* fetched_ms);

/**
 * Fetch from Open-Meteo when the data is old, failed recently-enough ago to retry,
 * or the location changed. Blocking (~1-2 s of HTTPS); call it from the network
 * task, never from the render loop. Does nothing when the feature is off.
 */
void fetchIfDue(double lat, double lon);

/** Why the last Wi-Fi fetch failed (Turkish, for the screen); false when it did not. */
bool lastError(char* out, size_t out_len);

/** USB mode: "PR:W ..." line from the PC bridge (format: weather_parse.h). */
void handleBridge(const char* args);

}  // namespace services::weather
