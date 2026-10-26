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


def cover_jpeg(raw, w, h, max_bytes=250 * 1024):
    """Album art filling a w x h screen (cropped, not stretched), as a JPEG."""
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    k = max(w / img.width, h / img.height)
    img = img.resize((max(w, round(img.width * k)), max(h, round(img.height * k))), Image.LANCZOS)
    left, top = (img.width - w) // 2, (img.height - h) // 2
    img = img.crop((left, top, left + w, top + h))
    for q in (85, 75, 65, 50):
        out = io.BytesIO()
        img.save(out, "JPEG", quality=q, optimize=True)  # baseline: the card's decoder needs it
        if out.tell() <= max_bytes:
            return out.getvalue()
    return out.getvalue()


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
    """Polls the current media session once a second; on_change(state, cover_bytes|None)."""

    def __init__(self, on_change, log=print):
        self.on_change = on_change
        self.log = log
        self.state = {"none": True}
        self._key = None
        self._id = 0
        threading.Thread(target=self._run, name="media", daemon=True).start()

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
            time.sleep(1.0)

    async def _poll(self, mgr):
        s = mgr.get_current_session()
        if s is None:
            if not self.state.get("none"):
                self.state, self._key = {"none": True}, None
                self.on_change(self.state, None)
            return
        props = await s.try_get_media_properties_async()
        info = s.get_playback_info()
        tl = s.get_timeline_properties()
        playing = info.playback_status == Status.PLAYING
        dur = max(0, int(tl.end_time.total_seconds() - tl.start_time.total_seconds()))
        pos = max(0.0, tl.position.total_seconds())
        if playing and tl.last_updated_time:
            try:
                now = datetime.datetime.now(datetime.timezone.utc)
                pos += max(0.0, (now - tl.last_updated_time).total_seconds())
            except TypeError:
                pass
        title, artist, album = props.title or "", props.artist or "", props.album_title or ""
        app = app_name(s.source_app_user_model_id)
        key = (title, artist, album, app)
        cover = None
        if key != self._key:
            self._key = key
            self._id = (self._id + 1) % 1000000
            if props.thumbnail is not None:
                try:
                    cover = await _read_thumbnail(props.thumbnail)
                except Exception as e:  # noqa: BLE001
                    self.log("cover: %s" % e)
        state = {"t": title, "a": artist, "al": album, "app": app, "d": dur,
                 "p": int(min(pos, dur) if dur else pos), "play": playing, "id": self._id}
        old = self.state
        moved = abs(state["p"] - old.get("p", 0) - (1 if playing else 0)) > 2
        if cover is not None or old.get("none") or any(state[k] != old.get(k) for k in ("t", "a", "al", "app", "d", "play", "id")) or moved:
            self.state = state
            self.on_change(state, cover)
        else:
            self.state = state
