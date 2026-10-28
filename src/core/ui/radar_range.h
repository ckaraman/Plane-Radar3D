#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets for the 3D view. outer_km is the radius of the ground disc
 * (outer ring); the other rings sit at 20 % and 50 % of it. ring3_km keeps the
 * old field name (label of the outer ring) so the shared setup code compiles.
 */
struct RangePreset {
  float ring3_km;
  float outer_km;
};

constexpr RangePreset kRangePresets[] = {
    {15.0f, 15.0f},
    {25.0f, 25.0f},
    {35.0f, 35.0f},
    {50.0f, 50.0f},
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
/** One preset smaller (dir < 0) or larger (dir > 0), no wrap-around; saved. Returns
 *  false at either end. */
bool rangeStep(int dir);
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool useMiles();
bool showRunways();
/** Radar rings and cross lines; off = a plain map view. */
bool showRings();
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void saveRingsFromPortal(const char* checkbox_value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset distance units to km (e.g. with WiFi credential wipe). */
void unitsReset();

}  // namespace ui::radar
