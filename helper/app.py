"""Plane Radar Deck: the Windows / Mac helper for the Plane Radar 3D card's control screen.

Sits in the notification area (Windows) or the menu bar (Mac), starts with the computer,
keeps the card's keys working
(open sites in a chosen browser, start programs, ...), sends the real volume and what is
playing (title, cover) to the card. The window only opens to edit the keys.
"""

import os
import socket
import sys
import threading
import time

APP_NAME = "Plane Radar Deck"
VERSION = "1.6.0"
SINGLE_PORT = 47821  # localhost: a second start asks the first one to show its window
IS_MAC = sys.platform == "darwin"


def resource(*parts):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, *parts)


LOG_LINES = []


def log(msg):
    line = time.strftime("%H:%M:%S ") + str(msg)
    LOG_LINES.append(line)
    del LOG_LINES[:-300]
    try:
        print(line, flush=True)
    except Exception:  # noqa: BLE001 - no console in the windowed build
        pass


# ------------------------------------------------------------------ autostart
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


LAUNCH_AGENT = os.path.expanduser("~/Library/LaunchAgents/com.planeradar.deck.plist")


def _mac_autostart(on):
    """A launch agent: started at login, in the menu bar only."""
    import plistlib

    if not on:
        try:
            os.remove(LAUNCH_AGENT)
        except FileNotFoundError:
            pass
        return
    if not getattr(sys, "frozen", False):
        return  # only the built app starts itself
    os.makedirs(os.path.dirname(LAUNCH_AGENT), exist_ok=True)
    with open(LAUNCH_AGENT, "wb") as f:
        plistlib.dump({"Label": "com.planeradar.deck", "ProgramArguments": [sys.executable, "--tray"],
                       "RunAtLoad": True, "ProcessType": "Interactive"}, f)


