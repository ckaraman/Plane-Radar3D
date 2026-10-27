"""What a key does on a Mac: open a site in a chosen browser, open (or bring up) an app,
open a folder, press keys, type text. Same interface as actions.py."""

import ctypes
import ctypes.util
import os
import subprocess
import time

KNOWN_BROWSERS = (
    ("chrome", "Google Chrome", "Google Chrome.app"),
    ("safari", "Safari", "Safari.app"),
    ("firefox", "Mozilla Firefox", "Firefox.app"),
    ("edge", "Microsoft Edge", "Microsoft Edge.app"),
    ("brave", "Brave", "Brave Browser.app"),
    ("arc", "Arc", "Arc.app"),
    ("opera", "Opera", "Opera.app"),
    ("vivaldi", "Vivaldi", "Vivaldi.app"),
)
APP_DIRS = ("/Applications", os.path.expanduser("~/Applications"), "/System/Applications",
            "/System/Volumes/Preboot/Cryptexes/App/System/Applications")


def browsers():
    out = []
    for bid, name, app in KNOWN_BROWSERS:
        for d in APP_DIRS:
            p = os.path.join(d, app)
            if os.path.isdir(p):
                out.append({"id": bid, "name": name, "path": p})
                break
    return out


def default_browser_exe():
    """The app Mac opens https links with ("Google Chrome.app", "Safari.app"), or None."""
    try:
        from AppKit import NSWorkspace
        from Foundation import NSURL

        url = NSWorkspace.sharedWorkspace().URLForApplicationToOpenURL_(NSURL.URLWithString_("https://example.com"))
        return os.path.basename(str(url.path())) if url is not None else None
    except Exception:  # noqa: BLE001
        return None


def app_binary(app_path):
    """The program inside an .app (to hand it an address such as chrome://extensions)."""
    import plistlib

    try:
        with open(os.path.join(app_path, "Contents", "Info.plist"), "rb") as f:
            exe = plistlib.load(f).get("CFBundleExecutable")
        p = os.path.join(app_path, "Contents", "MacOS", exe)
        return p if os.path.exists(p) else None
    except (OSError, ValueError, TypeError):
        return None


def open_site(url, browser=""):
    url = (url or "").strip()
    if not url:
        return
    if "://" not in url:
        url = "https://" + url
    for b in browsers():
        if browser and b["id"] == browser:
            subprocess.Popen(["/usr/bin/open", "-a", b["path"], url])  # opens and comes to the front
            return
    subprocess.Popen(["/usr/bin/open", url])


def lnk_target(path):
    return path


def open_app(path, front=True):
    """An .app (path or name): started, or brought to the front when it runs."""
    path = (path or "").strip()
    if not path:
        return
    if path.endswith(".app") or os.path.isdir(path):
        subprocess.Popen(["/usr/bin/open", path])
    elif os.path.exists(path):
        subprocess.Popen(["/usr/bin/open", "-a", path])
    else:
        subprocess.Popen(["/usr/bin/open", "-a", path])  # "Calculator", "Notes" ...


def open_folder(path):
    path = os.path.expanduser(os.path.expandvars((path or "").strip()))
    if path:
        subprocess.Popen(["/usr/bin/open", path])


# ------------------------------------------------------------------ keys (Quartz events)
# Virtual key codes of the US layout (positions on the keyboard).
_ANSI = {"a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9, "b": 11, "q": 12,
         "w": 13, "e": 14, "r": 15, "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22, "5": 23,
         "=": 24, "9": 25, "7": 26, "-": 27, "8": 28, "0": 29, "]": 30, "o": 31, "u": 32, "[": 33, "i": 34,
         "p": 35, "l": 37, "j": 38, "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44, "n": 45, "m": 46,
         ".": 47, "`": 50}
_NAMED = {"enter": 36, "return": 36, "tab": 48, "space": 49, "backspace": 51, "delete": 117, "del": 117,
          "esc": 53, "escape": 53, "home": 115, "end": 119, "pageup": 116, "pagedown": 121, "left": 123,
          "right": 124, "down": 125, "up": 126, "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96,
          "f6": 97, "f7": 98, "f8": 100, "f9": 101, "f10": 109, "f11": 103, "f12": 111, "f13": 105,
          "f14": 107, "f15": 113, "f16": 106, "f17": 64, "f18": 79, "f19": 80, "f20": 90}
_FLAGS = {"cmd": 1 << 20, "command": 1 << 20, "win": 1 << 20, "meta": 1 << 20, "shift": 1 << 17,
          "alt": 1 << 19, "option": 1 << 19, "opt": 1 << 19, "ctrl": 1 << 18, "control": 1 << 18}


