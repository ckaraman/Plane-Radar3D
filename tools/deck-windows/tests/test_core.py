"""Protocol tests against a fake card on a pseudo terminal (Linux CI / dev box)."""

import base64
import io
import json
import struct
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
        self.drop_once = False
        self.queue = None
        self.thumbs = {}
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
            self.say('PR:HELLO ' + json.dumps({"fw": "5in-0.32.0", "usb": self.usb, "hostv": 2, "w": 800, "h": 480,
                                               "deckicon": 72, "deckv": 2, "deckkw": 110, "deckkh": 120,
                                               "queuev": 1, "qthumb": 40}))
        elif tag in ("COVERBEGIN", "DECKBEGIN"):
            self._stage, self._meta, self._next = b"", json.loads(rest), 1
            self.say("PR:%s {\"seq\":0}" % ("COVERACK" if tag == "COVERBEGIN" else "DECKACK"))
        elif tag in ("COVER", "DECK"):
            seq, data = rest.split(" ", 1)
            seq = int(seq)
            ack = "COVERACK" if tag == "COVER" else "DECKACK"
            if self.drop_once and seq == 3:  # one line lost on the cable
                self.drop_once = False
                return
            if seq < self._next:
                self.say("PR:%s {\"seq\":%d}" % (ack, seq))
            elif seq > self._next:
                self.say("PR:%sNAK {\"need\":%d}" % (tag, self._next))
            else:
                self._stage += base64.b64decode(data)
                self._next += 1
                self.say("PR:%s {\"seq\":%d}" % (ack, seq))
        elif tag in ("COVEREND", "DECKEND"):
            ok = len(self._stage) == self._meta["len"] and zlib.crc32(self._stage) & 0xFFFFFFFF == self._meta["crc"]
            if tag == "COVEREND":
                self.cover = self._stage
            else:
                self.deck = self._stage
            self.say("PR:OK {}" if ok else 'PR:ERR {"msg":"bad"}')
        elif tag in ("QBEGIN", "QITEM", "QEND", "QCLEAR"):
            if tag == "QBEGIN":
                self.queue = {"meta": json.loads(rest), "rows": []}
            elif tag == "QITEM":
                self.queue["rows"].append(json.loads(rest))
            elif tag == "QCLEAR":
                self.queue = None
            self.say('PR:QACK {}')
        elif tag in ("QARTBEGIN",):
            self._stage, self._meta, self._next = b"", json.loads(rest), 1
            self.say('PR:QARTACK {"seq":0}')
        elif tag == "QART":
            seq, data = rest.split(" ", 1)
            self._stage += base64.b64decode(data)
            self._next += 1
            self.say('PR:QARTACK {"seq":%s}' % seq)
        elif tag == "QARTEND":
            self.thumbs[self._meta["k"]] = self._stage
            self.say("PR:OK {}")
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

    # Cover in small chunks, a few on the way; one is lost and sent again.
    jpeg = bytes(range(256)) * 40
    card.drop_once = True
    assert d.send_cover(jpeg, 3, 480, 480)
    assert card.cover == jpeg
    assert '"w": 480' in next(l for l in card.lines if l.startswith("PR:COVERBEGIN"))

    # Queue rows and a row picture.
    rows = [{"x": 4, "t": "Gece Yolu", "a": "Deniz Ay", "d": "3:58"}, {"x": 5, "t": "Kıyı", "a": "Mavi", "d": "4:12"}]
    assert d.send_queue(rows, 7, "YouTube Music")
    assert card.queue["meta"] == {"gen": 7, "n": 2, "src": "YouTube Music"} and card.queue["rows"][1]["k"] == 1
    assert card.queue["rows"][1]["t"] == "Kıyı"
    assert d.send_thumb(b"\xff\xd8" + bytes(500), 1, 7) and card.thumbs[1][:2] == b"\xff\xd8"
    assert d.clear_queue() and card.queue is None

    # Key set through setup mode and back.
    slots = [{"label": "YouTube", "img": "", "action": {"type": "site", "url": "x"}}] + \
            [{"label": "", "img": "", "action": {"type": "none"}}] * 11
    data = blob.build(slots, 72, 50, (110, 120, 18))
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


