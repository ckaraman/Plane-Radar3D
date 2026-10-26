"""Plane Radar Deck: the Windows helper for the Plane Radar 3D card's control screen.

Sits in the notification area, starts with Windows, keeps the card's keys working
(open sites in a chosen browser, start programs, ...), sends the real volume and what is
playing (title, cover) to the card. The window only opens to edit the keys.
"""

import os
import socket
import sys
import threading
import time

APP_NAME = "Plane Radar Deck"
VERSION = "1.0.0"
SINGLE_PORT = 47821  # localhost: a second start asks the first one to show its window


def resource(*parts):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, *parts)


LOG_LINES = []


def log(msg):
    line = time.strftime("%H:%M:%S ") + str(msg)
    LOG_LINES.append(line)
    del LOG_LINES[:-300]
    try:
        print(line)
    except Exception:  # noqa: BLE001 - no console in the windowed build
        pass


# ------------------------------------------------------------------ autostart
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def set_autostart(on):
    if sys.platform != "win32":
        return
    import winreg

    exe = sys.executable if getattr(sys, "frozen", False) else '"%s" "%s"' % (sys.executable, os.path.abspath(__file__))
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if on:
            cmd = ('"%s" --tray' % exe) if getattr(sys, "frozen", False) else exe + " --tray"
            winreg.SetValueEx(k, "PlaneRadarDeck", 0, winreg.REG_SZ, cmd)
        else:
            try:
                winreg.DeleteValue(k, "PlaneRadarDeck")
            except FileNotFoundError:
                pass


# ------------------------------------------------------------------ window API (JavaScript)
class Api:
    """Called from web/index.html through pywebview (window.pywebview.api.*)."""

    def __init__(self, core):
        self.core = core
        self.window = None

    def state(self):
        from deck import actions

        slots, settings = self.core.config.snapshot()
        v = self.core.volume
        return {
            "version": VERSION, "slots": slots, "settings": settings,
            "device": self.core.device.status(), "volume": {"v": v.level, "m": v.muted},
            "now": self.core.now, "browsers": [{"id": b["id"], "name": b["name"]} for b in actions.browsers()],
            "upload": dict(self.core.upload),
        }

    def live(self):
        v = self.core.volume
        return {"device": self.core.device.status(), "volume": {"v": v.level, "m": v.muted},
                "now": self.core.now, "upload": dict(self.core.upload)}

    def save_slots(self, slots):
        self.core.config.set_slots(slots)
        return True

    def save_settings(self, settings):
        self.core.config.set_settings(settings)
        try:
            set_autostart(bool(self.core.config.settings.get("autostart", True)))
        except OSError as e:
            log("autostart: %s" % e)
        self.core._send_now()
        return True

    def send_to_card(self, slots):
        self.core.config.set_slots(slots)
        return self.core.send_keys_to_card()

    def test(self, action):
        from deck import actions

        threading.Thread(target=actions.run, args=(action,), daemon=True).start()
        return True

    def programs(self):
        from deck import programs

        return programs.list_programs()

    def program_icon(self, path):
        from deck import programs

        return programs.icon_data_url(path)

    def _dialog(self, kind, types=()):
        w = self.window
        if w is None:
            return None
        res = w.create_file_dialog(kind, file_types=tuple(types))
        if not res:
            return None
        return res[0] if isinstance(res, (list, tuple)) else res

    def pick_image(self):
        import webview
        from deck import programs
        from PIL import Image

        path = self._dialog(webview.OPEN_DIALOG, ("Resimler (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.webp)", "Tüm dosyalar (*.*)"))
        if not path:
            return ""
        try:
            return programs._png_data_url(Image.open(path))
        except Exception as e:  # noqa: BLE001
            log("picture: %s" % e)
            return ""

    def pick_program(self):
        import webview

        return self._dialog(webview.OPEN_DIALOG, ("Programlar (*.exe;*.lnk)", "Tüm dosyalar (*.*)")) or ""

    def pick_folder(self):
        import webview

        return self._dialog(webview.FOLDER_DIALOG) or ""

    def release(self, released):
        self.core.device.release(bool(released))
        return self.core.device.status()

    def logs(self):
        return "\n".join(LOG_LINES[-200:])


