"""What is playing on a Mac (Music, Spotify, YouTube Music in Chrome / Safari, ...).

Since macOS 15.4 only Apple's own programs may read the system's "Now Playing". The
MediaRemote Adapter (github.com/ungive/mediaremote-adapter, BSD 3-Clause) works around
that: Apple's /usr/bin/perl, which is allowed, loads a small helper framework that prints
every change as a JSON line. Both ship inside the app (folder "mra").
"""

import base64
import json
import os
import subprocess
import sys
import threading
import time

from .media import cover_jpeg  # noqa: F401 - the core takes it from the media module

APP_NAMES = {
    "com.apple.music": "Müzik", "com.spotify.client": "Spotify", "com.google.chrome": "Chrome",
    "com.apple.safari": "Safari", "org.mozilla.firefox": "Firefox", "com.microsoft.edgemac": "Edge",
    "com.brave.browser": "Brave", "company.thebrowser.browser": "Arc", "com.operasoftware.opera": "Opera",
    "com.apple.podcasts": "Podcastler", "com.apple.tv": "TV", "org.videolan.vlc": "VLC",
    "com.tidal.desktop": "TIDAL", "com.deezer.deezer-desktop": "Deezer", "com.apple.quicktimeplayerx": "QuickTime",
    "com.vivaldi.vivaldi": "Vivaldi", "com.colliderli.iina": "IINA",
}


def app_name(bundle_id):
    b = (bundle_id or "").lower()
    if b in APP_NAMES:
        return APP_NAMES[b]
    for key, name in APP_NAMES.items():
        if b.startswith(key):
            return name
    return (bundle_id or "").split(".")[-1][:24]


def adapter_paths():
    """(perl script, framework) inside the app, or next to this file when run from source."""
    base = getattr(sys, "_MEIPASS", os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    for root in (os.path.join(base, "mra"), os.path.join(base, "..", "Resources", "mra")):
        script = os.path.join(root, "mediaremote-adapter.pl")
        fw = os.path.join(root, "MediaRemoteAdapter.framework")
        if os.path.exists(script) and os.path.isdir(fw):
            return script, fw
    return None, None


def send_command(cmd_id):
    """MediaRemote command to the playing app: 2 play/pause, 4 next, 5 previous."""
    script, fw = adapter_paths()
    if script:
        subprocess.Popen(["/usr/bin/perl", script, fw, "send", str(int(cmd_id))],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class MediaWatcher:
    """Same interface as the Windows one (media.MediaWatcher): on_change(state, cover|None),
    position() in seconds now."""

    def __init__(self, on_change, log=print):
        self.on_change = on_change
        self.log = log
        self.state = {"none": True}
        self._info = {}
        self._key = None
        self._id = 0
        self._anchor = (0.0, 0.0, 0.0)  # (elapsed s, at epoch s, rate)
        self._cover_hash = None
        self._proc = None
        threading.Thread(target=self._run, name="media", daemon=True).start()

    def position(self):
        st = self.state
        if st.get("none"):
            return 0.0
        pos, at, rate = self._anchor
        if st.get("play"):
            pos += max(0.0, time.time() - at) * (rate or 1.0)
        d = st.get("d") or 0
        return max(0.0, min(pos, d) if d else pos)

    def stop(self):
        p = self._proc
        if p is not None:
            try:
                p.terminate()
            except OSError:
                pass

    def _run(self):
        script, fw = adapter_paths()
        if not script:
            self.log("media: adapter not found")
            return
        while True:
            try:
                self._proc = subprocess.Popen(
                    ["/usr/bin/perl", script, fw, "stream", "--micros", "--debounce=150"],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=1 << 20)
                for raw in self._proc.stdout:
                    try:
                        msg = json.loads(raw)
                    except ValueError:
                        continue
                    if isinstance(msg, dict) and msg.get("type") == "data":
                        self._data(msg.get("payload") or {}, bool(msg.get("diff")))
                err = (self._proc.stderr.read() or b"").decode("utf-8", "replace").strip()
                self.log("media: adapter stopped %s %s" % (self._proc.wait(), err[-200:]))
            except Exception as e:  # noqa: BLE001
                self.log("media: %s" % e)
            if not self.state.get("none"):
                self.state, self._key = {"none": True}, None
                self.on_change(self.state, None)
            time.sleep(3)

    def _data(self, payload, diff):
        if diff:
            for k, v in payload.items():
                if v is None:
                    self._info.pop(k, None)
                else:
                    self._info[k] = v
        else:
            self._info = dict(payload)
        info = self._info
        title = info.get("title") or ""
        if not title:
            if not self.state.get("none"):
                self.state, self._key = {"none": True}, None
                self.on_change(self.state, None)
            return
        artist, album = info.get("artist") or "", info.get("album") or ""
        app = app_name(info.get("parentApplicationBundleIdentifier") or info.get("bundleIdentifier"))
        playing = bool(info.get("playing"))
        dur = int((info.get("durationMicros") or 0) / 1e6)
        key = (title, artist, album, app)
        if key != self._key:
            self._key = key
            self._id = (self._id + 1) % 1000000
            self._cover_hash = None
        el = info.get("elapsedTimeMicros")
        ts = info.get("timestampEpochMicros")
        rate = info.get("playbackRate")
        if el is not None:
            self._anchor = (el / 1e6, (ts / 1e6) if ts else time.time(), float(rate) if rate else 1.0)
        cover = None
        art = info.get("artworkData")
        if art:
            h = hash(art)
            if h != self._cover_hash:
                self._cover_hash = h
                try:
                    cover = base64.b64decode(art)
                except ValueError:
                    cover = None
        self.state = {"t": title, "a": artist, "al": album, "app": app, "d": dur, "play": playing, "id": self._id}
        self.on_change(self.state, cover)