def _png(w, h, rgba):
    from PIL import Image

    buf = io.BytesIO()
    Image.new("RGBA", (w, h), rgba).save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def test_blob_v2():
    from PIL import Image, ImageDraw

    photo = _png(300, 200, (200, 30, 30, 255))
    icon_img = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    ImageDraw.Draw(icon_img).ellipse([20, 20, 108, 108], fill=(30, 200, 30, 255))
    buf = io.BytesIO()
    icon_img.save(buf, "PNG")
    icon = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()
    slots = [{"label": "Foto", "img": photo, "action": {"type": "none"}},
             {"label": "", "img": icon, "action": {"type": "none"}}] + \
            [{"label": "", "img": "", "action": {"type": "none"}}] * 10
    b = blob.build(slots, 72, 50, (110, 120, 18))
    assert b[:8] == b"DK\x02\x0c\x6e\x78\x32\x00"
    assert len(b) == 8 + 28 * 12 + 2 * 110 * 120 * 2
    pic = b[8 + 28 * 12:8 + 28 * 12 + 110 * 120 * 2]
    px = lambda x, y: struct.unpack_from("<H", pic, 2 * (y * 110 + x))[0]
    assert px(0, 0) == blob.TRANSPARENT          # rounded corner
    assert px(55, 20) != blob.TRANSPARENT        # photo fills the key
    assert (px(55, 20) >> 11) > 20               # red
    assert (px(55, 115) >> 11) < (px(55, 20) >> 11)  # darker at the bottom for the label
    ic = b[8 + 28 * 12 + 110 * 120 * 2:]
    ipx = lambda x, y: struct.unpack_from("<H", ic, 2 * (y * 110 + x))[0]
    assert ipx(55, 60) != blob.TRANSPARENT and ipx(3, 60) == blob.TRANSPARENT  # icon large, inside
    assert blob.is_icon(Image.open(io.BytesIO(base64.b64decode(icon.split(",")[1]))).convert("RGBA"))


def test_config_moves_transport():
    import tempfile

    from deck import config

    d = tempfile.mkdtemp()
    path = os.path.join(d, "config.json")
    slots = [{"label": "Önceki", "img": "", "action": {"type": "media", "code": 5}},
             {"label": "Ses +", "img": "", "action": {"type": "media", "code": 0}},
             {"label": "YouTube", "img": "", "action": {"type": "site", "url": "x"}}] + \
            [{"label": "", "img": "", "action": {"type": "none"}}] * 9
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"slots": slots, "settings": {}}, f)
    c = config.Config(path)
    assert c.slots[0]["action"]["type"] == "none" and c.slots[1]["action"]["code"] == 0  # volume key stays
    assert c.slots[2]["label"] == "YouTube" and c.settings["transport_moved"]
    c.set_slots([{"label": "Oynat", "img": "", "action": {"type": "media", "code": 3}}] + slots[1:])
    assert config.Config(path).slots[0]["label"] == "Oynat"  # once only: a key added later stays
    fresh = config.Config(os.path.join(d, "none.json"))
    assert not any((s["action"].get("type") == "media") for s in fresh.slots)


