"""The "Plane Radar Deck" browser extension (browser-extension/): the YouTube Music queue
from the browser, and sites opened in the tab that already shows them.

The extension connects to ws://127.0.0.1:47822/deck (this computer only). Its id is fixed
by the key in its manifest, and only that origin is let in.
"""

import itertools
import json
import os
import shutil
import sys
import threading
import time

PORT = 47822
EXT_ID = "gpfmcolchbjcnabppopohpbhlcibmkmf"
ORIGIN = "chrome-extension://" + EXT_ID

# Sites where "open" goes to the tab that already plays (the editor ticks it by default).
MUSIC_HOSTS = ("music.youtube.com", "open.spotify.com", "deezer.com", "soundcloud.com", "music.apple.com",
               "listen.tidal.com", "music.amazon.com", "fizy.com", "muud.com.tr", "youtube.com")

# Browser ids of the editor (actions.KNOWN_BROWSERS) -> the brand the extension reports.
BRANDS = {"chrome": "Google Chrome", "edge": "Microsoft Edge", "brave": "Brave", "opera": "Opera",
          "vivaldi": "Vivaldi"}
EXE_BRANDS = {"chrome.exe": "Google Chrome", "msedge.exe": "Microsoft Edge", "brave.exe": "Brave",
              "opera.exe": "Opera", "launcher.exe": "Opera", "vivaldi.exe": "Vivaldi",
              "google chrome.app": "Google Chrome", "microsoft edge.app": "Microsoft Edge",
              "brave browser.app": "Brave", "opera.app": "Opera", "vivaldi.app": "Vivaldi"}


def is_music_url(url):
    from urllib.parse import urlparse

    try:
        host = (urlparse(url if "://" in url else "https://" + url).hostname or "").lower()
    except ValueError:
        return False
    host = host[4:] if host.startswith("www.") else host
    return any(host == h or host.endswith("." + h) for h in MUSIC_HOSTS)


def extension_dir():
    """Where the extension's files live for the browser ("Load unpacked"): a fixed folder
    of this user (the program itself may move or, on Windows, unpack to a new place)."""
    if sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support/PlaneRadarDeck")
    else:
        base = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "PlaneRadarDeck")
    return os.path.join(base, "browser-extension")


