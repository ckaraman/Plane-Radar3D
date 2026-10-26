"""What a key does on the computer: open a site in a chosen browser, start (or bring up)
a program, open a folder, press keys, type text."""

import ctypes
import os
import subprocess
import sys
import time

IS_WIN = sys.platform == "win32"
if IS_WIN:
    import winreg
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetWindow.restype = wintypes.HWND
    user32.GetWindow.argtypes = (wintypes.HWND, wintypes.UINT)
    user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
    user32.BringWindowToTop.argtypes = (wintypes.HWND,)
    user32.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
    user32.IsIconic.argtypes = (wintypes.HWND,)
    user32.GetWindowLongW.argtypes = (wintypes.HWND, ctypes.c_int)
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))

# ------------------------------------------------------------------ browsers
KNOWN_BROWSERS = (
    ("chrome", "Google Chrome", "chrome.exe"),
    ("edge", "Microsoft Edge", "msedge.exe"),
    ("firefox", "Mozilla Firefox", "firefox.exe"),
    ("brave", "Brave", "brave.exe"),
    ("opera", "Opera", "opera.exe"),
    ("vivaldi", "Vivaldi", "vivaldi.exe"),
)


def _app_path(exe):
    if not IS_WIN:
        return None
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            with winreg.OpenKey(root, r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\\" + exe) as k:
                path = winreg.QueryValue(k, None)
                if path and os.path.exists(path.strip('"')):
                    return path.strip('"')
        except OSError:
            pass
    return None


def browsers():
    """[{id, name, path}] of the browsers installed on this computer."""
    out = []
    for bid, name, exe in KNOWN_BROWSERS:
        path = _app_path(exe)
        if not path and bid == "opera":
            p = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Opera", "launcher.exe")
            path = p if os.path.exists(p) else None
        if path:
            out.append({"id": bid, "name": name, "path": path})
    return out


def default_browser_exe():
    """The program Windows opens https links with (its .exe file name), or None."""
    if not IS_WIN:
        return None
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice") as k:
            prog = winreg.QueryValueEx(k, "ProgId")[0]
        with winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, prog + r"\shell\open\command") as k:
            cmd = winreg.QueryValue(k, None)
    except OSError:
        return None
    cmd = cmd.strip()
    exe = cmd[1:].split('"', 1)[0] if cmd.startswith('"') else cmd.split(" ", 1)[0]
    return os.path.basename(exe) or None


def open_site(url, browser=""):
    url = (url or "").strip()
    if not url:
        return
    if "://" not in url:
        url = "https://" + url
    allow_foreground()
    exe = None
    if browser:
        for b in browsers():
            if b["id"] == browser:
                subprocess.Popen([b["path"], url])
                exe = os.path.basename(b["path"])
                break
    if exe is None:
        os.startfile(url)  # default browser
        exe = default_browser_exe()
    # A browser that is already open gets the new tab in its window, often behind the
    # others (Windows keeps the foreground for the program the user last used): bring it up.
    if exe:
        for _ in range(12):
            time.sleep(0.25)
            if _front_window_of(exe):
                break


# ------------------------------------------------------------------ programs
def allow_foreground():
    """Let the program we start (or a running one we tell to open something) come to the
    front. Windows only allows that to the program that had the last input, so a
    harmless key event of our own first."""
    if not IS_WIN:
        return
    user32.keybd_event(0x87, 0, 0, 0)  # F24: no program reacts to it
    user32.keybd_event(0x87, 0, 2, 0)
    user32.AllowSetForegroundWindow(-1)  # ASFW_ANY


def force_foreground(hwnd):
    """Raise a window even when Windows' foreground lock is on."""
    SW_RESTORE, SW_SHOW = 9, 5
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)
    else:
        user32.ShowWindow(hwnd, SW_SHOW)
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return True
    kernel32 = ctypes.WinDLL("kernel32")
    me = kernel32.GetCurrentThreadId()
    other = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    attached = bool(other and other != me and user32.AttachThreadInput(me, other, True))
    try:
        allow_foreground()
        user32.BringWindowToTop(hwnd)
        ok = bool(user32.SetForegroundWindow(hwnd))
    finally:
        if attached:
            user32.AttachThreadInput(me, other, False)
    return ok or user32.GetForegroundWindow() == hwnd


