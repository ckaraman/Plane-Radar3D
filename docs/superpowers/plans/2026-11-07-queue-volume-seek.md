# Queue Screen Volume and Time Bar Seeking (10n) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A volume bar on the card's queue screen, and tap / drag seeking on the time bar of the music page and the queue screen.

**Architecture:** The card sends `PR:SEEK {"ms":N}` on finger lift; the helper hands it to the current media session (Windows: `try_change_playback_position_async`; Mac: MediaRemote Adapter `seek`). The helper adds `"sk":1` to `PR:NOW` only when the session accepts seeking; the card makes the bar draggable only then. The pure seek arithmetic (finger x -> ms, the "hold" that keeps the chosen position shown until the helper agrees) lives in a header with a host test, like `queue_carry.h`.

**Tech Stack:** ESP32-S3 firmware (C++17, PlatformIO, LovyanGFX, ArduinoJson), helper in Python 3 (winrt on Windows, perl MediaRemote Adapter on the Mac), host tests with g++ in CI.

**Spec:** `docs/superpowers/specs/2026-11-07-queue-volume-seek-design.md`

## Global Constraints

- Card firmware version `5in-0.51.0` (`include/config.h` `kFirmwareVersion`); Deck `1.13.0` (`helper/app.py` `VERSION`).
- Queue screen volume layout A: prev / play / next move left, speaker icon and a shorter volume bar to their right; same `SETVOL` / `MUTE` messages as the music page.
- Seeking on both screens; tap jumps, drag grows the knob and shows the target time in a bubble above it; the jump is sent on finger lift; the page does not turn while the finger is on the bar.
- The bar is draggable only when the last `PR:NOW` had `"sk":1`; older helpers send none, so the bar stays display-only.
- After a seek the card shows the chosen position for about 3 s (`kSeekHoldMs = 3000`) or until a reported position lies within 2 s of it (`kSeekNearMs = 2000`).
- No changes to `helper/browser-extension/`.
- Helper tests run as scripts (`python helper/tests/test_core.py` needs Linux for `test_all`; on Windows run the single test functions); use a temporary venv with Pillow, websockets and the winrt packages.
- Commits and code comments in English; user-facing card text in Turkish.

## Review Focus

- A drag that starts on the time bar and wanders sideways past `D(24)` must stay a seek, never a page turn (music page swipe rule at `deck_screen.cpp:1571`). Test: in Task 5, the swipe check excludes `Target::SeekBar`.
- A tap on the time bar while `sk` is absent (desktop Spotify, older helper) must do nothing at all, not even move the knob. Test: Task 1 `seek_bar_test` "disabled" case; Task 5 touch start checks `host::now().can_seek`.
- The song changes during the hold (next track starts): the hold must end at once, not show the old target on the new song. Test: Task 1 `holdShows` with a changed track id.
- The finger lifts beyond the bar ends (x < bar x or > bar x + w): the target clamps to 0 / duration, never wraps or overflows. Test: Task 1 `seekTarget` clamp cases.
- Duration 0 (live stream, unknown length): no seek is sent. Test: Task 1 `seekTarget` returns `-1` for duration 0.

---

### Task 1: Seek arithmetic header and host test

**Files:**
- Create: `src/deck/services/seek_bar.h`
- Create: `tools/host-tests/seek_bar_test.cpp`
- Modify: `.github/workflows/build.yml:206-207` (build and run the new test after `queue_carry_test`)

**Interfaces:**
- Produces (namespace `services::seek`, header-only, no Arduino includes):
  - `constexpr uint32_t kSeekHoldMs = 3000; constexpr uint32_t kSeekNearMs = 2000;`
  - `int32_t seekTarget(int x, int bar_x, int bar_w, uint32_t duration_s)` -> ms in `[0, duration_s*1000]`, `-1` when `duration_s == 0` or `bar_w <= 0`.
  - `struct Hold { int32_t target_ms = -1; uint32_t since_ms = 0; uint32_t track_id = 0; };`
  - `void holdStart(Hold& h, int32_t target_ms, uint32_t now_ms, uint32_t track_id)`
  - `int32_t holdShows(Hold& h, uint32_t reported_ms, uint32_t now_ms, uint32_t track_id)` -> the ms to show while the hold lasts, else `-1` (and clears `h`). Ends when `now_ms - since_ms >= kSeekHoldMs`, when `|reported_ms - target_ms| <= kSeekNearMs`, or when `track_id` differs.

