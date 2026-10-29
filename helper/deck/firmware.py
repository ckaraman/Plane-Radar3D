"""New card firmware: the version on the setup site and the file the card takes over USB
(stage 8c; the upload itself is Device.upload_firmware, as the setup page's usbUpdate)."""

import urllib.error
import urllib.parse
import urllib.request

from . import feed

SITE = "https://ckaraman.github.io/Plane-Radar3D/"


def latest_version(get_json=feed.get_json):
    """The newest system 2 firmware on the site, None when the site cannot be read."""
    try:
        v = get_json(SITE + "version.json").get("version_ws5")
    except (feed.HttpError, AttributeError):
        return None
    return v or None


def offer(hello, latest):
    """Offer the update: a system 2 Waveshare 5" card with another version than the site's."""
    h = hello or {}
    return bool(latest) and h.get("sys") == 2 and h.get("hw") == "ws5" and h.get("fw") != latest


def _fetch_bytes(url):
    req = urllib.request.Request(url, headers={"User-Agent": feed.USER_AGENT, "Cache-Control": "no-cache"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def download(version, fetch=_fetch_bytes):
    """firmware-ws5.bin of that version; RuntimeError with a short reason."""
    try:
        data = fetch(SITE + "firmware-ws5.bin?v=" + urllib.parse.quote(version))
    except urllib.error.HTTPError as e:
        raise RuntimeError("indirme: HTTP %d" % e.code)
    except (urllib.error.URLError, OSError) as e:
        raise RuntimeError("indirme: %s" % (getattr(e, "reason", e) or "bağlantı hatası"))
    if len(data) < 4096 or data[0] != 0xE9:  # 0xE9: the first byte of an ESP app image
        raise RuntimeError("indirilen dosya bozuk")
    return data


def state(hello, latest, skip):
    """What the window and the tray show: an update to offer, and whether it was set aside."""
    o = offer(hello, latest)
    return {"offer": o, "skipped": o and skip == latest}
