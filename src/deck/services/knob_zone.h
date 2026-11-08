#pragma once
// The microphone half of the volume knob (stage 10q). No Arduino here:
// tools/host-tests/knob_zone_test.cpp tests it.

namespace services::knob {

/** The helper's "mic" field: 0 none, 1 open, 2 muted. Anything else counts as none. */
inline int parseMic(int v) { return v == 1 || v == 2 ? v : 0; }

/** A tap at y belongs to the microphone (lower half), when there is one. */
inline bool tapIsMic(int y, int cy, int mic_state) { return mic_state != 0 && y >= cy; }

}  // namespace services::knob