- [ ] **Step 1: Write `tools/host-tests/seek_bar_test.cpp`** in the style of `queue_carry_test.cpp` (`check(bool, const char*)`, exit code = failures). Cases:

```cpp
check(seekTarget(100, 100, 200, 240) == 0, "left end");
check(seekTarget(300, 100, 200, 240) == 240000, "right end");
check(seekTarget(200, 100, 200, 240) == 120000, "middle");
check(seekTarget(40, 100, 200, 240) == 0, "left of bar clamps");
check(seekTarget(900, 100, 200, 240) == 240000, "right of bar clamps");
check(seekTarget(200, 100, 200, 0) == -1, "no duration: no seek");
check(seekTarget(200, 100, 0, 240) == -1, "disabled bar");
Hold h; holdStart(h, 90000, 1000, 7);
check(holdShows(h, 30000, 1500, 7) == 90000, "old report: target shown");
check(holdShows(h, 89000, 1600, 7) == -1 && h.target_ms == -1, "report near: hold ends");
holdStart(h, 90000, 1000, 7);
check(holdShows(h, 30000, 4000, 7) == -1, "3 s: hold ends");
holdStart(h, 90000, 1000, 7);
check(holdShows(h, 30000, 1200, 8) == -1, "new track: hold ends");
```

- [ ] **Step 2: Run it, expect a compile failure** (header missing)

Run: `g++ -std=gnu++17 -O2 -Wall -Isrc/deck tools/host-tests/seek_bar_test.cpp -o seek_bar_test`
Expected: `fatal error: services/seek_bar.h: No such file or directory`

- [ ] **Step 3: Implement `src/deck/services/seek_bar.h`** with the signatures above (integer math; `seekTarget` rounds `(x - bar_x) * duration_ms / bar_w` to nearest).

- [ ] **Step 4: Run, expect pass**

Run: `g++ -std=gnu++17 -O2 -Wall -Isrc/deck tools/host-tests/seek_bar_test.cpp -o seek_bar_test && ./seek_bar_test`
Expected: exit 0, no `FAIL` lines. (No local g++: rely on CI, step 5.)

- [ ] **Step 5: Add the two CI lines** after `queue_carry_test` in `build.yml`, then commit

```bash
git add src/deck/services/seek_bar.h tools/host-tests/seek_bar_test.cpp .github/workflows/build.yml
git commit -m "Card: seek arithmetic and its host test (10n)"
```

### Task 2: Helper (Windows) can-seek flag and seeking

**Files:**
- Modify: `helper/deck/media.py` (`MediaWatcher.__init__`, `_poll`, new `seek`)
- Modify: `helper/deck/core.py:_card_event` (new `SEEK` tag)
- Modify: `helper/deck/device.py:193` (let `SEEK` through)
- Test: `helper/tests/test_core.py` (new `test_seek`, add to `__main__`)

**Interfaces:**
- Produces: `MediaWatcher.seek(ms: int) -> None` (queues the request; the media thread applies it on its next `_poll` to the current session); `state["sk"]` is `True` / `False` (part of the "changed" key list, so the card gets a fresh `NOW` when it flips); `PR:NOW` carries `"sk"` because `_now_msg` copies `self.now`.
- Card event: `PR:SEEK {"ms": N}` -> `self.media.seek(int(data["ms"]))` (cheap, no thread needed).

- [ ] **Step 1: Write `test_seek`** using the fake `Sess` / `Mgr` pattern of `test_cover_and_position` (no thread: `media.MediaManager = None`). The fake session gets `get_playback_info().controls.is_playback_position_enabled` and an async `try_change_playback_position_async(ticks)` that records `ticks`. Assertions:

```python
run()
assert w.state["sk"] is True
w.seek(95000)
run()
assert asked == [95000 * 10000]          # 100 ns ticks
assert abs(w.position() - 95) < 0.01     # re-anchored at once
controls.is_playback_position_enabled = False
run()
assert w.state["sk"] is False
w.seek(10000); run()
assert asked == [95000 * 10000]          # not sent when the session refuses seeking
me = SimpleNamespace(media=SimpleNamespace(seek=got.append), log=lambda m: None)
core.Core._card_event(me, "SEEK", {"ms": 42000})
assert got == [42000]
```

