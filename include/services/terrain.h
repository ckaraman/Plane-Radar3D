#pragma once

#include <cstdint>

namespace services::terrain {

/**
 * Ground elevation around the radar centre, for the 3D terrain mesh.
 *
 * The setup page downloads an n x n grid of heights (metres, Open-Meteo
 * elevation API) over a square of side 2 * half_km centred on lat/lon and sends it
 * over USB; it is kept in NVS. Rows go south -> north, columns west -> east.
 *
 *   PR:ELEVBEGIN {"n":25,"half":22.6,"lat":41.0,"lon":29.0} -> PR:ELEVACK {"row":-1}
 *   PR:ELEVROW <i> h0,h1,...,h(n-1)                         -> PR:ELEVACK {"row":i}
 *   PR:ELEVEND                                              -> PR:OK {} | PR:ERR
 *   PR:ELEVCLEAR                                            -> PR:OK {}
 */
constexpr int kMaxN = 33;
/** Reported in HELLO as "elevv" so the page knows it can send a grid. */
constexpr int kProtocolVersion = 1;

void init();
bool available();
/** Changes whenever the grid is replaced or removed. */
uint32_t generation();

/** Highest point of the loaded grid in metres (0 without data). */
float maxHeightM();

/** Height in metres at x km east / y km north of the radar centre (0 outside the grid;
 *  fades out over the outer part of the grid's inscribed circle). */
float heightM(float x_km, float y_km);

// USB setup handlers (called from serial_setup).
void handleBegin(const char* json);
void handleRow(char* args);
void handleEnd();
void handleClear();
void clear();

}  // namespace services::terrain
