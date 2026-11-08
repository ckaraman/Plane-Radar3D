"""The default speaker's volume (Windows Core Audio): read for the card, set from its knob."""

import queue
import threading
import time

from . import mic

try:
    import comtypes
    from comtypes import CLSCTX_ALL
    from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume
except ImportError:  # not on Windows (tests)
    comtypes = None


class VolumeWatcher:
    """on_change(level 0..100, muted) when the volume changes; set() / toggle_mute()."""

    def __init__(self, on_change, log=print):
        self.on_change = on_change
        self.log = log
        self.level, self.muted = 50, False
        self.mic = mic.NONE
        self._mic_eps = []
        self._q = queue.Queue()
        threading.Thread(target=self._run, name="volume", daemon=True).start()

    def set(self, level):
        self._q.put(("set", max(0, min(100, int(level)))))

    def toggle_mute(self):
        self._q.put(("mute", None))

    def toggle_mic(self):
        self._q.put(("mic", None))

    def _mics(self):
        """Every active capture device; trouble with one or all never touches the speaker."""
        eps = []
        try:
            found = AudioUtilities.GetDeviceEnumerator().EnumAudioEndpoints(1, 1)  # eCapture, ACTIVE
            for i in range(found.GetCount()):
                try:
                    iface = found.Item(i).Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
                    eps.append(iface.QueryInterface(IAudioEndpointVolume))
                except Exception as e:  # noqa: BLE001
                    self.log("mic %d: %s" % (i, e))
        except Exception as e:  # noqa: BLE001
            self.log("mics: %s" % e)
        return eps

    def _endpoint(self):
        dev = AudioUtilities.GetSpeakers()
        ep = getattr(dev, "EndpointVolume", None)  # newer pycaw
        if ep is not None:
            return ep
        iface = dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        return iface.QueryInterface(IAudioEndpointVolume)

    def _run(self):
        if comtypes is None:
            return
        comtypes.CoInitialize()
        ep, last_dev_check = None, 0.0
        while True:
            try:
                # The default device can change (headphones plugged in): look again now and then.
                if ep is None or time.time() - last_dev_check > 5:
                    ep, last_dev_check = self._endpoint(), time.time()
                    self._mic_eps = self._mics()
                try:
                    cmd, arg = self._q.get(timeout=0.25)
                except queue.Empty:
                    cmd = None
                # A knob turn sends many levels: only the newest one counts.
                while cmd == "set" and not self._q.empty():
                    nxt = self._q.get_nowait()
                    if nxt[0] != "set":
                        self._q.put(nxt)
                        break
                    arg = nxt[1]
                if cmd == "set":
                    ep.SetMasterVolumeLevelScalar(arg / 100.0, None)
                    if arg > 0 and ep.GetMute():
                        ep.SetMute(0, None)
                elif cmd == "mute":
                    ep.SetMute(0 if ep.GetMute() else 1, None)
                elif cmd == "mic":
                    mic.toggle(self._mic_eps)
                level = int(round(ep.GetMasterVolumeLevelScalar() * 100))
                muted = bool(ep.GetMute())
                mic_state = mic.state_of(self._mic_eps)
                if (level, muted, mic_state) != (self.level, self.muted, self.mic):
                    self.level, self.muted, self.mic = level, muted, mic_state
                    self.on_change(level, muted)
            except Exception as e:  # noqa: BLE001 - no audio device for a moment
                self.log("volume: %s" % e)
                ep = None
                time.sleep(2)