def _layout_keycodes():
    """{character: key code} of the keyboard layout in use (Turkish Q puts ğ, ü, ş ... on
    other keys than the US one), via UCKeyTranslate; {} when that fails."""
    try:
        carbon = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/Carbon.framework/Carbon")
        cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        carbon.TISCopyCurrentKeyboardLayoutInputSource.restype = ctypes.c_void_p
        carbon.TISGetInputSourceProperty.restype = ctypes.c_void_p
        carbon.TISGetInputSourceProperty.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        cf.CFDataGetBytePtr.restype = ctypes.c_void_p
        cf.CFDataGetBytePtr.argtypes = [ctypes.c_void_p]
        prop = ctypes.c_void_p.in_dll(carbon, "kTISPropertyUnicodeKeyLayoutData")
        src = carbon.TISCopyCurrentKeyboardLayoutInputSource()
        data = carbon.TISGetInputSourceProperty(src, prop)
        if not data:
            return {}
        layout = cf.CFDataGetBytePtr(data)
        carbon.LMGetKbdType.restype = ctypes.c_uint8
        kbd = carbon.LMGetKbdType()
        carbon.UCKeyTranslate.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                                          ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
        out = {}
        for code in range(128):
            dead, n = ctypes.c_uint32(0), ctypes.c_ulong(0)
            buf = (ctypes.c_uint16 * 4)()
            if carbon.UCKeyTranslate(layout, code, 3, 0, kbd, 1, ctypes.byref(dead), 4, ctypes.byref(n), buf) == 0 and n.value:
                ch = "".join(chr(buf[i]) for i in range(n.value)).lower()
                out.setdefault(ch, code)
        return out
    except Exception:  # noqa: BLE001
        return {}


def accessibility_ok(prompt=True):
    """Pressing keys for another app needs the Accessibility permission (asked once)."""
    try:
        from ApplicationServices import AXIsProcessTrustedWithOptions, kAXTrustedCheckOptionPrompt

        return bool(AXIsProcessTrustedWithOptions({kAXTrustedCheckOptionPrompt: bool(prompt)}))
    except Exception:  # noqa: BLE001
        return True


def _post(code, flags, down):
    import Quartz

    ev = Quartz.CGEventCreateKeyboardEvent(None, code, down)
    if flags:
        Quartz.CGEventSetFlags(ev, flags)
    Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)


def press_combo(combo):
    """'Cmd+Shift+4', 'Ctrl+Alt+T' ... (Win means Cmd on a Mac)."""
    if not accessibility_ok():
        return
    flags, key = 0, None
    layout = None
    for part in [p.strip().lower() for p in str(combo).split("+") if p.strip()]:
        if part in _FLAGS:
            flags |= _FLAGS[part]
        elif part in _NAMED:
            key = _NAMED[part]
        elif len(part) == 1:
            if layout is None:
                layout = _layout_keycodes()
            key = layout.get(part, _ANSI.get(part))
            if key is None:
                return
        else:
            return
    if key is None:
        return
    _post(key, flags, True)
    time.sleep(0.01)
    _post(key, flags, False)


def press_media(code):
    """0 vol+, 1 vol-, 2 mute, 3 play/pause, 4 next, 5 previous (the card does these
    itself as a keyboard; here for the editor's test button)."""
    from . import media_mac, volume_mac  # noqa: F401

    code = int(code)
    if code in (3, 4, 5):
        media_mac.send_command({3: 2, 4: 4, 5: 5}[code])
    elif code in (0, 1, 2):
        script = {0: "set volume output volume ((output volume of (get volume settings)) + 6)",
                  1: "set volume output volume ((output volume of (get volume settings)) - 6)",
                  2: "set volume output muted (not (output muted of (get volume settings)))"}[code]
        subprocess.Popen(["/usr/bin/osascript", "-e", script])


def type_text(text):
    """Any letters (Turkish too), whatever the keyboard layout."""
    if not accessibility_ok():
        return
    import Quartz

    text = str(text).replace("\r\n", "\n")
    for line_i, line in enumerate(text.split("\n")):
        if line_i:
            _post(36, 0, True)
            _post(36, 0, False)
        for i in range(0, len(line), 16):
            part = line[i:i + 16]
            for down in (True, False):
                ev = Quartz.CGEventCreateKeyboardEvent(None, 0, down)
                Quartz.CGEventKeyboardSetUnicodeString(ev, len(part.encode("utf-16-le")) // 2, part)
                Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
            time.sleep(0.01)


def run(action):
    t = action.get("type")
    if t == "site":
        open_site(action.get("url"), action.get("browser", ""))
    elif t == "app":
        open_app(action.get("path"), bool(action.get("front", True)))
    elif t == "folder":
        open_folder(action.get("path"))
    elif t == "media":
        press_media(action.get("code", 3))
    elif t == "keys":
        press_combo(action.get("combo", ""))
    elif t == "text":
        type_text(action.get("text", ""))
