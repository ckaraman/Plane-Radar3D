"""System 2 data engine: the PC downloads the aircraft around the card and sends them over
USB ready to draw (docs/PROTOCOL.md). Moved here from tools/usb-kopru*/radar_kopru.py.

Feed(card, fetch).tick(now) does whatever is due and returns. card is device.Device (or
the fake card in the tests), fetch(url, timeout) -> parsed JSON (get_json or a canned
answer in the tests).
"""

import json
import math
import unicodedata
import urllib.error
import urllib.request

INTERVAL_SEC = 5
MAX_AIRCRAFT = 64        # services::adsb::kMaxAircraft
ROUTE_LOOKUPS = 6        # adsbdb lookups per batch (the rest wait for the next ones)
AOK_TIMEOUT = 3
DEFAULT_FETCH_KM = 28    # cards that do not send prefs.fetch_km
USER_AGENT = "PlaneRadarDeck"
SOURCES = [
    ("adsb.fi", "https://opendata.adsb.fi/api/v3/lat/{la}/lon/{lo}/dist/{nm}"),
    ("airplanes.live", "https://api.airplanes.live/v2/point/{la}/{lo}/{nm}"),
    ("adsb.lol", "https://api.adsb.lol/v2/point/{la}/{lo}/{nm}"),
]
TR_LETTERS = "çÇğĞıİöÖşŞüÜ"


class HttpError(Exception):
    def __init__(self, msg, status=None):
        super().__init__(msg)
        self.status = status


def get_json(url, timeout=10):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        raise HttpError("HTTP %d" % e.code, e.code)
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise HttpError(str(getattr(e, "reason", e)) or "bağlantı hatası")


def num(v):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def num_or(v, default):
    n = num(v)
    return default if n is None else n


def fold_city(text):
    """Turkish letters stay (the card font has them), other accents are dropped."""
    out = []
    for ch in str(text or ""):
        if ch in TR_LETTERS:
            out.append(ch)
            continue
        for d in unicodedata.normalize("NFD", ch):
            if unicodedata.category(d) != "Mn":
                out.append(d)
    s = "".join(c for c in "".join(out)
                if c.isascii() and (c.isalnum() or c in " .-") or c in TR_LETTERS).strip()
    return s[:19]


class Feed:
    def __init__(self, card, fetch=get_json, log=print):
        self.card, self.fetch, self.log = card, fetch, log
        self.routes = {}     # callsign -> "FROM,FromCity,TO,ToCity" or "-"
        self.src = 0         # the source that answered last is tried first
        self.next_air = 0.0
        self.state = "kart yok"

    def tick(self, now):
        h = self.card.hello if self.card.connected else None
        if not h:
            self.state = "kart yok"
            return
        if h.get("sys") != 2 or not h.get("feedv"):
            self.state = "kart eski"  # system 1 or a firmware before 0.41: it fetches itself
            return
        if now < self.next_air:
            return
        self.next_air = now + INTERVAL_SEC
        lat, lon = num(h.get("lat")), num(h.get("lon"))
        if lat is None or lon is None:
            self.state = "konum yok"
            return
        km = num((h.get("prefs") or {}).get("fetch_km")) or DEFAULT_FETCH_KM
        try:
            lst, name = self._traffic(lat, lon, km)
        except HttpError as e:
            # Nothing is sent: the card empties the radar after 20 s and shows the clock.
            if self.state != "internet yok":
                self.log("feed: %s" % e)
            self.state = "internet yok"
            return
        lines = self._lines(lst, lat, lon)
        ack = self.card.send_batch(["PR:ABEGIN"] + lines, "PR:AEND %d" % int(now), "AOK", AOK_TIMEOUT)
        # No AOK: this batch is dropped, the next one goes out on time (no backlog).
        self.state = "%d uçak (%s)" % (len(lines), name) if ack else "kart cevap vermedi"

    def _traffic(self, lat, lon, km):
        nm = max(1, min(250, int(math.ceil(km / 1.852))))
        last = "kaynak yok"
        for k in range(len(SOURCES)):
            idx = (self.src + k) % len(SOURCES)
            name, tmpl = SOURCES[idx]
            try:
                r = self.fetch(tmpl.format(la="%.4f" % lat, lo="%.4f" % lon, nm=nm), 10)
                lst = r.get("ac")
                if lst is None:
                    lst = r.get("aircraft")
            except (HttpError, AttributeError) as e:  # AttributeError: not a JSON object
                last = "%s: %s" % (name, e)
                continue
            self.src = idx
            return (lst or []), name
        raise HttpError(last)

    def _route(self, cs):
        """'FROM,FromCity,TO,ToCity', '-' (no route), or None (try again later)."""
        try:
            r = self.fetch("https://api.adsbdb.com/v0/callsign/" + cs, 6)
        except HttpError as e:
            return "-" if e.status == 404 else None
        resp = r.get("response") if isinstance(r, dict) else None
        fr = resp.get("flightroute") if isinstance(resp, dict) else None
        if not fr:
            return "-"
        o, d = fr.get("origin") or {}, fr.get("destination") or {}
        fc = "".join(c for c in str(o.get("iata_code") or o.get("icao_code") or "") if c.isalnum())
        tc = "".join(c for c in str(d.get("iata_code") or d.get("icao_code") or "") if c.isalnum())
        if not fc and not tc:
            return "-"
        return "%s,%s,%s,%s" % (fc, fold_city(o.get("municipality")), tc, fold_city(d.get("municipality")))

    def _lines(self, lst, lat0, lon0):
        """PR:A lines, nearest first (field order: src/core/services/usb_feed.cpp)."""
        items = []
        for a in lst:
            if not isinstance(a, dict):
                continue
            la, lo = num(a.get("lat")), num(a.get("lon"))
            if la is None or lo is None or isinstance(a.get("alt_baro"), str):  # "ground"
                continue
            alt = num_or(a.get("alt_baro"), num_or(a.get("alt_geom"), 0))
            vr = num_or(a.get("baro_rate"), num_or(a.get("geom_rate"), 0))
            trk = num_or(a.get("track"), num_or(a.get("true_heading"), 0))
            gs = num_or(a.get("gs"), num_or(a.get("tas"), 0))
            cs = str(a.get("flight") or "").strip() or str(a.get("hex") or "")
            cs = "".join(c for c in cs if c.isalnum() or c == "-")[:8]
            ty = "".join(c for c in str(a.get("t") or "") if c.isalnum())[:4]
            dx = (lo - lon0) * math.cos(math.radians(lat0))
            dy = la - lat0
            line = "PR:A %.5f,%.5f,%.0f,%.0f,%.0f,%.0f,%s,%s" % (la, lo, trk, gs, alt, vr, cs, ty)
            items.append((dx * dx + dy * dy, cs, line))
        items.sort(key=lambda t: t[0])
        sel = items[:MAX_AIRCRAFT]
        if len(self.routes) > 5000:  # shortcut: forget all once a long day has filled it
            self.routes.clear()
        lookups = 0
        for _, cs, _ in sel:
            if lookups >= ROUTE_LOOKUPS:
                break
            if cs and cs not in self.routes:
                lookups += 1
                rt = self._route(cs)
                if rt is not None:
                    self.routes[cs] = rt
        return [line + ("," + self.routes[cs] if cs in self.routes else "") for _, cs, line in sel]