def source_dir():
    base = getattr(sys, "_MEIPASS", os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    for p in (os.path.join(base, "browser-extension"), os.path.join(base, "..", "Resources", "browser-extension")):
        if os.path.isfile(os.path.join(p, "manifest.json")):
            return p
    return None


def bundled_version():
    src = source_dir()
    try:
        with open(os.path.join(src, "manifest.json"), encoding="utf-8") as f:
            return json.load(f).get("version")
    except (OSError, ValueError, TypeError):
        return None


def install_files():
    """Copies the extension next to the settings (again when the program brings a newer
    one). -> (folder, updated: True when the files changed)."""
    src, dst = source_dir(), extension_dir()
    if not src:
        return dst, False
    try:
        with open(os.path.join(dst, "manifest.json"), encoding="utf-8") as f:
            old = json.load(f).get("version")
    except (OSError, ValueError):
        old = None
    new = bundled_version()
    if old == new and os.path.isfile(os.path.join(dst, "background.js")):
        return dst, False
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        shutil.copy2(os.path.join(src, name), os.path.join(dst, name))
    return dst, old is not None


class BrowserLink:
    """on_queue(queue dict | None) when the YouTube Music queue changes or goes away."""

    def __init__(self, on_queue=lambda q: None, log=print, port=PORT):
        self.on_queue = on_queue
        self.log = log
        self.port = port
        self.conns = {}          # connection -> {"brand", "v"}
        self.lock = threading.Lock()
        self.queue = None        # newest queue from YouTube Music
        self.queue_conn = None
        self._ids = itertools.count(1)
        self._waiting = {}       # open id -> (event, holder)
        self.error = None
        threading.Thread(target=self._serve, name="browser-link", daemon=True).start()
        threading.Thread(target=self._ping, name="browser-ping", daemon=True).start()

    # ------------------------------------------------------------------ state
    def status(self):
        with self.lock:
            browsers = sorted({c["brand"] for c in self.conns.values()})
            versions = sorted({c.get("v") or "" for c in self.conns.values()})
        q = self.queue or {}
        return {"connected": bool(browsers), "browsers": browsers, "versions": versions,
                "queue": len(q.get("items") or []), "error": self.error, "id": EXT_ID,
                "folder": extension_dir(), "bundled": bundled_version()}

    # ------------------------------------------------------------------ server
    def _serve(self):
        try:
            from websockets.sync.server import serve
        except ImportError as e:
            self.error = "websockets: %s" % e
            return
        while True:
            try:
                with serve(self._handler, "127.0.0.1", self.port, origins=[ORIGIN], ping_interval=None,
                           max_size=4 * 1024 * 1024, logger=None) as server:
                    self.error = None
                    server.serve_forever()
            except OSError as e:  # the port in use (an old copy still running)
                self.error = "port %d: %s" % (self.port, e)
                self.log("browser link: %s" % e)
                time.sleep(10)

    def _handler(self, conn):
        info = {"brand": "?", "v": None}
        with self.lock:
            self.conns[conn] = info
        try:
            for raw in conn:
                try:
                    msg = json.loads(raw)
                except (ValueError, TypeError):
                    continue
                t = msg.get("t")
                if t == "hello":
                    info["brand"], info["v"] = str(msg.get("brand") or "?")[:40], msg.get("v")
                    self.log("browser link: %s, extension %s" % (info["brand"], info["v"]))
                elif t == "queue":
                    self._queue(conn, msg)
                elif t == "opened":
                    w = self._waiting.get(msg.get("id"))
                    if w:
                        w[1].append(msg)
                        w[0].set()
        except Exception:  # noqa: BLE001 - the browser went away
            pass
        finally:
            with self.lock:
                self.conns.pop(conn, None)
                gone = conn is self.queue_conn
            if gone:
                self.queue, self.queue_conn = None, None
                self.on_queue(None)

    def _queue(self, conn, msg):
        items = []
        for it in (msg.get("items") or [])[:200]:
            if isinstance(it, dict) and it.get("t"):
                items.append({"t": str(it.get("t"))[:120], "a": str(it.get("a") or "")[:120],
                              "d": str(it.get("d") or "")[:10], "img": str(it.get("img") or "")[:600]})
        cur = msg.get("cur")
        q = {"items": items, "cur": cur if isinstance(cur, int) else -1, "now": str(msg.get("now") or "")[:120]}
        if not items:
            q = None
        self.queue, self.queue_conn = q, (conn if q else None)
        self.on_queue(q)

    def _ping(self):
        while True:  # also keeps the extension's background page awake
            time.sleep(20)
            self._send_all({"t": "ping"})

    def _send(self, conn, obj):
        try:
            conn.send(json.dumps(obj, ensure_ascii=False))
            return True
        except Exception:  # noqa: BLE001
            return False

    def _send_all(self, obj):
        with self.lock:
            conns = list(self.conns)
        for c in conns:
            self._send(c, obj)

    # ------------------------------------------------------------------ to the browser
    def _pick(self, browser="", default_exe=None):
        """The connection of the wanted browser (editor id, or the default browser)."""
        want = BRANDS.get(browser) if browser else EXE_BRANDS.get((default_exe or "").lower())
        if not browser and default_exe and not want:
            return None  # the default browser takes no extension (Safari, Firefox)
        with self.lock:
            items = list(self.conns.items())
        if want:
            for c, info in items:
                if info["brand"] == want:
                    return c
            return None  # that browser has no extension (or is closed): open as usual
        return items[0][0] if len(items) == 1 else None

    def open_url(self, url, browser="", default_exe=None, timeout=2.5):
        """The site in the tab that already shows it (else a new tab) by the extension.
        False when no extension of that browser answers: the caller opens it as usual."""
        conn = self._pick(browser, default_exe)
        if conn is None:
            return False
        oid = next(self._ids)
        ev, holder = threading.Event(), []
        self._waiting[oid] = (ev, holder)
        try:
            if not self._send(conn, {"t": "open", "url": url, "id": oid}):
                return False
            ev.wait(timeout)
            return bool(holder)
        finally:
            self._waiting.pop(oid, None)

    def play(self, index, title):
        conn = self.queue_conn
        return bool(conn) and self._send(conn, {"t": "play", "i": int(index), "title": title})