# ------------------------------------------------------------------ tray
def tray_image():
    from PIL import Image, ImageDraw

    size = 64
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Peach -> lavender rounded tile with four small keys, as in the design.
    top, bottom = (255, 180, 140), (154, 162, 255)
    for y in range(size):
        t = y / (size - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        d.line([(0, y), (size, y)], fill=c)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=16, fill=255)
    img.putalpha(mask)
    ink = (21, 25, 56, 255)
    for x0, y0 in ((14, 14), (36, 14), (14, 36)):
        d.rounded_rectangle([x0, y0, x0 + 14, y0 + 14], radius=4, outline=ink, width=4)
    d.ellipse([36, 36, 50, 50], outline=ink, width=4)
    return img


def _deep_selftest():
    """CI: touch the Windows APIs once and write what happened to selftest.txt."""
    import asyncio
    import traceback

    out = []

    def step(name, fn):
        try:
            out.append("%s: %s" % (name, fn()))
        except Exception:  # noqa: BLE001
            out.append("%s FAILED: %s" % (name, traceback.format_exc(limit=3).replace("\n", " | ")))

    from deck import actions, media, programs, volume

    step("browsers", lambda: [b["name"] for b in actions.browsers()])
    step("programs", lambda: len(programs.list_programs()))
    step("icon", lambda: len(programs.icon_data_url(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "notepad.exe"))))

    def media_once():
        async def go():
            mgr = await media.MediaManager.request_async()
            s = mgr.get_current_session()
            return "session=%s" % (s is not None)
        return asyncio.new_event_loop().run_until_complete(go())

    step("media", media_once)

    def vol():
        import comtypes
        comtypes.CoInitialize()
        ep = volume.VolumeWatcher.__new__(volume.VolumeWatcher)._endpoint()
        return "%.2f muted=%s" % (ep.GetMasterVolumeLevelScalar(), ep.GetMute())

    step("volume", vol)
    step("cover", lambda: len(media.cover_jpeg(_test_png(), 800, 480)))
    with open("selftest.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def _test_png():
    import io

    buf = io.BytesIO()
    tray_image().convert("RGB").resize((300, 300)).save(buf, "PNG")
    return buf.getvalue()


def run():
    if "--selftest" in sys.argv:  # CI: the packaged program starts and its modules load
        from deck import actions, blob, config, device, media, programs, volume  # noqa: F401
        import webview  # noqa: F401
        import pystray  # noqa: F401

        print("ok", VERSION)
        if "--deep" in sys.argv:
            _deep_selftest()
        return

    # One copy only: a second start shows the first one's window.
    try:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.bind(("127.0.0.1", SINGLE_PORT))
        srv.listen(2)
    except OSError:
        try:
            with socket.create_connection(("127.0.0.1", SINGLE_PORT), timeout=2) as s:
                s.sendall(b"show")
        except OSError:
            pass
        return

    import pystray
    import webview
    from deck.core import Core

    core = Core(log=log)
    try:
        set_autostart(bool(core.config.settings.get("autostart", True)))
    except OSError as e:
        log("autostart: %s" % e)
    api = Api(core)
    start_hidden = "--tray" in sys.argv
    window = webview.create_window(APP_NAME, resource("web", "index.html"), js_api=api, width=1240, height=820,
                                   min_size=(900, 640), hidden=start_hidden, background_color="#111634")
    api.window = window
    quitting = {"now": False}

    def on_closing():
        if quitting["now"]:
            return True
        window.hide()  # closing the window keeps the helper running in the tray
        return False

    window.events.closing += on_closing

    def show(*_):
        window.show()
        window.restore()

    def toggle_release(icon, item):
        core.device.release(not core.device.released)

    def quit_app(icon, _item):
        quitting["now"] = True
        core.device.stop()
        icon.stop()
        window.destroy()

    def status_text(_item):
        st = core.device.status()
        if st["released"]:
            return "Bağlantı bırakıldı (kurulum sayfası için)"
        if st["connected"]:
            return "Kart bağlı · Ses %%%d" % core.volume.level
        return "Kart bekleniyor…"

    menu = pystray.Menu(
        pystray.MenuItem(status_text, None, enabled=False),
        pystray.MenuItem("Tuşları düzenle…", show, default=True),
        pystray.MenuItem("Bağlantıyı bırak (kurulum sayfası için)", toggle_release,
                         checked=lambda _i: core.device.released),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Çıkış", quit_app),
    )
    icon = pystray.Icon("PlaneRadarDeck", tray_image(), APP_NAME, menu)
    icon.run_detached()

    def listen():
        while True:
            try:
                conn, _ = srv.accept()
                conn.close()
                show()
            except OSError:
                return

    threading.Thread(target=listen, daemon=True).start()
    webview.start(gui="edgechromium", debug="--debug" in sys.argv)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    run()
