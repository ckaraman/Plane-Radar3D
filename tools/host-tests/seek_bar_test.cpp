// Host test for src/deck/services/seek_bar.h (stage 10n): where a finger on the time bar
// seeks to, and how long the card keeps showing that place. Build and run (PC or CI):
//   g++ -std=gnu++17 -O2 -Wall -Isrc/deck tools/host-tests/seek_bar_test.cpp -o seek_bar_test
//   ./seek_bar_test
#include <cstdint>
#include <cstdio>

#include "services/seek_bar.h"

using namespace services::seek;

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
  check(seekTarget(100, 100, 200, 240) == 0, "left end");
  check(seekTarget(300, 100, 200, 240) == 240000, "right end");
  check(seekTarget(200, 100, 200, 240) == 120000, "middle");
  check(seekTarget(40, 100, 200, 240) == 0, "left of bar clamps");
  check(seekTarget(900, 100, 200, 240) == 240000, "right of bar clamps");
  check(seekTarget(200, 100, 200, 0) == -1, "no duration: no seek");
  check(seekTarget(200, 100, 0, 240) == -1, "disabled bar");
  check(seekTarget(101, 100, 200, 7200) == 36000, "long song, one pixel");

  Hold h;
  check(holdShows(h, 30000, 1000, 7) == -1, "no hold: nothing held");
  holdStart(h, 90000, 1000, 7);
  check(holdShows(h, 30000, 1500, 7) == 90000, "old report: target shown");
  check(holdShows(h, 89000, 1600, 7) == -1 && h.target_ms == -1, "report near: hold ends");
  holdStart(h, 90000, 1000, 7);
  check(holdShows(h, 30000, 4000, 7) == -1, "3 s: hold ends");
  holdStart(h, 90000, 1000, 7);
  check(holdShows(h, 30000, 1200, 8) == -1, "new track: hold ends");
  holdStart(h, 90000, 0xFFFFFC00u, 7);  // 1024 ms before millis() wraps
  check(holdShows(h, 30000, 0x00000100u, 7) == 90000, "millis wraps: still held (1.3 s)");

  if (failures == 0) std::printf("seek bar ok\n");
  return failures;
}