def set_autostart(on):
    if IS_MAC:
        _mac_autostart(on)
        return
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
        self._core = core
        self._window = None

    def state(self):
        from deck.plat import actions

        slots, settings = self._core.config.snapshot()
        v = self._core.volume
        return {
            "version": VERSION, "slots": slots, "settings": settings,
            "device": self._core.device.status(), "feed": self._core.feed.state, "volume": {"v": v.level, "m": v.muted},
            "now": self._core.now, "browsers": [{"id": b["id"], "name": b["name"]} for b in actions.browsers()],
            "upload": dict(self._core.upload), "platform": "mac" if IS_MAC else "win", "link": self._link(),
            "music_hosts": list(__import__("deck.browser_link", fromlist=["MUSIC_HOSTS"]).MUSIC_HOSTS),
        }

    def live(self):
        v = self._core.volume
        return {"device": self._core.device.status(), "feed": self._core.feed.state, "volume": {"v": v.level, "m": v.muted},
                "now": self._core.now, "upload": dict(self._core.upload), "link": self._link()}

    def _link(self):
        st = self._core.link.status()
        st["updated"] = bool(self._core.ext_updated and st["connected"] and st["bundled"] not in st["versions"])
        return st

    # ------------------------------------------------------------ browser extension
    def ext_browsers(self):
        """Browsers that take the extension (Chrome and its relatives), with their path."""
        from deck.plat import actions

        return [{"id": b["id"], "name": b["name"]} for b in actions.browsers()
                if b["id"] in ("chrome", "edge", "brave", "opera", "vivaldi", "arc")]

    def ext_open(self, browser_id):
        """Opens the browser's extensions page and copies the folder to paste."""
        import subprocess

        from deck.plat import actions

        self.copy_text(self._core.ext_folder)
        page = "edge://extensions/" if browser_id == "edge" else "chrome://extensions/"
        for b in actions.browsers():
            if b["id"] != browser_id:
                continue
            exe = b["path"]
            if IS_MAC:
                exe = actions.app_binary(b["path"])
                if not exe:
                    return False
            try:
                subprocess.Popen([exe, page])
                return True
            except OSError as e:
                log("extensions page: %s" % e)
        return False

    def ext_folder_show(self):
        import subprocess

        folder = self._core.ext_folder
        if IS_MAC:
            subprocess.Popen(["/usr/bin/open", folder])
        else:
            os.startfile(folder)
        return True

    def copy_text(self, text):
        try:
            if IS_MAC:
                import subprocess

                subprocess.run(["/usr/bin/pbcopy"], input=str(text).encode("utf-8"), check=True, timeout=5)
            else:
                import win32clipboard

                win32clipboard.OpenClipboard()
                try:
                    win32clipboard.EmptyClipboard()
                    win32clipboard.SetClipboardText(str(text), win32clipboard.CF_UNICODETEXT)
                finally:
                    win32clipboard.CloseClipboard()
            return True
        except Exception as e:  # noqa: BLE001
            log("copy: %s" % e)
            return False

    def save_slots(self, slots):
        self._core.config.set_slots(slots)
        return True

    def save_settings(self, settings):
        self._core.config.set_settings(settings)
        try:
            set_autostart(bool(self._core.config.settings.get("autostart", True)))
        except OSError as e:
            log("autostart: %s" % e)
        self._core._send_now()
        return True

    def send_to_card(self, slots):
        try:
            self._core.config.set_slots(slots)
            return self._core.send_keys_to_card()
        except Exception as e:  # noqa: BLE001 - shown in the window, not lost
            log("send: %s" % e)
            self._core.upload.update(running=False, pct=0, text="", error=str(e) or "Gönderilemedi")
            return False

    def preview(self, slots):
        return self._core.preview_layout(slots)

    def test(self, action):
        from deck.plat import actions

        threading.Thread(target=actions.run, args=(action,), daemon=True).start()
        return True

    def programs(self):
        from deck.plat import programs

        return programs.list_programs()

    def program_icon(self, path):
        from deck.plat import programs

        return programs.icon_data_url(path)

    def _dialog(self, kind, types=()):
        w = self._window
        if w is None:
            return None
        res = w.create_file_dialog(kind, file_types=tuple(types))
        if not res:
            return None
        return res[0] if isinstance(res, (list, tuple)) else res

    def pick_image(self):
        import webview
        from deck.plat import programs
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

        if IS_MAC:
            return self._dialog(webview.OPEN_DIALOG, ("Uygulamalar (*.app)",)) or ""
        return self._dialog(webview.OPEN_DIALOG, ("Programlar (*.exe;*.lnk)", "Tüm dosyalar (*.*)")) or ""

    def pick_folder(self):
        import webview

        return self._dialog(webview.FOLDER_DIALOG) or ""

    def radar_settings(self):
        return self._core.device.radar_settings()

    def search_address(self, q):
        from deck import feed

        try:
            return {"results": feed.search_address(q)}
        except feed.HttpError as e:
            return {"error": str(e)}

    def save_radar(self, settings):
        return self._core.device.save_radar(settings or {})

    def release(self, released):
        self._core.device.release(bool(released))
        return self._core.device.status()

    def logs(self):
        return "\n".join(LOG_LINES[-200:])

    def _log_text(self):
        st = self._core.device.status()
        head = "Plane Radar Deck %s, kart: %s %s, port %s" % (VERSION, st.get("fw"), st.get("usb"), st.get("port"))
        return head + "\n" + "\n".join(LOG_LINES[-300:])

    def copy_logs(self):
        if IS_MAC:
            try:
                import subprocess

                subprocess.run(["/usr/bin/pbcopy"], input=self._log_text().encode("utf-8"), check=True, timeout=5)
                return True
            except Exception as e:  # noqa: BLE001
                log("copy: %s" % e)
                return False
        try:
            import win32clipboard

            win32clipboard.OpenClipboard()
            try:
                win32clipboard.EmptyClipboard()
                win32clipboard.SetClipboardText(self._log_text(), win32clipboard.CF_UNICODETEXT)
            finally:
                win32clipboard.CloseClipboard()
            return True
        except Exception as e:  # noqa: BLE001
            log("copy: %s" % e)
            return False

    def save_logs(self):
        try:
            desk = os.path.join(os.path.expanduser("~"), "Desktop")
            if not os.path.isdir(desk):
                desk = os.path.expanduser("~")
            path = os.path.join(desk, "PlaneRadarDeck-kayit.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write(self._log_text())
            return path
        except OSError as e:
            log("save log: %s" % e)
            return ""

    def exit(self):
        """'Programı kapat' in the window."""
        threading.Thread(target=self._quit, daemon=True).start()
        return True

    def _quit(self):  # replaced in run()
        os._exit(0)


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


def tray_template_png():
    """Mac menu bar icon: black on clear (macOS colours it for light / dark menu bars)."""
    import io

    from PIL import Image, ImageDraw

    size = 44
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ink = (0, 0, 0, 255)
    d.rounded_rectangle([2, 2, size - 3, size - 3], radius=11, outline=ink, width=3)
    for x0, y0 in ((10, 10), (24, 10), (10, 24)):
        d.rounded_rectangle([x0, y0, x0 + 9, y0 + 9], radius=3, fill=ink)
    d.ellipse([24, 24, 33, 33], outline=ink, width=3)
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


_PAGE = {}


def page_url():
    """The window's page. On a Mac WebKit loads no local file through pywebview's request,
    so the page comes from a small server on this computer only (127.0.0.1)."""
    if not IS_MAC:
        return resource("web", "index.html")
    if "url" not in _PAGE:
        import functools
        import http.server

        root = os.path.realpath(resource("web"))

        class Quiet(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *_):
                pass

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=root))
        threading.Thread(target=srv.serve_forever, name="page", daemon=True).start()
        _PAGE["url"] = "http://127.0.0.1:%d/index.html" % srv.server_address[1]
    return _PAGE["url"]


