"""Settings and the 12 deck keys, kept in %APPDATA%\\PlaneRadarDeck\\config.json (Windows) or
~/Library/Application Support/PlaneRadarDeck/config.json (Mac)."""

import copy
import json
import os
import sys
import threading

SLOTS = 12

IS_MAC = sys.platform == "darwin"
if IS_MAC:
    APP_DIR = os.path.join(os.path.expanduser("~/Library/Application Support"), "PlaneRadarDeck")
else:
    APP_DIR = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "PlaneRadarDeck")
CONFIG_PATH = os.path.join(APP_DIR, "config.json")

# Action types (the editor's "Bu tuş ne yapsın?"):
#   none                          nothing
#   site    {url, browser}        browser: "" (default) or an id from actions.browsers()
#   app     {path, name, front}   a .exe / .lnk / shell:AppsFolder id; front = raise if running
#   media   {code}                0 vol+, 1 vol-, 2 mute, 3 play/pause, 4 next, 5 previous
#   keys    {combo}               "Ctrl+Shift+S" (the card sends it itself when it can)
#   text    {text}                typed as Unicode, any keyboard layout
#   folder  {path}                a folder or a file, opened as in Explorer


def _default_slots():
    if IS_MAC:
        s = [
            {"label": "YouTube", "img": "", "action": {"type": "site", "url": "https://www.youtube.com", "browser": ""}},
            {"label": "Hesap Makinesi", "img": "", "action": {"type": "app", "path": "/System/Applications/Calculator.app", "name": "Hesap Makinesi", "front": True}},
            {"label": "İndirilenler", "img": "", "action": {"type": "folder", "path": os.path.join(os.path.expanduser("~"), "Downloads")}},
            {"label": "Ekran görüntüsü", "img": "", "action": {"type": "keys", "combo": "Cmd+Shift+4"}},
            {"label": "Notlar", "img": "", "action": {"type": "app", "path": "/System/Applications/Notes.app", "name": "Notlar", "front": True}},
        ]
        while len(s) < SLOTS:
            s.append({"label": "", "img": "", "action": {"type": "none"}})
        return s
    s = [
        {"label": "YouTube", "img": "", "action": {"type": "site", "url": "https://www.youtube.com", "browser": ""}},
        {"label": "Hesap makinesi", "img": "", "action": {"type": "app", "path": "calc.exe", "name": "Hesap Makinesi", "front": True}},
        {"label": "İndirilenler", "img": "", "action": {"type": "folder", "path": os.path.join(os.path.expanduser("~"), "Downloads")}},
        {"label": "Ekran görüntüsü", "img": "", "action": {"type": "keys", "combo": "Win+Shift+S"}},
        {"label": "Not defteri", "img": "", "action": {"type": "app", "path": "notepad.exe", "name": "Not Defteri", "front": True}},
    ]
    while len(s) < SLOTS:
        s.append({"label": "", "img": "", "action": {"type": "none"}})
    return s


DEFAULT_SETTINGS = {
    "autostart": True,          # start with Windows
    "music_screen": True,       # send what is playing (the card opens its music page)
    "back_seconds": 60,         # after a stop, back to the keys (-1 = never)
    "send_volume": True,        # real volume on the card
    "steps": 50,                # volume keys per full knob turn without the helper (Windows: 2 %)
    "close_quits": False,       # closing the window ends the program (else it stays in the tray)
    "queue": True,              # the YouTube Music queue on the card (browser extension)
    "transport_moved": False,   # done once: previous / play / next keys taken off (they sit under the knob)
}


class Config:
    def __init__(self, path=CONFIG_PATH):
        self.path = path
        self.lock = threading.RLock()
        self.slots = _default_slots()
        self.settings = dict(DEFAULT_SETTINGS)
        self.resend = False  # keys changed here on start: send them to the card once
        self.load()

    def load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            self.settings["transport_moved"] = True  # a new set: nothing to move
            return
        slots = data.get("slots")
        if isinstance(slots, list) and len(slots) == SLOTS:
            self.slots = [self._clean_slot(s) for s in slots]
        st = data.get("settings")
        if isinstance(st, dict):
            for k, v in DEFAULT_SETTINGS.items():
                if isinstance(st.get(k), type(v)):
                    self.settings[k] = st[k]
        self._move_transport()

    def _move_transport(self):
        """Firmware 0.34 shows previous / play / next under the knob on every page: the
        same keys in the grid only take room (the card shows the rest larger)."""
        if self.settings.get("transport_moved"):
            return
        for i, s in enumerate(self.slots):
            a = s.get("action") or {}
            if a.get("type") == "media" and int(a.get("code", -1)) in (3, 4, 5):
                self.slots[i] = {"label": "", "img": "", "action": {"type": "none"}}
                self.resend = True  # the card gets the set without them
        self.settings["transport_moved"] = True
        try:
            self.save()
        except OSError:
            pass

    @staticmethod
    def _clean_slot(s):
        if not isinstance(s, dict):
            s = {}
        a = s.get("action") if isinstance(s.get("action"), dict) else {"type": "none"}
        if a.get("type") not in ("none", "site", "app", "media", "keys", "text", "folder"):
            a = {"type": "none"}
        return {"label": str(s.get("label", ""))[:40], "img": str(s.get("img", "")), "action": a}

    def save(self):
        with self.lock:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"slots": self.slots, "settings": self.settings}, f, ensure_ascii=False, indent=1)
            os.replace(tmp, self.path)

    def snapshot(self):
        with self.lock:
            return copy.deepcopy(self.slots), dict(self.settings)

    def set_slots(self, slots):
        if not isinstance(slots, list) or len(slots) != SLOTS:
            raise ValueError("12 keys expected")
        with self.lock:
            self.slots = [self._clean_slot(s) for s in slots]
            self.save()

    def set_settings(self, st):
        with self.lock:
            for k, v in DEFAULT_SETTINGS.items():
                if isinstance(st.get(k), type(v)):
                    self.settings[k] = st[k]
            self.save()
