"""The Mac's output volume (Core Audio, the same value as the menu bar slider): read for
the card, set from its knob. Same interface as volume.VolumeWatcher."""

import ctypes
import queue
import struct
import subprocess
import threading
import time


def _fourcc(s):
    return struct.unpack(">I", s.encode("ascii"))[0]


class _Addr(ctypes.Structure):
    _fields_ = [("selector", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("element", ctypes.c_uint32)]


SYSTEM_OBJECT = 1
DEFAULT_OUTPUT = _Addr(_fourcc("dOut"), _fourcc("glob"), 0)
MAIN_VOLUME = _Addr(_fourcc("vmvc"), _fourcc("outp"), 0)  # kAudioHardwareServiceDeviceProperty_VirtualMainVolume
MUTE = _Addr(_fourcc("mute"), _fourcc("outp"), 0)


class CoreAudio:
    def __init__(self):
        self.ca = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreAudio.framework/CoreAudio")
        self.at = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox")
        for f in (self.ca.AudioObjectGetPropertyData, self.at.AudioHardwareServiceGetPropertyData):
            f.argtypes = [ctypes.c_uint32, ctypes.POINTER(_Addr), ctypes.c_uint32, ctypes.c_void_p,
                          ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
            f.restype = ctypes.c_int32
        for f in (self.ca.AudioObjectSetPropertyData, self.at.AudioHardwareServiceSetPropertyData):
            f.argtypes = [ctypes.c_uint32, ctypes.POINTER(_Addr), ctypes.c_uint32, ctypes.c_void_p,
                          ctypes.c_uint32, ctypes.c_void_p]
            f.restype = ctypes.c_int32

    def device(self):
        dev, size = ctypes.c_uint32(0), ctypes.c_uint32(4)
        err = self.ca.AudioObjectGetPropertyData(SYSTEM_OBJECT, ctypes.byref(DEFAULT_OUTPUT), 0, None,
                                                 ctypes.byref(size), ctypes.byref(dev))
        if err:
            raise OSError("default output: %d" % err)
        return dev.value

    def volume(self, dev):
        v, size = ctypes.c_float(0), ctypes.c_uint32(4)
        err = self.at.AudioHardwareServiceGetPropertyData(dev, ctypes.byref(MAIN_VOLUME), 0, None,
                                                          ctypes.byref(size), ctypes.byref(v))
        if err:
            raise OSError("volume: %d" % err)
        return v.value

    def set_volume(self, dev, value):
        v = ctypes.c_float(max(0.0, min(1.0, value)))
        err = self.at.AudioHardwareServiceSetPropertyData(dev, ctypes.byref(MAIN_VOLUME), 0, None, 4, ctypes.byref(v))
        if err:
            raise OSError("set volume: %d" % err)

    def muted(self, dev):
        m, size = ctypes.c_uint32(0), ctypes.c_uint32(4)
        if self.ca.AudioObjectGetPropertyData(dev, ctypes.byref(MUTE), 0, None, ctypes.byref(size), ctypes.byref(m)):
            return False  # some outputs have no mute
        return bool(m.value)

    def set_muted(self, dev, on):
        m = ctypes.c_uint32(1 if on else 0)
        if self.ca.AudioObjectSetPropertyData(dev, ctypes.byref(MUTE), 0, None, 4, ctypes.byref(m)):
            raise OSError("mute")


class _AppleScript:
    """Fallback for outputs Core Audio cannot set (some HDMI / USB devices)."""

    @staticmethod
    def _osa(script):
        return subprocess.run(["/usr/bin/osascript", "-e", script], capture_output=True, text=True, timeout=5).stdout.strip()

    def device(self):
        return 0

    def volume(self, _dev):
        return float(self._osa("output volume of (get volume settings)") or 0) / 100.0

    def set_volume(self, _dev, value):
        self._osa("set volume output volume %d" % round(value * 100))

    def muted(self, _dev):
        return self._osa("output muted of (get volume settings)") == "true"

    def set_muted(self, _dev, on):
        self._osa("set volume output muted %s" % ("true" if on else "false"))


class VolumeWatcher:
    """on_change(level 0..100, muted) when the volume changes; set() / toggle_mute()."""

    def __init__(self, on_change, log=print):
        self.on_change = on_change
        self.log = log
        self.level, self.muted = 50, False
        self._q = queue.Queue()
        threading.Thread(target=self._run, name="volume", daemon=True).start()

    def set(self, level):
        self._q.put(("set", max(0, min(100, int(level)))))

    def toggle_mute(self):
        self._q.put(("mute", None))

    def step(self, delta):
        self._q.put(("set", max(0, min(100, self.level + int(delta)))))

    def _run(self):
        try:
            api = CoreAudio()
        except OSError as e:
            self.log("volume: %s" % e)
            api = _AppleScript()
        dev, last_dev_check, slow = None, 0.0, False
        while True:
            try:
                if dev is None or time.time() - last_dev_check > 3:
                    dev, last_dev_check = api.device(), time.time()  # headphones in / out
                try:
                    cmd, arg = self._q.get(timeout=0.25 if not slow else 1.0)
                except queue.Empty:
                    cmd = None
                while cmd == "set" and not self._q.empty():  # a knob turn: only the newest
                    nxt = self._q.get_nowait()
                    if nxt[0] != "set":
                        self._q.put(nxt)
                        break
                    arg = nxt[1]
                if cmd == "set":
                    api.set_volume(dev, arg / 100.0)
                    if arg > 0 and api.muted(dev):
                        api.set_muted(dev, False)
                elif cmd == "mute":
                    api.set_muted(dev, not api.muted(dev))
                level = int(round(api.volume(dev) * 100))
                muted = api.muted(dev)
                if (level, muted) != (self.level, self.muted):
                    self.level, self.muted = level, muted
                    self.on_change(level, muted)
            except Exception as e:  # noqa: BLE001
                if isinstance(api, CoreAudio):
                    self.log("volume: %s, using AppleScript" % e)
                    api, slow = _AppleScript(), True  # osascript is slower: poll less often
                else:
                    self.log("volume: %s" % e)
                    time.sleep(2)
                dev = None