- [ ] **Step 2: Run it, expect failure**

Run (temp venv): `python -c "import sys; sys.path.insert(0,'helper/tests'); import test_core; test_core.test_seek()"`
Expected: `KeyError: 'sk'` or `AttributeError: ... 'seek'`

- [ ] **Step 3: Implement** `seek` (store `self._seek_ms` under a small lock), and in `_poll` after reading `info`: `sk = bool(getattr(getattr(info, "controls", None), "is_playback_position_enabled", False))`; if a request is pending and `sk`, `await s.try_change_playback_position_async(ms * 10000)`, then `self._anchor = (ms / 1000, time.monotonic())` and set `self._sig` so the next stale report within 2 s does not pull it back; drop the request either way. Add `"sk"` to `state` and to the changed-keys tuple. `core._card_event`: `elif tag == "SEEK"`. `device.py`: add `"SEEK"` to the tag tuple.

- [ ] **Step 4: Run `test_seek` and `test_cover_and_position`, expect both pass**

- [ ] **Step 5: Commit**

```bash
git add helper/deck/media.py helper/deck/core.py helper/deck/device.py helper/tests/test_core.py
git commit -m "Deck: seek the playing song from the card (Windows), can-seek flag in NOW (10n)"
```

### Task 3: Helper (Mac) seeking

**Files:**
- Modify: `helper/deck/media_mac.py` (new module function `seek_to(ms)`, `MediaWatcher.seek`, `"sk"` in state)
- Test: `helper/tests/test_core.py` (`test_seek_mac`, runs anywhere: patches `subprocess.Popen` and `adapter_paths`)

