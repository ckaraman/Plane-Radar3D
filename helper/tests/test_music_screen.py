"""The "open the music screen" tick only decides the page: what plays is always sent."""

import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deck.core import Core  # noqa: E402


def fake_core(tick, now):
    return SimpleNamespace(
        config=SimpleNamespace(settings={"music_screen": tick, "back_seconds": 60}),
        now=now,
        media=SimpleNamespace(position=lambda: 12.5),
    )


SONG = {"t": "Song", "a": "Artist", "play": True, "id": 7}


def test_tick_off_still_sends_the_song():
    msg = Core._now_msg(fake_core(False, dict(SONG)))
    assert msg["t"] == "Song" and msg["id"] == 7 and "none" not in msg
    assert msg["open"] is False


def test_tick_on_opens():
    msg = Core._now_msg(fake_core(True, dict(SONG)))
    assert msg["t"] == "Song" and msg["open"] is True


def test_nothing_playing():
    assert Core._now_msg(fake_core(True, {"none": True})) == {"none": True}
    assert Core._now_msg(fake_core(False, {"none": True})) == {"none": True}


if __name__ == "__main__":
    test_tick_off_still_sends_the_song()
    test_tick_on_opens()
    test_nothing_playing()
    print("ok")
