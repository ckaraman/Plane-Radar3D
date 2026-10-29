#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NOT: 2. sistemde Plane Radar Deck 1.5.0+ bu işi kendisi yapar (helper/deck/feed.py).
İkisini aynı anda açmayın. Aşama 10'da silinecek.

Radar USB köprüsü — macOS (Windows'taki usb-koprusu.ps1'in aynısı).

Uçuş verisini (adsb.fi / airplanes.live / adsb.lol), rotaları (adsbdb.com) ve hava
durumunu (Open-Meteo) internetten indirir, USB seri port üzerinden Plane Radar 3D
cihazına gönderir. Ek kurulum gerekmez: yalnızca macOS'teki python3 ve curl.

Protokol: PR:ABEGIN / PR:A lat,lon,track,gs,alt,vrate,callsign,type[,rota] / PR:AEND <unix>
          PR:W ... (hava durumu, 10 dakikada bir)

Çalıştırma:  python3 radar_kopru.py            (portu kendisi bulur)
             python3 radar_kopru.py /dev/cu.usbserial-0001
"""
import fcntl
import glob
import json
import math
import os
import select
import struct
import subprocess
import sys
import termios
import time
import unicodedata

INTERVAL_SEC = 5
RADIUS_NM = 15            # yaklaşık 28 km (15 km menzil için fazlasıyla yeterli)
WEATHER_EVERY_SEC = 600
USER_AGENT = "plane-radar-3d-usb-mac"
SOURCES = [
    ("adsb.fi", "https://opendata.adsb.fi/api/v3/lat/{la}/lon/{lo}/dist/{nm}"),
    ("airplanes.live", "https://api.airplanes.live/v2/point/{la}/{lo}/{nm}"),
    ("adsb.lol", "https://api.adsb.lol/v2/point/{la}/{lo}/{nm}"),
]
TR_LETTERS = "çÇğĞıİöÖşŞüÜ"

route_cache = {}   # callsign -> "FROM,FromCity,TO,ToCity" or "-"
src_idx = 0


def say(msg, color=None):
    codes = {"green": "32", "yellow": "33", "red": "31", "cyan": "36"}
    if color and sys.stdout.isatty():
        print("\033[%sm%s\033[0m" % (codes[color], msg), flush=True)
    else:
        print(msg, flush=True)


# ------------------------------------------------------------------ HTTP (curl)

class HttpError(Exception):
    def __init__(self, msg, status=None):
        super().__init__(msg)
        self.status = status


def get_json(url, timeout=10):
    """GET with the system curl (uses the macOS certificate store)."""
    try:
        p = subprocess.run(
            ["curl", "-sS", "-L", "--max-time", str(timeout), "-A", USER_AGENT,
             "-w", "\n%{http_code}", url],
            capture_output=True, timeout=timeout + 5)
    except subprocess.TimeoutExpired:
        raise HttpError("zaman aşımı")
    if p.returncode != 0:
        raise HttpError(p.stderr.decode("utf-8", "replace").strip() or "curl hatası")
    body, _, code = p.stdout.decode("utf-8", "replace").rpartition("\n")
    status = int(code) if code.strip().isdigit() else 0
    if status != 200:
        raise HttpError("HTTP %d" % status, status)
    return json.loads(body)


# ------------------------------------------------------------------ serial port

def find_port():
    pats = ["/dev/cu.usbserial*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*",
            "/dev/cu.usbmodem*"]
    found = []
    for p in pats:
        found += sorted(glob.glob(p))
    if found:
        say("Cihaz bulundu: " + found[0], "green")
        return found[0]
    ports = sorted(glob.glob("/dev/cu.*"))
    raise SystemExit(
        "USB seri port bulunamadı. Kabloyu (veri kablosu olmalı) ve sürücüyü kontrol edin.\n"
        "Bulunan portlar: " + (", ".join(ports) if ports else "yok") + "\n"
        "Port listede varsa elle verin:  python3 radar_kopru.py /dev/cu.XXXX")


class Serial:
    def __init__(self, path):
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            raise SystemExit(
                "%s açılamadı (%s). Kurulum sayfası ya da başka bir program portu "
                "kullanıyorsa kapatın." % (path, e.strerror))
        attrs = termios.tcgetattr(self.fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        iflag = 0
        oflag = 0
        lflag = 0
        cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, cc])
        # Keep DTR/RTS released so the ESP32 is not reset (same as the Windows bridge).
        try:
            bits = struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS)
            fcntl.ioctl(self.fd, termios.TIOCMBIC, bits)
        except (AttributeError, OSError):
            pass
        self.rx = b""

    def write_line(self, text):
        data = (text + "\n").encode("utf-8")
        while data:
            try:
                n = os.write(self.fd, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self.fd], [], 1.0)

    def read_lines(self):
        while True:
            r, _, _ = select.select([self.fd], [], [], 0)
            if not r:
                break
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            if not chunk:
                break
            self.rx += chunk
        out = []
        while b"\n" in self.rx:
            line, self.rx = self.rx.split(b"\n", 1)
            out.append(line.decode("utf-8", "replace").rstrip("\r"))
        if len(self.rx) > 8000:
            self.rx = b""
        return out

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


# ------------------------------------------------------------------ data

def num(v):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def num_or(v, default):
    n = num(v)
    return default if n is None else n


def fold_city(text):
    """Turkish letters stay (the device font has them), other accents are dropped."""
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


def get_traffic(lat, lon):
    global src_idx
    last = "kaynak yok"
    for k in range(len(SOURCES)):
        idx = (src_idx + k) % len(SOURCES)
        name, tmpl = SOURCES[idx]
        url = tmpl.format(la="%.4f" % lat, lo="%.4f" % lon, nm=RADIUS_NM)
        try:
            r = get_json(url)
            src_idx = idx
            lst = r.get("ac")
            if lst is None:
                lst = r.get("aircraft")
            return (lst or []), name
        except Exception as e:  # noqa: BLE001 - try the next source
            last = "%s: %s" % (name, e)
    raise HttpError(last)


def get_route(cs):
    """'FROM,FromCity,TO,ToCity', '-' (no route), or None (temporary error)."""
    try:
        r = get_json("https://api.adsbdb.com/v0/callsign/" + cs, timeout=6)
    except HttpError as e:
        return "-" if e.status == 404 else None
    except Exception:  # noqa: BLE001
        return None
    fr = (r.get("response") or {}).get("flightroute") if isinstance(r.get("response"), dict) else None
    if not fr:
        return "-"
    o, d = fr.get("origin") or {}, fr.get("destination") or {}
    fc = "".join(c for c in str(o.get("iata_code") or o.get("icao_code") or "") if c.isalnum())
    tc = "".join(c for c in str(d.get("iata_code") or d.get("icao_code") or "") if c.isalnum())
    if not fc and not tc:
        return "-"
    return "%s,%s,%s,%s" % (fc, fold_city(o.get("municipality")), tc, fold_city(d.get("municipality")))


def build_lines(lst, lat0, lon0):
    items = []
    for a in lst:
        la, lo = num(a.get("lat")), num(a.get("lon"))
        if la is None or lo is None:
            continue
        if isinstance(a.get("alt_baro"), str):   # "ground"
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
    sel = items[:64]
    lookups = 0
    for _, cs, _ in sel:
        if lookups >= 6:
            break
        if cs and cs not in route_cache:
            rt = get_route(cs)
            lookups += 1
            if rt is not None:
                route_cache[cs] = rt
    return [line + ("," + route_cache[cs] if cs in route_cache else "") for _, cs, line in sel]


def weather_line(lat, lon):
    url = ("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
           "is_day,wind_speed_10m,wind_direction_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,sunrise,sunset,wind_speed_10m_max,"
           "wind_direction_10m_dominant,precipitation_sum,relative_humidity_2m_mean"
           "&forecast_days=5&timezone=auto&timeformat=unixtime" % (lat, lon))
    r = get_json(url)
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


# ------------------------------------------------------------------ main

def ask_location(sp):
    lat, lon = 41.0082, 28.9784
    sp.write_line("PR:HELLO")
    until = time.time() + 4
    while time.time() < until:
        for line in sp.read_lines():
            if line.startswith("PR:HELLO "):
                try:
                    d = json.loads(line[9:])
                    if d.get("lat") is not None and d.get("lon") is not None:
                        lat, lon = float(d["lat"]), float(d["lon"])
                    say("Cihaz yazılımı %s, konum %.4f, %.4f" % (d.get("fw"), lat, lon), "green")
                except ValueError:
                    pass
                return lat, lon
        time.sleep(0.1)
    say("Cihaz HELLO cevabı vermedi; varsayılan konum (İstanbul) kullanılıyor.", "yellow")
    return lat, lon


def main():
    say("")
    say("=== Radar USB köprüsü (Mac) ===", "cyan")
    say("Bu pencere açık kaldığı sürece uçuş verisi cihaza gönderilir. Durdurmak için Ctrl+C.")
    say("Kurulum sayfası ve Arduino IDE kapalı olmalı (port tek programa açılır).")
    say("")
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    sp = Serial(port)
    say(port + " açıldı.", "green")
    time.sleep(1.5)
    sp.read_lines()
    lat, lon = ask_location(sp)
    say("")
    weather_next = 0.0
    try:
        while True:
            stamp = time.strftime("%H:%M:%S")
            try:
                lst, name = get_traffic(lat, lon)
                lines = build_lines(lst, lat, lon)
                sp.write_line("PR:ABEGIN")
                for i, line in enumerate(lines):
                    sp.write_line(line)
                    if i % 6 == 5:
                        time.sleep(0.04)
                sp.write_line("PR:AEND %d" % int(time.time()))
                time.sleep(0.6)
                ack = "?"
                for line in sp.read_lines():
                    if line.startswith("PR:AOK "):
                        try:
                            ack = json.loads(line[7:]).get("n", "?")
                        except ValueError:
                            pass
                say("[%s] %s: %d uçak gönderildi, cihaz aldı: %s" % (stamp, name, len(lines), ack))
            except Exception as e:  # noqa: BLE001 - keep running
                say("[%s] Veri alınamadı: %s" % (stamp, e), "yellow")
            if time.time() >= weather_next:
                try:
                    sp.write_line(weather_line(lat, lon))
                    weather_next = time.time() + WEATHER_EVERY_SEC
                    say("[%s] Hava durumu gönderildi" % time.strftime("%H:%M:%S"), "cyan")
                except Exception as e:  # noqa: BLE001
                    weather_next = time.time() + 60   # bir dakika sonra tekrar dene
                    say("Hava durumu alınamadı: %s" % e, "yellow")
            time.sleep(INTERVAL_SEC)
    except KeyboardInterrupt:
        say("\nKöprü durduruldu.")
    finally:
        sp.close()


if __name__ == "__main__":
    main()
