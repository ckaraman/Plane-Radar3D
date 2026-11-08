// Host test for src/deck/services/knob_zone.h (stage 10q): the helper's mic value and which
// half of the knob a tap belongs to. Build and run (PC or CI):
//   g++ -std=gnu++17 -O2 -Wall -Isrc/deck tools/host-tests/knob_zone_test.cpp -o knob_zone_test
//   ./knob_zone_test
#include <cstdio>

#include "services/knob_zone.h"

using namespace services::knob;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  check(parseMic(0) == 0, "none stays none");
  check(parseMic(1) == 1, "open stays open");
  check(parseMic(2) == 2, "muted stays muted");
  check(parseMic(9) == 0, "unknown value is none");
  check(parseMic(-1) == 0, "negative value is none");

  // dx, dy from the knob's centre; the face (inside the ring) has radius 120.
  check(!tapIsMic(0, -60, 120, 1), "upper half is the speaker");
  check(tapIsMic(0, 0, 120, 1), "the line itself is the microphone");
  check(tapIsMic(0, 60, 120, 2), "lower half is the microphone while muted");
  check(!tapIsMic(0, 60, 120, 0), "without a microphone the whole knob is the speaker");
  check(!tapIsMic(0, 130, 120, 1), "the ring's lower half stays the speaker");
  check(!tapIsMic(90, 90, 120, 1), "the corner outside the face stays the speaker");

  if (failures == 0) std::printf("knob_zone ok\n");
  return failures != 0;
}
