// Host test for src/deck/services/queue_carry.h (stage 10b-3): a new queue keeps the row
// pictures the card already has. Build and run (PC or CI):
//   g++ -std=gnu++17 -O2 -Wall -Isrc/deck tools/host-tests/queue_carry_test.cpp -o queue_carry_test
//   ./queue_carry_test
#include <cstdint>
#include <cstdio>

#include "services/queue_carry.h"

using services::host::carryArts;

namespace {

struct Row {
  const uint8_t* art;
  size_t art_len;
};

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  const uint8_t a[1] = {}, b[2] = {}, c[3] = {};

  {
    Row old_rows[3] = {{a, 1}, {b, 2}, {c, 3}};
    Row new_rows[3] = {};
    const int16_t from[3] = {1, -1, 0};
    carryArts(old_rows, 3, new_rows, 3, from);
    check(new_rows[0].art == b && new_rows[0].art_len == 2, "new 0 = old 1");
    check(new_rows[1].art == nullptr && new_rows[1].art_len == 0, "new 1 none");
    check(new_rows[2].art == a && new_rows[2].art_len == 1, "new 2 = old 0");
    check(old_rows[0].art == nullptr && old_rows[0].art_len == 0, "old 0 moved");
    check(old_rows[1].art == nullptr && old_rows[1].art_len == 0, "old 1 moved");
    check(old_rows[2].art == c && old_rows[2].art_len == 3, "old 2 stays");
  }
  {
    Row old_rows[1] = {{a, 1}};
    Row new_rows[2] = {};
    const int16_t from[2] = {0, 0};
    carryArts(old_rows, 1, new_rows, 2, from);
    check(new_rows[0].art == a, "first gets it");
    check(new_rows[1].art == nullptr, "second gets nothing");
  }
  {
    Row old_rows[2] = {{a, 1}, {b, 2}};
    Row new_rows[2] = {};
    const int16_t from[2] = {5, -3};
    carryArts(old_rows, 2, new_rows, 2, from);
    check(new_rows[0].art == nullptr && new_rows[1].art == nullptr, "out of range: nothing");
    check(old_rows[0].art == a && old_rows[1].art == b, "out of range: old stay");
  }

  if (failures) return 1;
  std::printf("queue carry ok\n");
  return 0;
}
