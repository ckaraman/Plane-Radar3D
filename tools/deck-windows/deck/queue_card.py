"""The YouTube Music queue on the card (firmware 0.36+, HELLO "queuev"): the rows from the
current song on, then their small covers one by one, between the other card traffic."""

import collections
import io
import json
import re
import unicodedata
import urllib.request

ROWS = 30
PIC_HOSTS = ("ytimg.com", "googleusercontent.com", "ggpht.com")


def _norm(t):
    t = unicodedata.normalize("NFKD", (t or "").lower())
    t = "".join(ch for ch in t if not unicodedata.combining(ch))
    return re.sub(r"[^a-z0-9]+", " ", t).strip()


def cut_utf8(text, limit):
    b = (text or "").encode("utf-8")
    if len(b) <= limit:
        return text or ""
    b = b[:limit]
    while b and (b[-1] & 0xC0) == 0x80:
        b = b[:-1]
    if b and b[-1] >= 0xC0:
        b = b[:-1]
    return b.decode("utf-8", "ignore")


def card_rows(queue, now):
    """Rows for the card (the current song first) or None: no queue, or it is not what plays."""
    if not queue or now.get("none"):
        return None
    items, cur = queue.get("items") or [], queue.get("cur", -1)
    if not (0 <= cur < len(items)):
        return None
    a, b = _norm(items[cur]["t"]), _norm(now.get("t"))
    if not a or not b or not (a == b or a in b or b in a):
        return None  # something else plays (another app or tab)
    rows = []
    for x in range(cur, min(len(items), cur + ROWS)):
        it = items[x]
        rows.append({"x": x, "t": cut_utf8(it["t"], 62), "a": cut_utf8(it["a"], 46), "d": cut_utf8(it["d"], 8),
                     "img": it.get("img", "")})
    return rows


def fetch_picture(url, limit=4 * 1024 * 1024):
    """A picture from YouTube's picture servers (only those), or None. A video without a
    "maxres" picture falls back to the smaller ones."""
    import urllib.error
    from urllib.parse import urlparse

    host = (urlparse(url).hostname or "").lower()
    if not any(host == h or host.endswith("." + h) for h in PIC_HOSTS):
        return None
    tries = [url]
    if "/maxresdefault.jpg" in url:
        tries += [url.replace("maxresdefault", n) for n in ("sddefault", "hqdefault")]
    for u in tries:
        try:
            with urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "PlaneRadarDeck"}), timeout=8) as r:
                return r.read(limit)
        except urllib.error.HTTPError as e:
            if e.code != 404:
                raise
    return None


def same_song(a, b):
    a, b = _norm(a), _norm(b)
    return bool(a and b and (a == b or a in b or b in a))


def thumb_jpeg(url, px):
    """A square small cover of px x px from YouTube's picture servers, or None."""
    from urllib.parse import urlparse

    from PIL import Image

    host = (urlparse(url).hostname or "").lower()
    if not any(host == h or host.endswith("." + h) for h in PIC_HOSTS):
        return None
    if "googleusercontent.com" in host or "ggpht.com" in host:
        url = re.sub(r"=w\d+-h\d+[^&?]*$", "=w%d-h%d-l90-rj" % (px * 2, px * 2), url)  # a sharper size
    req = urllib.request.Request(url, headers={"User-Agent": "PlaneRadarDeck"})
    with urllib.request.urlopen(req, timeout=6) as r:
        raw = r.read(2 * 1024 * 1024)
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    w, h = img.size
    # YouTube video pictures are 16:9 (or 4:3 with bars): the middle square.
    from .media import _trim_bars

    img = _trim_bars(img)
    w, h = img.size
    s = min(w, h)
    img = img.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s)).resize((px, px), Image.LANCZOS)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=88)
    return out.getvalue()


class QueueSender:
    def __init__(self, core, log=print):
        self.core = core
        self.log = log
        self.dirty = True
        self.sent_key = None
        self.card_has = False
        self.gen = 0
        self.todo = []       # (row, url) small covers still to send
        self.cache = collections.OrderedDict()

    def changed(self):
        self.dirty = True

    def reset(self):
        self.sent_key, self.card_has, self.todo, self.dirty = None, False, [], True

    def rows(self):
        if not self.core.config.settings.get("queue", True):
            return None
        return card_rows(self.core.link.queue, self.core.now)

    def _thumb(self, url, px):
        key = (url, px)
        if key in self.cache:
            self.cache.move_to_end(key)
            return self.cache[key]
        try:
            data = thumb_jpeg(url, px)
        except Exception as e:  # noqa: BLE001 - offline, a picture that is gone
            self.log("queue picture: %s" % e)
            data = None
        self.cache[key] = data
        while len(self.cache) > 300:
            self.cache.popitem(last=False)
        return data

    def pump(self, dev):
        hello = dev.hello or {}
        if not hello.get("queuev"):
            return
        if self.dirty:
            self.dirty = False
            rows = self.rows()
            key = json.dumps(rows, sort_keys=True)
            if key == self.sent_key:
                return
            if rows is None:
                if self.card_has and not dev.clear_queue():
                    self.dirty = True
                    return
                self.card_has, self.todo, self.sent_key = False, [], key
                return
            self.gen = (self.gen + 1) % 60000
            card = [{"x": r["x"], "t": r["t"], "a": r["a"], "d": r["d"]} for r in rows]
            if dev.send_queue(card, self.gen, "YouTube Music"):
                self.sent_key, self.card_has = key, True
                self.todo = [(k, r["img"]) for k, r in enumerate(rows) if r["img"]]
            else:
                self.dirty = True
            return
        if self.todo:
            k, url = self.todo.pop(0)
            px = int(hello.get("qthumb") or 40)
            jpeg = self._thumb(url, px)
            if jpeg:
                dev.send_thumb(jpeg, k, self.gen, abort=lambda: self.dirty)  # a failed one is left out
