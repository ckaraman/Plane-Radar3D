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
        self._cover = None          # (jpeg bytes, id) waiting for the card
        self._cover_sent_id = None
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
            st = self.device.status()
            try:
                self._cover = (cover_jpeg(cover, st["w"], st["h"]), state.get("id", 0))
            except Exception as e:  # noqa: BLE001 - a picture Pillow cannot read
                self.log("cover: %s" % e)
        self._send_now()

    def _send_now(self):
        if not self.config.settings.get("music_screen", True):
            msg = {"none": True}
        elif self.now.get("none"):
            msg = {"none": True}
        else:
            msg = dict(self.now)
            msg["back"] = int(self.config.settings.get("back_seconds", 60))
        self.device.send_json("NOW", msg)
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
                self._cover_sent_id = None
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
            if c and c[1] != self._cover_sent_id and self.config.settings.get("music_screen", True) \
                    and not self.now.get("none"):
                if dev.send_cover(c[0], c[1]):
                    self._cover_sent_id = c[1]
                    self._send_now()
                else:
                    time.sleep(3)

    # ------------------------------------------------------------------ editor
    def send_keys_to_card(self):
        """Builds the key set and uploads it (runs in a thread; see self.upload)."""
        if self.upload["running"]:
            return False
        if not self.device.connected:
            self.upload.update(running=False, pct=0, text="", error="Kart bağlı değil.")
            return False
        slots, settings = self.config.snapshot()
        st = self.device.status()
        data = blob.build(slots, int(st.get("deckicon") or 72), settings.get("steps", 50))
        self.upload.update(running=True, pct=0, text="Hazırlanıyor…", error="")

        def work():
            try:
                self.device.upload_deck(data, lambda pct, text: self.upload.update(pct=pct, text=text))
                self.upload.update(running=False, pct=100, text="Tuşlar karta gönderildi.")
            except Exception as e:  # noqa: BLE001
                self.upload.update(running=False, error=str(e) or "Gönderilemedi")

        threading.Thread(target=work, name="upload", daemon=True).start()
        return True
