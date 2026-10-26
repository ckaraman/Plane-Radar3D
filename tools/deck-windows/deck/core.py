"""Ties it together: card <-> volume, what is playing, key actions."""

import threading
import time

from . import actions, blob
from .config import Config
from .device import Device
from .media import MediaWatcher, cover_jpeg
from .volume import VolumeWatcher


class Core:
    def __init__(self, log=print):
        self.log = log
        self.config = Config()
        self.device = Device(self._card_event, log=log)
        self.volume = VolumeWatcher(self._volume_changed, log=log)
        self.media = MediaWatcher(self._media_changed, log=log)
        self.now = {"none": True}
        self._cover = None          # (jpeg, track id, w, h, version) for the card
        self._cover_ver = 0
        self._cover_sent = None     # version the card has
        self._cover_lock = threading.Lock()
        self._last_now_sent = 0.0
        self._was_connected = False
        self.upload = {"running": False, "pct": 0, "text": "", "error": ""}
        threading.Thread(target=self._pump, name="pump", daemon=True).start()

    # ------------------------------------------------------------------ from the card
    def _card_event(self, tag, data):
        if tag == "KEY":
            i = int(data.get("i", -1))
            slots, _ = self.config.snapshot()
            if 0 <= i < len(slots):
                self.log("key %d: %s" % (i, slots[i].get("label")))
                threading.Thread(target=self._run_action, args=(slots[i]["action"],), daemon=True).start()
        elif tag == "SETVOL":
            self.volume.set(int(data.get("v", self.volume.level)))
        elif tag == "MUTE":
            self.volume.toggle_mute()

    def _run_action(self, action):
        try:
            actions.run(action)
        except Exception as e:  # noqa: BLE001
            self.log("action: %s" % e)

    # ------------------------------------------------------------------ to the card
    def _volume_changed(self, level, muted):
        if self.config.settings.get("send_volume", True):
            self.device.send_json("VOL", {"v": level, "m": muted})

    def _media_changed(self, state, cover):
        self.now = state
        if cover is not None and not state.get("none"):
            threading.Thread(target=self._make_cover, args=(cover, dict(state)), name="cover", daemon=True).start()
        self._send_now()

    def _make_cover(self, raw, state):
        """Screen-fitting JPEG of the picture the media session gives."""
        st = self.device.status()
        w, h = int(st.get("w") or 800), int(st.get("h") or 480)
        try:
            jpeg, cw, ch = cover_jpeg(raw, w, h)
        except Exception as e:  # noqa: BLE001 - a picture Pillow cannot read
            self.log("cover: %s" % e)
            return
        with self._cover_lock:
            if self.now.get("id") != state.get("id"):
                return  # the song changed meanwhile
            self._cover_ver += 1
            self._cover = (jpeg, state.get("id", 0), cw, ch, self._cover_ver)

    def _now_msg(self):
        if not self.config.settings.get("music_screen", True) or self.now.get("none"):
            return {"none": True}
        msg = dict(self.now)
        pos = self.media.position()
        msg["p"], msg["pm"] = int(pos), int(pos * 1000)
        msg["back"] = int(self.config.settings.get("back_seconds", 60))
        return msg

    def _send_now(self):
        self.device.send_json("NOW", self._now_msg())
        self._last_now_sent = time.time()

    def _pump(self):
        """Heartbeat (volume every 2 s, now playing every 5 s), the cover once per track."""
        last_vol = 0.0
        while True:
            time.sleep(0.25)
            dev = self.device
            if not dev.connected or dev.busy:
                self._was_connected = False
                continue
            if not self._was_connected:  # (re)connected: tell it everything
                self._was_connected = True
                self._cover_sent = None
                last_vol = 0.0
                self._last_now_sent = 0.0
            if time.time() - last_vol >= 2.0:
                last_vol = time.time()
                if self.config.settings.get("send_volume", True):
                    dev.send_json("VOL", {"v": self.volume.level, "m": self.volume.muted})
                else:
                    dev.send_json("VOL", {"v": 50, "m": False})
            if time.time() - self._last_now_sent >= 5.0:
                self._send_now()
            c = self._cover
            if c and c[4] != self._cover_sent and c[1] == self.now.get("id") \
                    and self.config.settings.get("music_screen", True) and not self.now.get("none"):
                if dev.send_cover(c[0], c[1], c[2], c[3], abort=lambda: self._cover is not c):
                    self._cover_sent = c[4]
                elif self._cover is c:
                    time.sleep(2)

    # ------------------------------------------------------------------ editor
    def preview_layout(self, slots):
        """Where the keys sit on the card's screen, for the window's preview."""
        st = self.device.status()
        w = int(st.get("w") or 800)
        h = int(st.get("h") or 480)
        area = st.get("deckarea")
        area = tuple(int(v) for v in area) if isinstance(area, list) and len(area) == 4 else blob.default_area(w)
        compact = bool(st.get("deckarea")) or not st.get("connected")  # new firmware (or not known yet)
        if compact:
            bx = blob.boxes(slots, area)
        else:  # older firmware: the fixed 4 x 3 grid
            kw, kh, gap = (area[0] - 3 * area[2]) // 4, (area[1] - 2 * area[2]) // 3, area[2]
            bx = [(i, (i % 4) * (kw + gap), (i // 4) * (kh + gap), kw, kh) for i in range(len(slots))]
        ox, oy = 20 * w // 800, 68 * w // 800
        return {"screen": [w, h], "radius": area[3], "compact": compact,
                "knob": [512 * w // 800, 88 * w // 800, 268 * w // 800],
                "keys": [{"i": i, "x": ox + x, "y": oy + y, "w": kw, "h": kh} for i, x, y, kw, kh in bx]}

    def send_keys_to_card(self):
        """Builds the key set and uploads it (runs in a thread; see self.upload)."""
        if self.upload["running"]:
            return False
        if self.device.released:  # sending means: take the card back
            self.device.release(False)
        for _ in range(24):
            if self.device.connected:
                break
            time.sleep(0.25)
        if not self.device.connected:
            self.upload.update(running=False, pct=0, text="", error="Kart bağlı değil. USB kablosunu ve kurulum sayfasının kartı bırakmış olduğunu kontrol et.")
            return False
        slots, settings = self.config.snapshot()
        st = self.device.status()
        key = None
        if st.get("deckkw") and st.get("deckkh"):  # firmware 0.32+: pictures fill the key
            key = (int(st["deckkw"]), int(st["deckkh"]), round(18 * int(st.get("w") or 800) / 800))
        area = st.get("deckarea")
        area = tuple(int(v) for v in area) if isinstance(area, list) and len(area) == 4 else None
        try:
            data = blob.build(slots, int(st.get("deckicon") or 72), settings.get("steps", 50), key, area)
        except Exception as e:  # noqa: BLE001
            self.log("keys: %s" % e)
            self.upload.update(running=False, pct=0, text="", error="Tuşlar hazırlanamadı: %s" % e)
            return False
        self.log("keys: %d KB for the card (%s)" % (len(data) // 1024, "used keys fill the screen" if area else ("whole-key pictures" if key else "small pictures")))
        self.upload.update(running=True, pct=0, text="Hazırlanıyor…", error="")

        def work():
            try:
                self.device.upload_deck(data, lambda pct, text: self.upload.update(pct=pct, text=text))
                self.upload.update(running=False, pct=100, text="Tuşlar karta gönderildi.")
            except Exception as e:  # noqa: BLE001
                self.log("keys: %s" % e)
                self.upload.update(running=False, error=str(e) or "Gönderilemedi")

        threading.Thread(target=work, name="upload", daemon=True).start()
        return True
