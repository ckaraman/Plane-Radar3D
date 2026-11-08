"""Microphone rules for the card's volume knob (stage 10q). Runs on Windows too, without COM."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deck import mic  # noqa: E402


class FakeEp:
    def __init__(self, muted, broken=False):
        self.muted, self.broken = muted, broken

    def GetMute(self):
        if self.broken:
            raise OSError("device gone")
        return 1 if self.muted else 0

    def SetMute(self, value, _ctx):
        if self.broken:
            raise OSError("device gone")
        self.muted = bool(value)


def test_state_with_no_usable_microphone_is_none():
    assert mic.state_of([]) == 0
    assert mic.state_of([FakeEp(True, broken=True)]) == 0


def test_state_is_muted_only_when_all_are():
    assert mic.state_of([FakeEp(False)]) == 1
    assert mic.state_of([FakeEp(True), FakeEp(False)]) == 1
    assert mic.state_of([FakeEp(True), FakeEp(True)]) == 2


def test_state_skips_a_broken_microphone():
    assert mic.state_of([FakeEp(True), FakeEp(True, broken=True)]) == 2


def test_toggle_mutes_all_when_one_is_open():
    eps = [FakeEp(True), FakeEp(False)]
    mic.toggle(eps)
    assert all(e.muted for e in eps)


def test_toggle_opens_all_when_all_are_muted():
    eps = [FakeEp(True), FakeEp(True)]
    mic.toggle(eps)
    assert not any(e.muted for e in eps)


def test_toggle_still_reaches_the_others_past_a_broken_one():
    eps = [FakeEp(False, broken=True), FakeEp(False)]
    mic.toggle(eps)
    assert eps[1].muted


def test_payload_carries_mic_when_volume_is_sent():
    assert mic.vol_payload(40, False, 2, True) == {"v": 40, "m": False, "mic": 2}


def test_payload_is_neutral_without_mic_when_volume_is_off():
    assert mic.vol_payload(40, True, 0, False) == {"v": 50, "m": False}
    assert "mic" not in mic.vol_payload(40, False, 1, False)


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
    print("test_mic ok")
