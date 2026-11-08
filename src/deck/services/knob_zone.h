#pragma once
// The microphone half of the volume knob (stage 10q). No Arduino here:
// tools/host-tests/knob_zone_test.cpp tests it.

namespace services::knob {

/** The helper's "mic" field: 0 none, 1 open, 2 muted. Anything else counts as none. */
inline int parseMic(int v) { return v == 1 || v == 2 ? v : 0; }

/** A tap at dx, dy from the knob's centre belongs to the microphone: lower half of the face
 * (radius face_r, inside the ring), when there is one. */
inline bool tapIsMic(int dx, int dy, int face_r, int mic_state) {
  return mic_state != 0 && dy >= 0 && dx * dx + dy * dy < face_r * face_r;
}

}  // namespace services::knob
