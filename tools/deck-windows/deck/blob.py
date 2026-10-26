"""The key set sent to the card. Format: firmware include/services/deck_store.h.

'D' 'K' 1 12 icon_px volume_steps, 12 x 28-byte slots (kind, code, mods, has_icon,
label[24] UTF-8), then the pictures (icon_px^2 RGB565 little endian, magenta = see-through).
"""

import base64
import io
import struct

from PIL import Image

KIND_EMPTY, KIND_MEDIA, KIND_KEY, KIND_HOST = 0, 1, 2, 3
MOD_CTRL, MOD_SHIFT, MOD_ALT, MOD_GUI = 1, 2, 4, 8
TRANSPARENT = 0xF81F
KEY_BG = (36, 40, 73)  # a key on the card's background, for soft picture edges

# Arduino USBHIDKeyboard codes for keys that are not plain characters.
_NAMED = {
    "enter": 0xB0, "esc": 0xB1, "escape": 0xB1, "backspace": 0xB2, "tab": 0xB3, "space": 0x20,
    "insert": 0xD1, "home": 0xD2, "pageup": 0xD3, "delete": 0xD4, "del": 0xD4, "end": 0xD5,
    "pagedown": 0xD6, "right": 0xD7, "left": 0xD8, "down": 0xD9, "up": 0xDA,
    "printscreen": 0xCE, "prtsc": 0xCE,
}
for _i in range(1, 13):
    _NAMED["f%d" % _i] = 0xC2 + _i - 1
for _i in range(13, 25):
    _NAMED["f%d" % _i] = 0xF0 + _i - 13
_MODS = {"ctrl": MOD_CTRL, "control": MOD_CTRL, "shift": MOD_SHIFT, "alt": MOD_ALT,
         "win": MOD_GUI, "windows": MOD_GUI, "cmd": MOD_GUI}


def parse_combo(combo):
    """'Ctrl+Shift+S' -> (mods, key code) for the card's keyboard, or None."""
    mods, key = 0, None
    for part in [p.strip().lower() for p in str(combo).replace(" ", "").split("+") if p.strip()]:
        if part in _MODS:
            mods |= _MODS[part]
        elif key is None and len(part) == 1 and part.isascii() and part.isalnum():
            key = ord(part)
        elif key is None and part in _NAMED:
            key = _NAMED[part]
        else:
            return None
    return (mods, key) if key is not None else None


def slot_kind(i, action):
    """(kind, code, mods) of one key for the card."""
    t = action.get("type", "none")
    if t == "none":
        return KIND_EMPTY, 0, 0
    if t == "media":
        return KIND_MEDIA, int(action.get("code", 3)) & 0xFF, 0
    if t == "keys":
        parsed = parse_combo(action.get("combo", ""))
        if parsed:  # the card sends it itself: works without this program too
            return KIND_KEY, parsed[1], parsed[0]
    return KIND_HOST, i, 0


def label_bytes(label):
    b = (label or "").encode("utf-8")
    if len(b) > 23:
        n = 23
        while n > 0 and (b[n] & 0xC0) == 0x80:
            n -= 1
        b = b[:n]
    return b + b"\0" * (24 - len(b))


def image_from_data_url(url):
    if not url or "," not in url:
        return None
    try:
        raw = base64.b64decode(url.split(",", 1)[1])
        return Image.open(io.BytesIO(raw)).convert("RGBA")
    except Exception:  # noqa: BLE001 - a broken picture just stays off the key
        return None


def icon_pixels(img, px):
    """px x px RGB565 little endian, magenta where transparent."""
    w, h = img.size
    k = min(px / w, px / h)
    fit = img.resize((max(1, round(w * k)), max(1, round(h * k))), Image.LANCZOS)
    canvas = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    canvas.paste(fit, ((px - fit.size[0]) // 2, (px - fit.size[1]) // 2), fit)
    out = bytearray()
    for r, g, b, a in canvas.getdata():
        if a < 64:
            v = TRANSPARENT
        else:
            r = (r * a + KEY_BG[0] * (255 - a)) // 255
            g = (g * a + KEY_BG[1] * (255 - a)) // 255
            b = (b * a + KEY_BG[2] * (255 - a)) // 255
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            if v == TRANSPARENT:
                v = 0xF81E
        out += struct.pack("<H", v)
    return bytes(out)


def build(slots, icon_px, volume_steps):
    head = bytearray(b"DK" + bytes([1, len(slots), icon_px & 0xFF, max(4, min(100, int(volume_steps)))]))
    icons = []
    for i, s in enumerate(slots):
        kind, code, mods = slot_kind(i, s.get("action", {}))
        img = image_from_data_url(s.get("img", ""))
        has = 1 if img is not None else 0
        if has:
            icons.append(icon_pixels(img, icon_px))
        head += bytes([kind, code & 0xFF, mods & 0xFF, has]) + label_bytes(s.get("label", ""))
    return bytes(head) + b"".join(icons)