def _front_window_of(exe_name):
    """Bring a running program's main window to the front; False if none."""
    if not IS_WIN or not exe_name:
        return False
    import psutil

    want = exe_name.lower()
    pids = {p.pid for p in psutil.process_iter(["name"]) if (p.info.get("name") or "").lower() == want}
    if not pids:
        return False
    found = []
    GW_OWNER, GWL_EXSTYLE, WS_EX_TOOLWINDOW = 4, -20, 0x80

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        # Top-level, visible, titled, no tool window: the program's real windows, the
        # topmost first (EnumWindows goes front to back).
        if user32.IsWindowVisible(hwnd) and user32.GetWindowTextLengthW(hwnd) > 0 \
                and not user32.GetWindow(hwnd, GW_OWNER) \
                and not (user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value in pids:
                found.append(hwnd)
                return False
        return True

    user32.EnumWindows(each, 0)
    if not found:
        return False
    force_foreground(found[0])
    return True


def lnk_target(path):
    if not IS_WIN or not path.lower().endswith(".lnk"):
        return path
    try:
        import win32com.client

        return win32com.client.Dispatch("WScript.Shell").CreateShortCut(path).Targetpath or path
    except Exception:  # noqa: BLE001
        return path


def open_app(path, front=True):
    path = (path or "").strip().strip('"')
    if not path:
        return
    if front:
        target = lnk_target(path)
        if _front_window_of(os.path.basename(target)):
            return
    allow_foreground()
    if path.lower().startswith("shell:"):
        subprocess.Popen(["explorer.exe", path])
    elif os.path.exists(path) or path.lower().endswith(".lnk"):
        os.startfile(path)
    else:
        subprocess.Popen(path, shell=True)  # calc.exe, notepad.exe: found on PATH


def open_folder(path):
    path = os.path.expandvars((path or "").strip().strip('"'))
    if path:
        allow_foreground()
        os.startfile(path)


# ------------------------------------------------------------------ keys (SendInput)
if IS_WIN:
    ULONG_PTR = ctypes.c_size_t

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("u", _U)]

KEYEVENTF_KEYUP, KEYEVENTF_UNICODE, KEYEVENTF_EXTENDED = 0x2, 0x4, 0x1

VK = {"ctrl": 0x11, "control": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B, "windows": 0x5B,
      "enter": 0x0D, "esc": 0x1B, "escape": 0x1B, "tab": 0x09, "space": 0x20, "backspace": 0x08,
      "delete": 0x2E, "del": 0x2E, "insert": 0x2D, "home": 0x24, "end": 0x23, "pageup": 0x21,
      "pagedown": 0x22, "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
      "printscreen": 0x2C, "prtsc": 0x2C}
for _i in range(1, 25):
    VK["f%d" % _i] = 0x6F + _i
EXTENDED = {0x2E, 0x2D, 0x24, 0x23, 0x21, 0x22, 0x25, 0x26, 0x27, 0x28, 0x5B}
MEDIA_VK = {0: 0xAF, 1: 0xAE, 2: 0xAD, 3: 0xB3, 4: 0xB0, 5: 0xB1}  # vol+, vol-, mute, play, next, prev


def _send(inputs):
    arr = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))


def _key(vk, up=False):
    flags = (KEYEVENTF_KEYUP if up else 0) | (KEYEVENTF_EXTENDED if vk in EXTENDED else 0)
    return INPUT(type=1, u=_U(ki=KEYBDINPUT(wVk=vk, wScan=0, dwFlags=flags, time=0, dwExtraInfo=0)))


def press_combo(combo):
    """'Ctrl+Shift+S', 'Win+D', 'Alt+F4' ..."""
    vks = []
    for part in [p.strip().lower() for p in str(combo).split("+") if p.strip()]:
        if part in VK:
            vks.append(VK[part])
        elif len(part) == 1:
            r = user32.VkKeyScanW(ord(part))  # the key for this character on the current layout
            if r == -1:
                return
            vks.append(r & 0xFF)
        else:
            return
    if vks:
        _send([_key(v) for v in vks] + [_key(v, True) for v in reversed(vks)])


def press_media(code):
    vk = MEDIA_VK.get(int(code))
    if vk:
        _send([_key(vk), _key(vk, True)])


def type_text(text):
    """Unicode input: correct on any keyboard layout (Turkish letters included)."""
    inputs = []
    for ch in str(text).replace("\r\n", "\n"):
        if ch == "\n":
            inputs += [_key(0x0D), _key(0x0D, True)]
            continue
        code = ord(ch)
        units = [code] if code < 0x10000 else [0xD800 + ((code - 0x10000) >> 10), 0xDC00 + ((code - 0x10000) & 0x3FF)]
        for u in units:
            inputs.append(INPUT(type=1, u=_U(ki=KEYBDINPUT(wVk=0, wScan=u, dwFlags=KEYEVENTF_UNICODE, time=0, dwExtraInfo=0))))
            inputs.append(INPUT(type=1, u=_U(ki=KEYBDINPUT(wVk=0, wScan=u, dwFlags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, time=0, dwExtraInfo=0))))
    for i in range(0, len(inputs), 40):
        _send(inputs[i:i + 40])
        time.sleep(0.005)


def run(action):
    """Carry out one key's action (config.py lists the types)."""
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
