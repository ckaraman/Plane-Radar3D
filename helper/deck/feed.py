"""System 2 data engine: the PC downloads the aircraft around the card and the weather and
sends them over USB ready to draw (docs/PROTOCOL.md). The card's clock follows the AEND time. Moved here from tools/usb-kopru*/radar_kopru.py.

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
WEATHER_EVERY_SEC = 600
WEATHER_RETRY_SEC = 60
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
        self.next_wx = 0.0
        self._wx_hello = self._wx_at = None  # the connection / location the weather went to
        self.state = "kart yok"

    def tick(self, now):
        h = self.card.hello if self.card.connected else None
        if not h:
            self.state = "kart yok"
            return
        if h.get("sys") != 2 or not h.get("feedv"):
            self.state = "kart eski"  # system 1 or a firmware before 0.41: it fetches itself
            return
        lat, lon = num(h.get("lat")), num(h.get("lon"))
        if lat is None or lon is None:
            self.state = "konum yok"
            return
        self._weather_tick(h, lat, lon, now)
        if now < self.next_air:
            return
        self.next_air = now + INTERVAL_SEC
        # prefs: from HELLO, replaced by the card's PR:PREFS when the range changes (device.py)
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

    def _weather_tick(self, h, lat, lon, now):
        """PR:W every 10 minutes, at once on a new connection (the card may have restarted)
        or a new location; a minute later after an error."""
        if not h.get("wxv") or not h.get("weather", True):
            return
        if h is self._wx_hello and (lat, lon) == self._wx_at and now < self.next_wx:
            return
        self._wx_hello, self._wx_at = h, (lat, lon)
        try:
            line = self._weather_line(lat, lon)
        except (HttpError, AttributeError, TypeError) as e:  # the last two: an odd answer
            self.log("weather: %s" % e)
            self.next_wx = now + WEATHER_RETRY_SEC
            return
        ok = self.card.send_batch([line])
        self.next_wx = now + (WEATHER_EVERY_SEC if ok else WEATHER_RETRY_SEC)

    def _weather_line(self, lat, lon):
        """Open-Meteo forecast as the card's PR:W line (src/core/services/weather_parse.h)."""
        r = self.fetch(
            "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
            "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
            "is_day,wind_speed_10m,wind_direction_10m"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
            "precipitation_probability_max,sunrise,sunset,wind_speed_10m_max,"
            "wind_direction_10m_dominant,precipitation_sum,relative_humidity_2m_mean"
            "&forecast_days=5&timezone=auto&timeformat=unixtime" % (lat, lon), 10)
        c, d = r.get("current") or {}, r.get("daily") or {}
        t = num(c.get("temperature_2m"))
        if t is None:
            raise HttpError("hava durumu cevabı beklenmedik")

        def at(key, i, default):
            arr = d.get(key) or []
            return num_or(arr[i], default) if i < len(arr) else default

        head = "PR:W %.0f %.1f %.0f %.1f %.0f %.0f %.1f %.0f %.0f %.0f" % (
            num_or(r.get("utc_offset_seconds"), 0), t, num_or(c.get("relative_humidity_2m"), -1),
            num_or(c.get("apparent_temperature"), t), num_or(c.get("weather_code"), 3),
            num_or(c.get("is_day"), 1), num_or(c.get("wind_speed_10m"), -1),
            num_or(c.get("wind_direction_10m"), -1), at("sunrise", 0, 0), at("sunset", 0, 0))
        days = []
        for i in range(len(d.get("time") or [])):
            if len(days) >= 5:
                break
            code, mx, mn = at("weather_code", i, None), at("temperature_2m_max", i, None), at("temperature_2m_min", i, None)
            if code is None or mx is None or mn is None:
                continue
            days.append("%.0f,%.0f,%.1f,%.1f,%.0f,%.0f,%.0f,%.1f,%.0f" % (
                at("time", i, 0), code, mx, mn, at("precipitation_probability_max", i, -1),
                at("wind_speed_10m_max", i, -1), at("wind_direction_10m_dominant", i, -1),
                at("precipitation_sum", i, -1), at("relative_humidity_2m_mean", i, -1)))
        return head + " %d " % len(days) + " ".join(days)

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
