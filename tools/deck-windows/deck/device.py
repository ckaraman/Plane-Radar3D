"""The card on the USB cable: finds it, keeps the serial port open and speaks its line
protocol (firmware services/serial_setup.cpp, services/host_link.h).

Keyboard mode (TinyUSB, VID 303A PID 8150) is the normal state: small lines only, the
card is also a USB keyboard. Uploads (the key set) switch the card to setup mode
(USB-Serial/JTAG, PID 1001) and back, as the setup page does.
"""

import base64
import json
import threading
import time
import zlib

import serial
from serial.tools import list_ports

VID = 0x303A
PID_KEYBOARD = 0x8150
PID_SETUP = 0x1001


def find_port(pid):
    for p in list_ports.comports():
        if p.vid == VID and p.pid == pid:
            return p.device
    return None


class Device:
    """Owns the serial port. on_event(tag, data) gets KEY / SETVOL / MUTE from the card."""

    def __init__(self, on_event, log=print):
        self.on_event = on_event
        self.log = log
        self.ser = None
        self.port = None
        self.hello = None
        self.lock = threading.Lock()        # one writer at a time
        self.req_lock = threading.Lock()    # one request waiting for an answer
        self.pending = None                 # (tags, event, holder)
        self.released = False               # user asked to leave the port to the setup page
        self.busy = False                   # an upload runs: the watcher keeps out
        self._stop = False
        self._reader = None
        threading.Thread(target=self._watch, name="device-watch", daemon=True).start()

    # ------------------------------------------------------------------ state
    @property
    def connected(self):
        return self.ser is not None and self.hello is not None

    def status(self):
        h = self.hello or {}
        return {
            "connected": self.connected, "port": self.port, "released": self.released,
            "fw": h.get("fw"), "usb": h.get("usb"), "hostv": h.get("hostv"),
            "w": h.get("w", 800), "h": h.get("h", 480), "deckicon": h.get("deckicon", 72),
        }

    def release(self, released):
        """Leave the port free for the setup page (True) or take it back (False)."""
        self.released = bool(released)
        if self.released:
            self._close()

    def stop(self):
        self._stop = True
        self._close()

    # ------------------------------------------------------------------ connection
    def _watch(self):
        while not self._stop:
            if self.ser is None and not self.released and not self.busy:
                port = find_port(PID_KEYBOARD)
                if port:
                    self._open(port)
            time.sleep(1.0)

    def _open(self, port):
        try:
            ser = serial.Serial(port, 115200, timeout=0.2, write_timeout=2)
        except (serial.SerialException, OSError) as e:
            self.log("port %s: %s" % (port, e))
            return False
        self.ser, self.port, self.hello = ser, port, None
        self._reader = threading.Thread(target=self._read_loop, args=(ser,), name="device-read", daemon=True)
        self._reader.start()
        for _ in range(8):
            r = self.request("PR:HELLO", ["HELLO"], 1.5)
            if r:
                self.hello = r[1]
                self.log("card on %s, %s, USB %s" % (port, self.hello.get("fw"), self.hello.get("usb")))
                return True
            if self.ser is None:
                return False
        self.log("card on %s does not answer" % port)
        self._close()
        return False

    def _close(self):
        ser, self.ser, self.hello = self.ser, None, None
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
        p = self.pending
        if p:
            p[1].set()

    def _read_loop(self, ser):
        buf = b""
        while self.ser is ser:
            try:
                chunk = ser.read(512)
            except (serial.SerialException, OSError, TypeError, AttributeError):
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._line(line.decode("utf-8", "replace").strip())
            if len(buf) > 20000:
                buf = buf[-2000:]
        if self.ser is ser:
            self.log("card gone")
            self._close()

    def _line(self, line):
        at = line.find("PR:")
        if at < 0:
            return
        body = line[at + 3:]
        tag, _, rest = body.partition(" ")
        try:
            data = json.loads(rest) if rest else {}
        except ValueError:
            return
        p = self.pending
        if p and tag in p[0]:
            p[2].append((tag, data))
            p[1].set()
            return
        if tag in ("KEY", "SETVOL", "MUTE"):
            try:
                self.on_event(tag, data)
            except Exception as e:  # noqa: BLE001
                self.log("event %s: %s" % (tag, e))

    # ------------------------------------------------------------------ I/O
    def send(self, text):
        ser = self.ser
        if ser is None:
            return False
        with self.lock:
            try:
                ser.write(("\n" + text + "\n").encode("utf-8"))
                return True
            except (serial.SerialException, OSError) as e:
                self.log("write: %s" % e)
                self._close()
                return False

    def request(self, text, tags, timeout):
        with self.req_lock:
            ev, holder = threading.Event(), []
            self.pending = (tags, ev, holder)
            try:
                if not self.send(text):
                    return None
                ev.wait(timeout)
                return holder[0] if holder else None
            finally:
                self.pending = None

    def send_json(self, tag, obj):
        return self.send("PR:%s %s" % (tag, json.dumps(obj, ensure_ascii=False, separators=(",", ":"))))

    # ------------------------------------------------------------------ uploads
    def _chunked(self, begin_tag, chunk_tag, end_tag, ack_tag, blob, chunk, extra=None, end_tags=("OK", "ERR")):
        meta = {"len": len(blob), "crc": zlib.crc32(blob) & 0xFFFFFFFF}
        if extra:
            meta.update(extra)
        r = self.request("PR:%s %s" % (begin_tag, json.dumps(meta)), [ack_tag, "ERR"], 5)
        if not r or r[0] != ack_tag:
            raise RuntimeError((r[1].get("msg") if r else "no answer") or "error")
        seq = 1
        for off in range(0, len(blob), chunk):
            part = base64.b64encode(blob[off:off + chunk]).decode("ascii")
            for attempt in range(3):
                r = self.request("PR:%s %d %s" % (chunk_tag, seq, part), [ack_tag, "ERR"], 4)
                if r and r[0] == ack_tag and r[1].get("seq") == seq:
                    break
                if r and r[0] == "ERR":
                    raise RuntimeError(r[1].get("msg", "error"))
            else:
                raise RuntimeError("no answer")
            seq += 1
        r = self.request("PR:" + end_tag, list(end_tags), 10)
        if not r or r[0] != "OK":
            raise RuntimeError((r[1].get("msg") if r else "no answer") or "error")

    def send_cover(self, jpeg, track_id):
        """Album cover in keyboard mode: small acknowledged chunks (see the module doc)."""
        if not self.connected or self.busy:
            return False
        try:
            self._chunked("COVERBEGIN", "COVER", "COVEREND", "COVERACK", jpeg, 180, {"id": track_id})
            return True
        except RuntimeError as e:
            self.log("cover: %s" % e)
            return False

    def upload_deck(self, blob, progress=lambda pct, text: None):
        """The key set: to setup mode, upload, back to keyboard mode."""
        if self.busy:
            raise RuntimeError("busy")
        self.busy = True
        try:
            progress(2, "Kart kurulum moduna geçiyor…")
            if self.connected:
                self.request("PR:USBSETUP", ["USBMODE"], 3)
            self._close()
            port = None
            for _ in range(40):
                time.sleep(0.5)
                port = find_port(PID_SETUP)
                if port:
                    break
            if not port:
                raise RuntimeError("Kart kurulum modunda görünmedi")
            time.sleep(0.8)
            if not self._open(port):
                raise RuntimeError("Kart kurulum modunda yanıt vermedi")
            progress(10, "Tuşlar gönderiliyor…")
            total = len(blob)
            meta = {"len": total, "crc": zlib.crc32(blob) & 0xFFFFFFFF}
            r = self.request("PR:DECKBEGIN " + json.dumps(meta), ["DECKACK", "ERR"], 5)
            if not r or r[0] != "DECKACK":
                raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
            seq, CH = 1, 360
            for off in range(0, total, CH):
                part = base64.b64encode(blob[off:off + CH]).decode("ascii")
                r = self.request("PR:DECK %d %s" % (seq, part), ["DECKACK", "ERR"], 5)
                if not r or r[0] != "DECKACK" or r[1].get("seq") != seq:
                    raise RuntimeError((r[1].get("msg") if r and r[0] == "ERR" else "bağlantı koptu"))
                seq += 1
                progress(10 + int(85 * min(1, (off + CH) / total)), "Tuşlar gönderiliyor…")
            r = self.request("PR:DECKEND", ["OK", "ERR"], 10)
            if not r or r[0] != "OK":
                raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
            progress(97, "Kart klavye moduna dönüyor…")
            self.request("PR:USBNORMAL", ["USBMODE"], 3)
            self._close()
        finally:
            self.busy = False  # the watcher reconnects in keyboard mode
        for _ in range(30):
            time.sleep(0.5)
            if self.connected:
                break
        progress(100, "Tuşlar kartta.")
