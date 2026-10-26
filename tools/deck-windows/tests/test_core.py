"""Protocol tests against a fake card on a pseudo terminal (Linux CI / dev box)."""

import base64
import json
import os
import pty
import sys
import threading
import time
import tty
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deck import blob, device  # noqa: E402


class FakeCard:
    """Answers like services/serial_setup.cpp + host_link.cpp."""

    def __init__(self, usb="kb"):
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.path = os.ttyname(slave)
        self.usb = usb
        self.lines = []
        self.cover = None
        self.deck = None
        self._stage = b""
        threading.Thread(target=self._run, daemon=True).start()

    def say(self, text):
        os.write(self.master, (text + "\n").encode())

    def _run(self):
        buf = b""
        while True:
            try:
                d = os.read(self.master, 4096)
            except OSError:
                return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode().strip()
                if line:
                    self.lines.append(line)
                    self._handle(line)

    def _handle(self, line):
        tag, _, rest = line[3:].partition(" ")
        if tag == "HELLO":
            self.say('PR:HELLO ' + json.dumps({"fw": "5in-0.31.0", "usb": self.usb, "hostv": 1, "w": 800, "h": 480, "deckicon": 72}))
        elif tag in ("COVERBEGIN", "DECKBEGIN"):
            self._stage, self._meta = b"", json.loads(rest)
            self.say("PR:%s {\"seq\":0}" % ("COVERACK" if tag == "COVERBEGIN" else "DECKACK"))
        elif tag in ("COVER", "DECK"):
            seq, data = rest.split(" ", 1)
            self._stage += base64.b64decode(data)
            self.say("PR:%s {\"seq\":%s}" % ("COVERACK" if tag == "COVER" else "DECKACK", seq))
        elif tag in ("COVEREND", "DECKEND"):
            ok = len(self._stage) == self._meta["len"] and zlib.crc32(self._stage) & 0xFFFFFFFF == self._meta["crc"]
            if tag == "COVEREND":
                self.cover = self._stage
            else:
                self.deck = self._stage
            self.say("PR:OK {}" if ok else 'PR:ERR {"msg":"bad"}')
        elif tag == "USBSETUP":
            self.say('PR:USBMODE {"usb":"setup","restart":true}')
            self.usb = "setup"
        elif tag == "USBNORMAL":
            self.say('PR:USBMODE {"usb":"kb","restart":true}')
            self.usb = "kb"


def wait(cond, t=5):
    end = time.time() + t
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


def test_all():
    card = FakeCard()
    events = []
    device.find_port = lambda pid: card.path  # the same fake answers in both modes
    d = device.Device(lambda tag, data: events.append((tag, data)), log=lambda m: None)
    assert wait(lambda: d.connected), "no connection"
    assert d.status()["usb"] == "kb"

    # Card -> helper events.
    card.say('PR:KEY {"i":4}')
    card.say('PR:SETVOL {"v":55}')
    assert wait(lambda: len(events) == 2)
    assert events[0] == ("KEY", {"i": 4}) and events[1] == ("SETVOL", {"v": 55})

    # Volume / now playing lines.
    d.send_json("VOL", {"v": 42, "m": False})
    assert wait(lambda: any(l.startswith('PR:VOL {"v":42') for l in card.lines))

    # Cover in small chunks.
    jpeg = bytes(range(256)) * 40
    assert d.send_cover(jpeg, 3)
    assert card.cover == jpeg

    # Key set through setup mode and back.
    slots = [{"label": "YouTube", "img": "", "action": {"type": "site", "url": "x"}}] + \
            [{"label": "", "img": "", "action": {"type": "none"}}] * 11
    data = blob.build(slots, 72, 50)
    steps = []
    d.upload_deck(data, lambda p, t: steps.append(p))
    assert card.deck == data and steps[-1] == 100
    assert wait(lambda: d.connected)
    d.stop()


def test_blob():
    assert blob.parse_combo("Ctrl+Shift+S") == (blob.MOD_CTRL | blob.MOD_SHIFT, ord("s"))
    assert blob.parse_combo("Win+D") == (blob.MOD_GUI, ord("d"))
    assert blob.parse_combo("F13") == (0, 0xF0)
    assert blob.parse_combo("Ctrl+Ğ") is None  # not on the card's US map: the helper does it
    assert blob.slot_kind(3, {"type": "keys", "combo": "Ctrl+Ğ"}) == (blob.KIND_HOST, 3, 0)
    assert blob.slot_kind(0, {"type": "media", "code": 5}) == (blob.KIND_MEDIA, 5, 0)
    b = blob.build([{"label": "Çok uzun bir Türkçe etiket ışığı", "img": "", "action": {"type": "app"}}] + [{"label": "", "img": "", "action": {"type": "none"}}] * 11, 72, 50)
    assert b[:6] == b"DK\x01\x0c\x48\x32" and len(b) == 6 + 28 * 12
    assert b[6] == blob.KIND_HOST and b[6 + 4:6 + 28].rstrip(b"\0").decode() == "Çok uzun bir Türkçe "


if __name__ == "__main__":
    test_blob()
    test_all()
    print("tests ok")
