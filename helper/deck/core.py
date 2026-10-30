"""Ties it together: card <-> volume, what is playing, key actions."""

import json
import os
import threading
import time

from . import blob, browser_link, config, firmware, queue_card
from .config import Config
from .perf import PerfLog
from .device import Device
from .feed import Feed
from .media import cover_jpeg
from .plat import actions, media, volume


class Core:
    def __init__(self, log=print):
        self.log = log
        self.config = Config()
        self.perf = PerfLog(os.path.join(config.APP_DIR, "perf.log"))
        self.device = Device(self._card_event, log=log, perf=self.perf.write)
        self.volume = volume.VolumeWatcher(self._volume_changed, log=log)
        self.media = media.MediaWatcher(self._media_changed, log=log)
        try:
            self.ext_folder, self.ext_updated = browser_link.install_files()
        except OSError as e:
            self.ext_folder, self.ext_updated = browser_link.extension_dir(), False
            log("extension files: %s" % e)
        self.link = browser_link.BrowserLink(self._queue_changed, log=log)
        self.queue = queue_card.QueueSender(self, log=log)
        self.now = {"none": True}
        self._cover = None          # (jpeg, track id, w, h, version) for the card
        self._cover_ver = 0
        self._cover_sent = None     # version the card has
        self._cover_lock = threading.Lock()
        self._hq_key = None          # (track id, address) of the large cover asked for
        self._hq_for_id = None       # track id that has its large cover
        self._last_now_sent = 0.0
        self._was_connected = False
        self.upload = {"running": False, "pct": 0, "text": "", "error": ""}
        # New card firmware on the setup site (stage 8c): offered, never installed unasked.
        self.fw = {"latest": None, "offer": False, "skipped": False, "running": False,
                   "pct": 0, "text": "", "error": "", "done": ""}
        self.on_fw_offer = lambda version: None  # the tray's notification
        self._fw_told = None
        threading.Thread(target=self._fw_loop, name="firmware", daemon=True).start()
        threading.Thread(target=self._pump, name="pump", daemon=True).start()
        threading.Thread(target=self._heartbeat, name="heartbeat", daemon=True).start()
        self.feed = Feed(self.device, log=log)
        threading.Thread(target=self._feed_loop, name="feed", daemon=True).start()

    def _feed_loop(self):
        """System 2 cards: aircraft, routes and weather from the internet (feed.py)."""
        while True:
            time.sleep(1.0)
            if self.device.busy:  # key upload: the card is in setup mode
                continue
            try:
                self.feed.tick(time.time())
            except Exception as e:  # noqa: BLE001 - keep feeding
                self.log("feed: %s" % e)

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
        elif tag == "QPLAY":  # a song from the queue panel on the card
            self.link.play(int(data.get("x", -1)), str(data.get("t") or ""))
        elif tag == "PERF":  # the card's timing report (stage 10a)
            self.log("perf: %s" % json.dumps(data))
            self.perf.write("card " + json.dumps(data, separators=(",", ":")))

    def _run_action(self, action):
        try:
            if action.get("type") == "site" and self._open_in_tab(action):
                return
            actions.run(action)
        except Exception as e:  # noqa: BLE001
            self.log("action: %s" % e)

    def _open_in_tab(self, action):
        """A site already open in the browser (with the extension): its tab to the front,
        no new one. False: open it the usual way."""
        url = (action.get("url") or "").strip()
        if not url:
            return False
        reuse = action.get("reuse")
        if reuse is None:
            reuse = browser_link.is_music_url(url)
        if not reuse:
            return False
        if "://" not in url:
            url = "https://" + url
        browser = action.get("browser", "")
        default_exe = None if browser else actions.default_browser_exe()
        allow = getattr(actions, "allow_foreground", None)
        if allow:
            allow()  # Windows: let the browser come to the front
        if not self.link.open_url(url, browser, default_exe):
            return False
        front = getattr(actions, "_front_window_of", None)
        exe = None
        if browser:
            for b in actions.browsers():
                if b["id"] == browser:
                    exe = os.path.basename(b["path"])
        else:
            exe = default_exe
        if front and exe and exe.lower().endswith(".exe"):
            time.sleep(0.15)
            front(exe)
        return True

    def _queue_changed(self, q):
        self.queue.changed()
        self._check_hq_cover()

    # ------------------------------------------------------------------ to the card
    def _volume_changed(self, level, muted):
        if self.config.settings.get("send_volume", True):
            self.device.send_json("VOL", {"v": level, "m": muted})

    def _media_changed(self, state, cover):
        self.now = state
        self.queue.changed()  # the queue shows only while its song plays
        if cover is not None and not state.get("none") and self._hq_for_id != state.get("id"):
            if self._hq_likely(state):
                # The browser extension brings the large one in a moment: the small one only
                # if it does not.
                def later(raw=cover, st=dict(state)):
                    time.sleep(3)
                    if self._hq_for_id != st.get("id") and self.now.get("id") == st.get("id"):
                        self._make_cover(raw, st)
                threading.Thread(target=later, name="cover-wait", daemon=True).start()
            else:
                threading.Thread(target=self._make_cover, args=(cover, dict(state)), name="cover", daemon=True).start()
        self._send_now()
        self._check_hq_cover()

    def _hq_likely(self, state):
        art = self.link.art
        return bool(self.link.status()["connected"] and art and queue_card.same_song(art["now"], state.get("t")))

    def _check_hq_cover(self):
        """YouTube Music in the browser (with the extension): its large cover instead of
        the small copy the system's media info carries."""
        art, now = self.link.art, self.now
        if not art or now.get("none") or not queue_card.same_song(art["now"], now.get("t")):
            return
        key = (now.get("id"), art["art"])
        if key == self._hq_key:
            return
        self._hq_key = key
        st = dict(now)

        def work():
            try:
                raw = queue_card.fetch_picture(art["art"])
            except Exception as e:  # noqa: BLE001 - offline
                self.log("large cover: %s" % e)
                raw = None
            if raw and self.now.get("id") == st.get("id"):
                self._hq_for_id = st.get("id")
                self._make_cover(raw, st)
                self.log("large cover for %s" % st.get("t"))
            elif not raw:
                self._hq_key = None
        threading.Thread(target=work, name="cover-large", daemon=True).start()

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

    def _heartbeat(self):
        """Volume every 2 s, now playing every 5 s: on their own, so a cover or queue on its
        way never holds them up (the card shows its own guess when they stop coming)."""
        last_vol = 0.0
        while True:
            time.sleep(0.25)
            dev = self.device
            if not dev.connected or dev.busy:
                last_vol, self._last_now_sent = 0.0, 0.0
                continue
            if time.time() - last_vol >= 2.0:
                last_vol = time.time()
                if self.config.settings.get("send_volume", True):
                    dev.send_json("VOL", {"v": self.volume.level, "m": self.volume.muted})
                else:
                    dev.send_json("VOL", {"v": 50, "m": False})
            if time.time() - self._last_now_sent >= 5.0:
                self._send_now()

    def _pump(self):
        """The cover once per track, the queue: transfers that take a while."""
        while True:
            time.sleep(0.25)
            dev = self.device
            if not dev.connected or dev.busy:
                self._was_connected = False
                continue
            if not self._was_connected:  # (re)connected: tell it everything
                self._was_connected = True
                if self.config.resend and not self.upload["running"]:
                    self.config.resend = False
                    self.log("keys: previous / play / next moved under the knob, sending the keys")
                    threading.Thread(target=self.send_keys_to_card, daemon=True).start()
                    continue
                self._cover_sent = None
                self.queue.reset()
            c = self._cover
            if c and c[4] != self._cover_sent and c[1] == self.now.get("id") \
                    and self.config.settings.get("music_screen", True) and not self.now.get("none"):
                if dev.send_cover(c[0], c[1], c[2], c[3], abort=lambda: self._cover is not c):
                    self._cover_sent = c[4]
                elif self._cover is c:
                    time.sleep(2)
            self.queue.pump(dev)  # a little each round: the cover and heartbeats go first

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

    # ------------------------------------------------------------------ card firmware
    def _fw_loop(self):
        """Looks for new card firmware when a card comes, then every 6 hours."""
        last, card = 0.0, None
        while True:
            time.sleep(5.0)
            if self.device.busy or self.fw["running"]:
                continue
            fw = (self.device.hello or {}).get("fw") if self.device.connected else None
            if not fw:
                self.fw["offer"] = False  # no card: nothing to offer
                continue
            if fw != card or time.time() - last > 6 * 3600:
                card, last = fw, time.time()
                if not self.check_firmware():
                    last -= 6 * 3600 - 600  # no internet yet (start with Windows): again in 10 minutes

    def check_firmware(self, latest_version=firmware.latest_version):
        """False when the site could not be read."""
        latest = latest_version()
        if latest is None:
            return False
        _, settings = self.config.snapshot()
        st = firmware.state(self.device.hello if self.device.connected else None, latest,
                            settings.get("fw_skip", ""))
        self.fw.update(latest=latest, **st)
        if st["offer"] and not st["skipped"] and self._fw_told != latest:
            self._fw_told = latest
            self.log("firmware: %s on the site, card has %s" % (latest, (self.device.hello or {}).get("fw")))
            try:
                self.on_fw_offer(latest)
            except Exception as e:  # noqa: BLE001 - a notification is not worth a crash
                self.log("firmware notice: %s" % e)
        return True

    def skip_firmware(self):
        """"Not now": no more notices for this version (the window keeps a dim strip)."""
        self.config.skip_firmware(self.fw["latest"] or "")
        self.fw["skipped"] = self.fw["offer"]

    def update_firmware(self):
        """Download and install the offered firmware (in a thread; see self.fw)."""
        version = self.fw["latest"]
        if self.fw["running"] or self.device.busy or not self._fw_fits(version):
            return False
        self.fw.update(running=True, pct=0, text="Yazılım indiriliyor…", error="", done="")

        def work():
            try:
                data = firmware.download(version)
                if not self._fw_fits(version):  # the card may have changed during the download
                    raise RuntimeError("bağlı kart bu yazılıma uygun değil")
                done = self.device.upload_firmware(data, lambda pct, text: self.fw.update(pct=pct, text=text))
                self.log("firmware: card now on %s" % done)
                if done != version:
                    raise RuntimeError("kart yeni sürümü bildirmedi (kartta %s)" % (done or "?"))
                self.fw.update(running=False, pct=100, done=done, offer=False, text="")
            except Exception as e:  # noqa: BLE001 - shown in the window
                self.log("firmware: %s" % e)
                self.fw.update(running=False, error=str(e) or "hata", text="")

        threading.Thread(target=work, name="firmware-update", daemon=True).start()
        return True

    def _fw_fits(self, version):
        """The card on the cable now is the one this firmware is for (system 2, ws5)."""
        return firmware.offer(self.device.hello if self.device.connected else None, version)
