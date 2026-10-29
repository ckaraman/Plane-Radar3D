"""Card firmware check and download (stage 8c). No serial port: runs on Windows too."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deck import feed, firmware  # noqa: E402

CARD = {"fw": "5in-0.41.0", "sys": 2, "hw": "ws5"}


def test_latest_version():
    assert firmware.latest_version(lambda url: {"version_ws5": "5in-0.42.0"}) == "5in-0.42.0"
    assert firmware.latest_version(lambda url: {}) is None

    def boom(url):
        raise feed.HttpError("yok")
    assert firmware.latest_version(boom) is None


def test_offer():
    assert firmware.offer(CARD, "5in-0.42.0")
    assert not firmware.offer(CARD, "5in-0.41.0")
    assert not firmware.offer(CARD, None)
    assert not firmware.offer(None, "5in-0.42.0")
    assert not firmware.offer(dict(CARD, sys=1), "5in-0.42.0")
    assert not firmware.offer(dict(CARD, hw="ws5b"), "5in-0.42.0")


def test_download():
    good = b"\xe9" + bytes(5000)
    urls = []
    assert firmware.download("5in-0.42.0", lambda u: urls.append(u) or good) == good
    assert urls == ["https://ckaraman.github.io/Plane-Radar3D/firmware-ws5.bin?v=5in-0.42.0"]
    for bad in (b"\xe9" + bytes(100), b"<html>" + bytes(5000)):
        try:
            firmware.download("x", lambda u, b=bad: b)
            assert False, "a broken file was accepted"
        except RuntimeError as e:
            assert "bozuk" in str(e)


if __name__ == "__main__":
    test_latest_version()
    test_offer()
    test_download()
    print("tests ok")