def test_browser_link():
    from websockets.sync.client import connect

    from deck import browser_link, queue_card

    got = []
    link = browser_link.BrowserLink(lambda q: got.append(q), log=lambda m: None, port=47899)
    url = "ws://127.0.0.1:47899/deck"
    for _ in range(50):
        try:
            ws = connect(url, origin=browser_link.ORIGIN, open_timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    ws.send(json.dumps({"t": "hello", "v": "1.0.0", "brand": "Google Chrome"}))
    q = {"t": "queue", "cur": 1, "now": "Gece Yolu", "items": [
        {"t": "Önceki", "a": "X", "d": "3:00", "img": ""},
        {"t": "Gece Yolu", "a": "Deniz Ay", "d": "3:58", "img": "https://i.ytimg.com/vi/a/sddefault.jpg"},
        {"t": "Kıyıda", "a": "Mavi Tren", "d": "4:12", "img": ""}]}
    ws.send(json.dumps(q))
    assert wait(lambda: got and got[-1] and len(got[-1]["items"]) == 3)
    st = link.status()
    assert st["connected"] and st["browsers"] == ["Google Chrome"] and st["queue"] == 3

    rows = queue_card.card_rows(link.queue, {"t": "Gece Yolu", "a": "Deniz Ay"})
    assert [r["x"] for r in rows] == [1, 2] and rows[0]["img"].startswith("https://i.ytimg.com")
    assert queue_card.card_rows(link.queue, {"t": "Başka şarkı"}) is None  # something else plays
    assert queue_card.cut_utf8("ığüşöç" * 20, 13) == "ığüşöç"

    # open in a tab: the extension answers
    res = {}

    def opener():
        res["ok"] = link.open_url("https://music.youtube.com/", "chrome")
    t = threading.Thread(target=opener)
    t.start()
    msg = json.loads(ws.recv(timeout=3))
    assert msg["t"] == "open" and msg["url"] == "https://music.youtube.com/"
    ws.send(json.dumps({"t": "opened", "id": msg["id"], "reused": True}))
    t.join(3)
    assert res["ok"] is True
    assert link.open_url("https://x.com", "edge") is False  # Edge has no extension here
    assert link.open_url("https://x.com", "", "safari.app") is False  # nor Safari

    assert link.play(2, "Kıyıda")
    m = json.loads(ws.recv(timeout=3))
    assert m == {"t": "play", "i": 2, "title": "Kıyıda"}

    # another origin is not let in
    try:
        connect(url, origin="https://evil.example", open_timeout=2).close()
        assert False, "foreign origin accepted"
    except Exception:  # noqa: BLE001
        pass
    ws.close()
    assert wait(lambda: got[-1] is None)
    assert browser_link.is_music_url("music.youtube.com/watch?v=1") and not browser_link.is_music_url("https://github.com")


def test_layout():
    area = blob.default_area(800)
    assert area == (470, 380, 10, 18)
    assert blob.layout(12, area) == (4, 3, 110, 120)  # all keys: the usual grid
    assert blob.layout(8, area)[:2] == (3, 3)
    assert blob.layout(4, area)[:2] == (2, 2)
    assert blob.layout(1, area) == (1, 1, 470, 380)
    slots = [{"label": "A", "img": "", "action": {"type": "none"}}] + [{"label": "", "img": "", "action": {"type": "none"}}] * 8 + \
            [{"label": "", "img": "", "action": {"type": "media", "code": 3}}] * 3
    bx = blob.boxes(slots, area)
    assert [b[0] for b in bx] == [0, 9, 10, 11]
    assert bx[0][1:] == (0, 0, 230, 185) and bx[3][1:] == (240, 195, 230, 185)
    b = blob.build(slots, 72, 50, None, area)
    assert b[:12] == b"DK\x03\x0c\x02\x02\x32\x00" + struct.pack("<HH", 230, 185)
    assert len(b) == 12 + 28 * 12
    # 5B panel: the firmware's numbers
    assert blob.default_area(1024) == (600, 483, 12, 23)
    for n in range(1, 13):
        cols, rows, kw, kh = blob.layout(n, blob.default_area(1024))
        assert cols * kw + (cols - 1) * 12 <= 602 and rows * kh + (rows - 1) * 12 <= 485 and cols * rows >= n


def test_cover_and_position():
    from PIL import Image

    from deck import media

    buf = io.BytesIO()
    im = Image.new("RGB", (480, 360), (0, 0, 0))
    im.paste(Image.new("RGB", (480, 270), (90, 120, 200)), (0, 45))  # a letterboxed video frame
    im.save(buf, "JPEG")
    jpeg, w, h = media.cover_jpeg(buf.getvalue(), 800, 480)
    assert (w, h) == (480, 270), (w, h)  # bars gone, its own size (not blown up)
    assert Image.open(io.BytesIO(jpeg)).size == (480, 270)
    buf = io.BytesIO()
    Image.new("RGB", (1200, 1200), (10, 200, 10)).save(buf, "JPEG")
    assert media.cover_jpeg(buf.getvalue(), 800, 480)[1:] == (480, 480)  # large: fits the height

    # Position: a browser that keeps reporting the resume point must not pull the bar back.
    class T:
        def __init__(self, s):
            self.s = s

        def total_seconds(self):
            return self.s

    class TL:
        start_time, end_time, last_updated_time = T(0), T(200), None

        def __init__(self, p):
            self.position = T(p)

    class Props:
        title, artist, album_title, thumbnail = "Song", "Artist", "", None

    class Info:
        def __init__(self, playing):
            self.playback_status = "P" if playing else "S"

    media.Status = type("S", (), {"PLAYING": "P"})
    sess = {"tl": TL(30), "play": True}

    class Sess:
        source_app_user_model_id = "chrome"

        async def try_get_media_properties_async(self):
            return Props()

        def get_playback_info(self):
            return Info(sess["play"])

        def get_timeline_properties(self):
            return sess["tl"]

    class Mgr:
        def get_current_session(self):
            return Sess()

    import asyncio

    media.MediaManager = None  # no thread
    seen = []
    w = media.MediaWatcher(lambda st, c: seen.append(st), log=lambda m: None)
    run = lambda: asyncio.new_event_loop().run_until_complete(w._poll(Mgr()))
    clock = [1000.0]
    real_monotonic = media.time.monotonic
    media.time.monotonic = lambda: clock[0]
    run()
    assert abs(w.position() - 30) < 0.01
    clock[0] += 4.5
    run()  # still says 30: our clock runs on
    assert abs(w.position() - 34.5) < 0.01
    clock[0] += 5
    sess["tl"] = TL(39)  # a late report close to ours: kept
    run()
    assert abs(w.position() - 39.5) < 0.01
    sess["tl"] = TL(120)  # a seek
    run()
    assert abs(w.position() - 120) < 0.01
    sess["play"] = False
    sess["tl"] = TL(121)
    clock[0] += 1
    run()
    clock[0] += 10
    assert abs(w.position() - 121) < 0.01  # paused: stands still
    media.time.monotonic = real_monotonic


if __name__ == "__main__":
    test_blob()
    test_blob_v2()
    test_layout()
    test_config_moves_transport()
    test_browser_link()
    test_cover_and_position()
    test_all()
    print("tests ok")