**Interfaces:**
- Produces: `MediaWatcher.seek(ms: int) -> None` with the same meaning as Task 2; `state["sk"] = True` whenever something plays (the adapter gives no per-app flag).
- Command: `["/usr/bin/perl", script, fw, "seek", str(ms * 1000)]` (microseconds, adapter's `seek` command; to be confirmed on a Mac during the card test).

- [ ] **Step 1: Write `test_seek_mac`:** patch `media_mac.adapter_paths` to `("a.pl", "F")` and `media_mac.subprocess.Popen` to record args; build a `MediaWatcher` without its thread (patch `threading.Thread` to a no-op or call `object.__new__` and set the fields used); `w.state = {"t": "x", "play": True, "d": 200}`; `w.seek(61000)`; assert the recorded args end with `["seek", "61000000"]` and `abs(w.position() - 61) < 0.01`.

- [ ] **Step 2: Run it, expect failure** (`AttributeError: seek`)

- [ ] **Step 3: Implement** `seek` (start the perl command, re-anchor `self._anchor = (ms / 1000, time.time(), rate)`); put `"sk": True` into the state built in `_data`.

- [ ] **Step 4: Run, expect pass**

- [ ] **Step 5: Commit**

```bash
git add helper/deck/media_mac.py helper/tests/test_core.py
git commit -m "Deck (Mac): seek through the MediaRemote Adapter (10n)"
```

### Task 4: Card host link: can-seek, sending a seek, the hold

**Files:**
- Modify: `src/deck/services/host_link.h` (doc comment line 15 adds `"sk":1`; `NowPlaying` gets `bool can_seek = false;`; declare `void sendSeek(uint32_t ms);`)
- Modify: `src/deck/services/host_link.cpp` (parse `doc["sk"] | false` in the `NOW` handler; `sendSeek` prints `PR:SEEK {"ms":%lu}`, starts the hold; `positionNowMs()` returns `holdShows(...)` while it lasts; include `services/seek_bar.h`)

**Interfaces:**
- Consumes: Task 1 `services::seek::{Hold, holdStart, holdShows}`.
- Produces: `host::now().can_seek`, `host::sendSeek(uint32_t ms)`; `host::positionNowMs()` / `positionNow()` already used by both screens now return the held target during the hold, so drawing needs no change for it.

- [ ] **Step 1: Implement** as listed. In the `NOW` handler, when a hold is active, do not let `keep_clock` keep the old clock: after the hold ends the reported position wins (bump `s_now_gen`).
- [ ] **Step 2: Build the firmware**

Run: `pio run -e ws5`
Expected: `SUCCESS`. (No local toolchain: push and rely on the CI `firmware` job.)

- [ ] **Step 3: Commit**

```bash
git add src/deck/services/host_link.h src/deck/services/host_link.cpp
git commit -m "Card: can-seek flag, PR:SEEK and the hold after a seek (10n)"
```

### Task 5: Card screens: queue screen volume, seek touch and bubble, version

**Files:**
- Modify: `src/deck/ui/deck_screen.cpp`
  - Constants near line 87: queue transport moves left: `kQPrevCx = D(60), kQPlayCx = D(132), kQNextCx = D(204)`; queue volume bar `kQVolX = D(268), kQVolW = D(136)` on `kQCtrlY`, speaker icon at `kQCoverX + ...` left of `kQVolX` (`D(246)` centre). `static_assert` that the knob at 100 % stays inside `kQLeftPartW`.
  - Factor the music page volume drawing (lines 818-833) into `drawVolumeBar(Gfx&, const paint::Canvas&, int speaker_cx, int bar_x, int bar_w, int cy)` and call it from both `drawMusicFront` and `drawQueueFront` (queue: as part 3 of its left column, keyed on level and mute like the music page; adjust the `part()` band so it does not overlap the transport part).
  - `Target` gains `SeekBar`; a file-static `int s_seek_x = -1;` (finger x while dragging, -1 otherwise) and a helper `seekBar(bool queue, int& x, int& w, int& y)` returning the bar geometry of the current screen (music: `kMusicPad + kTimeW`, `kW - 2*kMusicPad - 2*kTimeW`, `kBarY`; queue: `kQCoverX + kTimeW`, `kQLeftW - 2*kTimeW`, `kQBarY`).
  - Touch start (both the queue branch and the music branch, before the existing checks): if `host::now().can_seek` and `|y - bar_y| < D(22)` and `bar_x - D(10) <= x < bar_x + w + D(10)`: `s_target = Target::SeekBar; s_seek_x = x; presentSoon();`. Queue volume bar touch mirrors the music page's `VolBar` branch with the queue geometry.
  - Move: the swipe check at line 1571 also excludes `Target::SeekBar`; while `SeekBar`, `s_seek_x = x; presentSoon();`.
  - Lift: `SeekBar` -> `t = seekTarget(s_seek_x, ...)`; if `t >= 0`, `host::sendSeek(t)`; `s_seek_x = -1; presentSoon()`.
  - Drawing (both time bars): while `s_seek_x >= 0`, the knob is drawn at the finger with radius `D(11)` plus a lavender halo, the left time and a bubble above the knob (white rounded rect, `text::Ui13` bold-ish dark text) show `seekTarget(...)`; include `s_seek_x` in the bar part's key so it redraws.
- Modify: `include/config.h:13` -> `"5in-0.51.0"`
- Modify: `helper/app.py:17` -> `"1.13.0"`

**Interfaces:**
- Consumes: Task 1 `seekTarget`, Task 4 `host::now().can_seek`, `host::sendSeek`.

- [ ] **Step 1: Implement the queue screen layout and volume bar**, build (`pio run -e ws5` or CI), commit `Card: volume bar on the queue screen (10n)`.
- [ ] **Step 2: Implement seek touch, drag drawing and the version bumps**, build, commit `Card 5in-0.51.0, Deck 1.13.0: seek on the time bar (10n)`.

### Task 6: Push, CI, card test, plan note

**Files:**
- Modify: `docs/PLAN.md` (new `10n` line under "Sıradaki oturum": what changed, versions, "kartta denenmedi")

- [ ] **Step 1:** Push `ws5`, open a PR to `main`, wait for CI: `firmware` (includes both host tests), `deckapp`, both `deckapp-mac` green.
- [ ] **Step 2:** Give the user the card test steps (install the PR build or, after "Birleştir", the site build; update the card): queue screen volume drag and tap; seek by tap and by drag on both screens in Chrome YouTube Music and Spotify web; a desktop app shows a display-only bar; a sideways drag on the bar never turns the page; next song during the 3 s hold.
- [ ] **Step 3:** Commit the PLAN.md note.
