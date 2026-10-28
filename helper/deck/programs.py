"""Programs for the "Program seç" list (Start menu shortcuts) and their icons."""

import base64
import io
import os

from PIL import Image


def start_menu_dirs():
    return [
        os.path.join(os.environ.get("PROGRAMDATA", r"C:\ProgramData"), r"Microsoft\Windows\Start Menu\Programs"),
        os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs"),
    ]


SKIP = ("uninstall", "kaldır", "readme", "help", "yardım", "documentation", "website", "release notes")


def list_programs():
    """[{name, path}] sorted by name; uninstallers and help links left out."""
    seen, out = set(), []
    for root in start_menu_dirs():
        for folder, _dirs, files in os.walk(root):
            for f in files:
                if not f.lower().endswith((".lnk", ".url")):
                    continue
                name = f[:-4]
                low = name.lower()
                if any(s in low for s in SKIP) or low in seen:
                    continue
                seen.add(low)
                out.append({"name": name, "path": os.path.join(folder, f)})
    out.sort(key=lambda p: p["name"].lower())
    return out


def _png_data_url(img, size=320):
    """A key picture as a PNG data URL: its own shape (the card fills or fits the key with
    it), at most size px."""
    img = img.convert("RGBA")
    if max(img.size) > size:
        k = size / max(img.size)
        img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")


def icon_data_url(path):
    """The program's own icon (largest in the .exe) as a PNG data URL; '' if none."""
    from .actions import lnk_target

    target = lnk_target(path)
    if not target.lower().endswith((".exe", ".dll")) or not os.path.exists(target):
        return ""
    try:
        from icoextract import IconExtractor

        data = IconExtractor(target).get_icon(num=0)
        ico = Image.open(data)
        # .ico holds several sizes: take the largest.
        sizes = sorted(getattr(ico, "info", {}).get("sizes", []) or [ico.size], key=lambda s: s[0] * s[1])
        if sizes:
            ico.size = sizes[-1]
        ico.load()
        return _png_data_url(ico)
    except Exception:  # noqa: BLE001 - some programs carry no icon resource
        return ""