def _mac_unquarantine():
    """A downloaded app is marked "from the internet"; Apple's perl then refuses to load
    the now-playing helper inside it. The user opened the app, so the mark can go."""
    if not (IS_MAC and getattr(sys, "frozen", False)):
        return
    bundle = os.path.abspath(os.path.join(os.path.dirname(sys.executable), "..", ".."))
    if bundle.endswith(".app"):
        import subprocess

        subprocess.run(["/usr/bin/xattr", "-dr", "com.apple.quarantine", bundle], capture_output=True)


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

    from deck import media as media_win
    from deck.plat import actions, media, programs, volume

    from deck import browser_link

    step("extension", lambda: "%s in %s" % (browser_link.bundled_version(), browser_link.source_dir()))

    def ws_lib():
        from websockets.sync.server import serve  # noqa: F401

        return "ok"

    step("websockets", ws_lib)
    step("browsers", lambda: [b["name"] for b in actions.browsers()])
    step("programs", lambda: len(programs.list_programs()))
    if IS_MAC:
        step("icon", lambda: len(programs.icon_data_url("/System/Applications/Calculator.app")))
        step("names", lambda: [p["name"] for p in programs.list_programs()][:6])

        def adapter():
            import subprocess

            script, fw = media.adapter_paths()
            if not script:
                return "adapter MISSING"
            r = subprocess.run(["/usr/bin/perl", script, fw, "get", "--micros"], capture_output=True, text=True, timeout=20)
            return "exit %d out %r err %r" % (r.returncode, r.stdout[:120], r.stderr[-200:])

        step("adapter", adapter)

        def vol():
            ca = volume.CoreAudio()
            dev = ca.device()
            return "%.2f muted=%s" % (ca.volume(dev), ca.muted(dev))

        step("volume", vol)
        step("keys", lambda: len(actions._layout_keycodes()))
        step("cover", lambda: media_win.cover_jpeg(_test_png(), 800, 480)[1:])
        with open("selftest.txt", "w", encoding="utf-8") as f:
            f.write("\n".join(out))
        return

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
    step("cover", lambda: media_win.cover_jpeg(_test_png(), 800, 480)[1:])
    step("browser", lambda: actions.default_browser_exe())
    with open("selftest.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def _test_png():
    import io

    buf = io.BytesIO()
    tray_image().convert("RGB").resize((300, 300)).save(buf, "PNG")
    return buf.getvalue()


def run():
    if "--selftest" in sys.argv:  # CI: the packaged program starts and its modules load
        from deck import blob, config, device, feed  # noqa: F401
        from deck.plat import actions, media, programs, volume  # noqa: F401
        import webview  # noqa: F401
        if IS_MAC:
            from deck import tray_mac  # noqa: F401
        else:
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

    import webview
    from deck.core import Core

    _mac_unquarantine()

    core = Core(log=log)
    try:
        set_autostart(bool(core.config.settings.get("autostart", True)))
    except OSError as e:
        log("autostart: %s" % e)
    api = Api(core)

    # pywebview runs one GUI loop, which ends when its last window closes. A hidden
    # keeper window keeps it alive, so the editor window can be closed and opened again
    # from the tray as often as wanted.
    webview.create_window("PlaneRadarDeck", html="<html><body></body></html>", hidden=True, width=200, height=100)
    lock = threading.Lock()

    def open_editor(*_):
        if core.device.released:  # "Tuşları düzenle" takes the card back
            core.device.release(False)
        with lock:
            w = api._window
            if w is not None:
                try:
                    w.show()
                    w.restore()
                    w.on_top = True   # to the front, then normal again
                    w.on_top = False
                    return
                except Exception:  # noqa: BLE001 - it was closed meanwhile
                    api._window = None
            w = webview.create_window(APP_NAME, page_url(), js_api=api, width=1240, height=820,
                                      min_size=(900, 640), background_color="#111634")

            def closed():
                if api._window is w:
                    api._window = None
                if core.config.settings.get("close_quits", False):
                    quit_app()

            w.events.closed += closed
            api._window = w
            log("editor window created")
        if IS_MAC:
            from deck import tray_mac

            tray_mac.activate()

    def quit_app(*_):
        log("quit")
        try:
            core.device.stop()
        except Exception:  # noqa: BLE001
            pass
        try:
            tray.stop()
        except Exception:  # noqa: BLE001
            pass
        try:
            core.media.stop()  # Mac: the now-playing reader
        except Exception:  # noqa: BLE001
            pass
        os._exit(0)  # the whole program: tray, card link, browser engine

    api._quit = quit_app

    def toggle_release(*_):
        core.device.release(not core.device.released)

    def status_text(*_):
        st = core.device.status()
        if st["released"]:
            return "Bağlantı bırakıldı (kurulum sayfası için)"
        if st["connected"]:
            return "Kart bağlı · Ses %%%d · Veri: %s" % (core.volume.level, core.feed.state)
        return "Kart bekleniyor…"

    if IS_MAC:
        from deck import tray_mac

        tray = tray_mac.MacTray(
            tray_template_png(), status_text, lambda: core.device.released,
            on_edit=lambda: threading.Thread(target=open_editor, daemon=True).start(),
            on_release=toggle_release, on_quit=quit_app)
        log("tray: ready")
    else:
        import pystray

        menu = pystray.Menu(
            pystray.MenuItem(status_text, None, enabled=False),
            pystray.MenuItem("Tuşları düzenle…", open_editor, default=True),
            pystray.MenuItem("Bağlantıyı bırak (kurulum sayfası için)", toggle_release,
                             checked=lambda _i: core.device.released),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Programı kapat", quit_app),
        )
        tray = pystray.Icon("PlaneRadarDeck", tray_image(), APP_NAME, menu)
        tray.run_detached()

    def listen():
        while True:
            try:
                conn, _ = srv.accept()
                conn.close()
                open_editor()
            except OSError:
                return

    threading.Thread(target=listen, daemon=True).start()

    log("gui: starting")

    def first():
        log("gui: started")
        if "--smoke" in sys.argv:
            smoke()
        elif "--tray" not in sys.argv:
            open_editor()

    def smoke():
        """CI: open the editor, close it, open it again, check the page each time, quit."""
        res = []
        log("smoke: start")
        for round_ in (1, 2):
            log("smoke: open %d" % round_)
            open_editor()
            log("smoke: opened %d, window %s" % (round_, api._window))
            ok = False
            w = api._window
            if w is not None:
                log("smoke: shown %s" % w.events.shown.wait(30))
                log("smoke: loaded %s" % w.events.loaded.wait(30))
            for _ in range(20):
                time.sleep(0.5)
                w = api._window
                box = []
                # evaluate_js waits for the page without a limit: asked in a thread, with one.
                t = threading.Thread(target=lambda: box.append(
                    w.evaluate_js("document.querySelectorAll('#grid .key').length")) if w is not None else None,
                    daemon=True)
                t.start()
                t.join(5)
                if box and box[0] == 12:
                    ok = True
                    break
                log("smoke: page answered %r" % (box,))
            res.append("round %d editor %s" % (round_, "ok" if ok else "FAILED"))
            log(res[-1])
            w = api._window
            if w is not None:
                w.destroy()
            for _ in range(20):
                time.sleep(0.25)
                if api._window is None:
                    break
            res.append("round %d closed %s" % (round_, "ok" if api._window is None else "FAILED"))
        with open("smoke.txt", "w", encoding="utf-8") as f:
            f.write("\n".join(res))
        quit_app()

    webview.start(first, gui="cocoa" if IS_MAC else "edgechromium", debug="--debug" in sys.argv)
    quit_app()  # the GUI loop ended: nothing left to show, close everything


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    run()
