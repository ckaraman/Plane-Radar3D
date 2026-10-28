"""Apps for the "Program seç" list on a Mac and their icons. Same interface as programs.py."""

import io
import os

from PIL import Image

from .programs import _png_data_url  # noqa: F401 - app.py takes it from here too

APP_DIRS = ("/Applications", "/Applications/Utilities", os.path.expanduser("~/Applications"),
            "/System/Applications", "/System/Applications/Utilities",
            "/System/Volumes/Preboot/Cryptexes/App/System/Applications")


def _display_name(path):
    try:
        from Foundation import NSFileManager

        name = str(NSFileManager.defaultManager().displayNameAtPath_(path))  # "Hesap Makinesi"
    except Exception:  # noqa: BLE001
        name = os.path.basename(path)
    return name[:-4] if name.endswith(".app") else name


def list_programs():
    seen, out = set(), []
    for root in APP_DIRS:
        try:
            names = os.listdir(root)
        except OSError:
            continue
        for f in names:
            if not f.endswith(".app"):
                continue
            path = os.path.join(root, f)
            real = os.path.realpath(path)
            if real in seen:
                continue
            seen.add(real)
            out.append({"name": _display_name(path), "path": path})
    out.sort(key=lambda p: p["name"].lower())
    return out


def icon_data_url(path, size=256):
    """The app's own icon (as Finder shows it) as a PNG data URL; '' if none."""
    try:
        from AppKit import (NSBitmapImageRep, NSCompositingOperationCopy, NSDeviceRGBColorSpace,
                            NSGraphicsContext, NSPNGFileType, NSWorkspace, NSZeroRect)

        img = NSWorkspace.sharedWorkspace().iconForFile_(path)
        if img is None:
            return ""
        rep = NSBitmapImageRep.alloc().initWithBitmapDataPlanes_pixelsWide_pixelsHigh_bitsPerSample_samplesPerPixel_hasAlpha_isPlanar_colorSpaceName_bytesPerRow_bitsPerPixel_(
            None, size, size, 8, 4, True, False, NSDeviceRGBColorSpace, 0, 0)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.setCurrentContext_(NSGraphicsContext.graphicsContextWithBitmapImageRep_(rep))
        img.drawInRect_fromRect_operation_fraction_(((0, 0), (size, size)), NSZeroRect, NSCompositingOperationCopy, 1.0)
        NSGraphicsContext.restoreGraphicsState()
        data = rep.representationUsingType_properties_(NSPNGFileType, {})
        pic = Image.open(io.BytesIO(bytes(data)))
        pic.load()
        # Mac icons leave a margin around the artwork: trim it so the key shows it large.
        box = pic.getchannel("A").getbbox() if pic.mode == "RGBA" else None
        if box:
            pic = pic.crop(box)
        return _png_data_url(pic)
    except Exception:  # noqa: BLE001
        return ""
