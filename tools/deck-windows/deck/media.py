"""What is playing, from Windows' media sessions (the same source as the volume flyout):
Spotify, YouTube in a browser, the Windows media player and so on."""

import asyncio
import datetime
import io
import threading
import time

from PIL import Image

try:
    from winrt.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as MediaManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as Status,
    )
    from winrt.windows.storage.streams import Buffer, DataReader, InputStreamOptions
except ImportError:  # not on Windows (tests)
    MediaManager = None


APP_NAMES = (("spotify", "Spotify"), ("chrome", "Chrome"), ("msedge", "Edge"), ("firefox", "Firefox"),
             ("zunemusic", "Medya Oynatıcı"), ("vlc", "VLC"), ("itunes", "iTunes"), ("opera", "Opera"),
             ("brave", "Brave"), ("deezer", "Deezer"), ("tidal", "TIDAL"), ("youtube", "YouTube Music"))


def app_name(aumid):
    low = (aumid or "").lower()
    for key, name in APP_NAMES:
        if key in low:
            return name
    stem = (aumid or "").split("!")[0].split("\\")[-1]
    return stem[:-4] if stem.lower().endswith(".exe") else stem[:24]


def _trim_bars(img):
    """Black bars around a video frame (YouTube thumbnails are letterboxed): cut off."""
    gray = img.convert("L")
    w, h = gray.size
    px = gray.load()

    def dark_row(y):
        return max(px[x, y] for x in range(0, w, max(1, w // 64))) < 24

    def dark_col(x):
        return max(px[x, y] for y in range(0, h, max(1, h // 64))) < 24

    top, bottom, left, right = 0, h, 0, w
    while top < h * 0.3 and dark_row(top):
        top += 1
    while bottom > h * 0.7 and dark_row(bottom - 1):
        bottom -= 1
    while left < w * 0.3 and dark_col(left):
        left += 1
    while right > w * 0.7 and dark_col(right - 1):
        right -= 1
    if (top, bottom, left, right) != (0, h, 0, w):
        img = img.crop((left, top, right, bottom))
    return img


def cover_size(raw):
    try:
        return Image.open(io.BytesIO(raw)).size
    except Exception:  # noqa: BLE001
        return (0, 0)


def cover_jpeg(raw, w, h, max_bytes=72 * 1024):
    """Album art for a w x h screen: as large as fits the screen height (the card shows it
    in the middle with fading edges), sharp, as a baseline JPEG. -> (jpeg, width, height)"""
    img = _trim_bars(Image.open(io.BytesIO(raw)).convert("RGB"))
    k = min(w / img.width, h / img.height)
    size = (max(1, min(w, round(img.width * k))), max(1, min(h, round(img.height * k))))
    if size != img.size:
        img = img.resize(size, Image.LANCZOS)
        if k > 1.3:  # made larger: a light sharpening keeps it from looking soft
            from PIL import ImageFilter

            img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))
    for q in (90, 86, 80, 72, 62):
        out = io.BytesIO()
        img.save(out, "JPEG", quality=q, optimize=True, subsampling=0 if q >= 88 else 2)  # baseline
        if out.tell() <= max_bytes:
            break
    return out.getvalue(), img.width, img.height


# ------------------------------------------------------------------ better covers
def _norm(t):
    import re
    import unicodedata

    t = unicodedata.normalize("NFKD", (t or "").lower())
    t = "".join(ch for ch in t if not unicodedata.combining(ch))
    t = re.sub(r"[\(\[].*?[\)\]]", " ", t)  # (Official Video), [Remastered] ...
    t = re.sub(r"\b(feat|ft)\.?\b.*", " ", t)
    t = re.sub(r"\s*-\s*topic$", " ", t)
    return re.sub(r"[^a-z0-9]+", " ", t).strip()


def lookup_cover(title, artist, timeout=4.0):
    """A large cover for title / artist from the iTunes catalogue (free, no account).
    Only when the shop's title and artist match what is playing; None otherwise."""
    import json
    import urllib.parse
    import urllib.request

    t, a = _norm(title), _norm(artist)
    if not t or not a:
        return None
    # YouTube titles often read "Artist - Song".
    if " - " in (title or "") and _norm(title.split(" - ", 1)[0]) == a:
        t = _norm(title.split(" - ", 1)[1])
    q = urllib.parse.urlencode({"term": "%s %s" % (artist, t), "media": "music", "entity": "song", "limit": 10})
    req = urllib.request.Request("https://itunes.apple.com/search?" + q, headers={"User-Agent": "PlaneRadarDeck"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = json.loads(r.read().decode("utf-8", "replace"))
    for res in data.get("results", []):
        rt, ra = _norm(res.get("trackName")), _norm(res.get("artistName"))
        if not rt or not ra or not res.get("artworkUrl100"):
            continue
        if (rt == t or rt.startswith(t) or t.startswith(rt)) and (a in ra or ra in a):
            url = res["artworkUrl100"].replace("100x100bb", "600x600bb")
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "PlaneRadarDeck"}),
                                        timeout=timeout) as r:
                return r.read()
    return None


async def _read_thumbnail(ref):
    stream = await ref.open_read_async()
    size = int(stream.size)
    if size <= 0 or size > 8 * 1024 * 1024:
        return None
    buf = Buffer(size)
    await stream.read_async(buf, size, InputStreamOptions.READ_AHEAD)
    try:
        return bytes(memoryview(buf))[: buf.length]
    except TypeError:
        reader = DataReader.from_buffer(buf)
        data = bytearray(buf.length)
        reader.read_bytes(data)
        return bytes(data)


class MediaWatcher:
    """Polls the current media session once a second; on_change(state, cover_bytes|None).

    Position: browsers (YouTube Music in Chrome, ...) report it only now and then, some
    with a stale "last updated" time. The position is anchored when Windows reports a new
    value or the play state changes and runs on with our own clock in between."""

    def __init__(self, on_change, log=print):
        self.on_change = on_change
        self.log = log
        self.state = {"none": True}
        self._key = None
        self._id = 0
        self._sig = None             # (raw position, playing) last anchored
        self._anchor = (0.0, 0.0)    # (position s, time.monotonic())
        self._cover_hash = None
        self._cover_until = 0.0      # look for a (better) cover until then
        threading.Thread(target=self._run, name="media", daemon=True).start()

    def position(self):
        """Seconds, now (the heartbeat to the card uses it)."""
        st = self.state
        if st.get("none"):
            return 0.0
        pos, at = self._anchor
        if st.get("play"):
            pos += time.monotonic() - at
        d = st.get("d") or 0
        return min(pos, d) if d else pos

    def _run(self):
        if MediaManager is None:
            return
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        mgr = None
        while True:
            try:
                if mgr is None:
                    mgr = loop.run_until_complete(MediaManager.request_async())
                loop.run_until_complete(self._poll(mgr))
            except Exception as e:  # noqa: BLE001 - sessions come and go
                self.log("media: %s" % e)
                mgr = None
                time.sleep(3)
            time.sleep(0.5 if time.monotonic() < self._cover_until else 1.0)

    @staticmethod
    def _elapsed_since(when):
        """Seconds since a Windows timestamp, or None when it makes no sense."""
        if not when:
            return None
        try:
            if when.tzinfo is None:
                when = when.replace(tzinfo=datetime.timezone.utc)
            e = (datetime.datetime.now(datetime.timezone.utc) - when).total_seconds()
        except (TypeError, ValueError, OverflowError, AttributeError):
            return None
        return e if 0 <= e < 6 * 3600 else None

    async def _poll(self, mgr):
        s = mgr.get_current_session()
        if s is None:
            if not self.state.get("none"):
                self.state, self._key, self._sig = {"none": True}, None, None
                self.on_change(self.state, None)
            return
        props = await s.try_get_media_properties_async()
        info = s.get_playback_info()
        tl = s.get_timeline_properties()
        playing = info.playback_status == Status.PLAYING
        dur = max(0, int(tl.end_time.total_seconds() - tl.start_time.total_seconds()))
        raw = max(0.0, tl.position.total_seconds() - tl.start_time.total_seconds())
        title, artist, album = props.title or "", props.artist or "", props.album_title or ""
        app = app_name(s.source_app_user_model_id)
        key = (title, artist, album, app)
        new_track = key != self._key
        if new_track:
            self._key = key
            self._id = (self._id + 1) % 1000000
            self._cover_hash = None
            self._cover_until = time.monotonic() + 15  # browsers send the picture a bit later
            self._sig = None

        # Position: anchor on news from Windows, otherwise keep our own clock running.
        sig = (round(raw, 1), playing)
        moved = False
        if sig != self._sig:
            first = self._sig is None
            resumed = not first and playing != self._sig[1]
            self._sig = sig
            e = self._elapsed_since(tl.last_updated_time) if playing else None
            if e is not None and dur and raw + e > dur + 2:
                e = None
            if first:
                pos = raw + (e or 0.0)  # we may have started in the middle of a song
            elif resumed:
                pos = raw + (e if e is not None and e < 3 else 0.0)
            else:
                # Same state, a new number: a seek, or just a late report of where it is.
                expected = self.position()
                cand = raw + (e or 0.0)
                pos = None if (abs(raw - expected) <= 2 or abs(cand - expected) <= 2) else cand
            if pos is not None:
                self._anchor = (pos, time.monotonic())
                moved = True

        cover = None
        if props.thumbnail is not None and (self._cover_hash is None or time.monotonic() < self._cover_until):
            try:
                data = await _read_thumbnail(props.thumbnail)
            except Exception as e:  # noqa: BLE001
                data = None
                self.log("cover: %s" % e)
            if data:
                h = hash(data)
                if h != self._cover_hash:  # a first or a changed picture for this track
                    self._cover_hash = h
                    cover = data
        state = {"t": title, "a": artist, "al": album, "app": app, "d": dur, "play": playing, "id": self._id}
        old = self.state
        changed = old.get("none") or any(state[k] != old.get(k) for k in ("t", "a", "al", "app", "d", "play", "id"))
        self.state = state
        if cover is not None or changed or moved:
            self.on_change(state, cover)
