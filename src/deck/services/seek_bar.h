#pragma once
// Seeking on the time bar (stage 10n): where a finger seeks to, and the "hold" that keeps
// showing the chosen place until the helper's reports agree (Windows may report the old
// position for a moment). No Arduino here: tools/host-tests/seek_bar_test.cpp tests it.
#include <cstdint>

namespace services::seek {

constexpr uint32_t kSeekHoldMs = 3000;  // at most this long
constexpr uint32_t kSeekNearMs = 2000;  // a report this close ends it

/** The song position (ms) under finger x on a bar from bar_x, bar_w wide; clamped to the
 * song. -1: nothing to seek (no duration, no bar). */
inline int32_t seekTarget(int x, int bar_x, int bar_w, uint32_t duration_s) {
  if (duration_s == 0 || bar_w <= 0) return -1;
  int dx = x - bar_x;
  if (dx < 0) dx = 0;
  if (dx > bar_w) dx = bar_w;
  const int64_t dur_ms = static_cast<int64_t>(duration_s) * 1000;
  return static_cast<int32_t>((dur_ms * dx + bar_w / 2) / bar_w);
}

struct Hold {
  int32_t target_ms = -1;  // -1: no hold
  uint32_t since_ms = 0;
  uint32_t track_id = 0;
};

inline void holdStart(Hold& h, int32_t target_ms, uint32_t now_ms, uint32_t track_id) {
  h.target_ms = target_ms;
  h.since_ms = now_ms;
  h.track_id = track_id;
}

/** While the hold lasts: the position to show (ms). Else -1, and the hold is cleared. */
inline int32_t holdShows(Hold& h, uint32_t reported_ms, uint32_t now_ms, uint32_t track_id) {
  if (h.target_ms < 0) return -1;
  const int64_t gap = static_cast<int64_t>(reported_ms) - h.target_ms;
  if (track_id != h.track_id || now_ms - h.since_ms >= kSeekHoldMs ||
      (gap <= static_cast<int64_t>(kSeekNearMs) && gap >= -static_cast<int64_t>(kSeekNearMs))) {
    h.target_ms = -1;
    return -1;
  }
  return h.target_ms;
}

}  // namespace services::seek
