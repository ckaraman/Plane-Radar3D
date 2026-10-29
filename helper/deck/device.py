"""The card on the USB cable: finds it, keeps the serial port open and speaks its line
protocol (firmware services/serial_setup.cpp, services/host_link.h).

Keyboard mode (TinyUSB, VID 303A PID 8150) is the normal state: small lines only, the
card is also a USB keyboard. Uploads (the key set) switch the card to setup mode
(USB-Serial/JTAG, PID 1001) and back, as the setup page does.
"""

import base64
import json
import queue
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
        self.stream_lock = threading.Lock() # one chunked transfer at a time
        self._stream = None                 # (tags, queue) of the running transfer
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
            "deckv": h.get("deckv", 1), "deckkw": h.get("deckkw"), "deckkh": h.get("deckkh"),
            "deckarea": h.get("deckarea"),
            "busy": self.busy,
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
        setup_since = None
        while not self._stop:
            if self.ser is None and not self.released and not self.busy:
                port = find_port(PID_KEYBOARD)
                if port:
                    setup_since = None
                    self._open(port)
                elif find_port(PID_SETUP):
                    # Left in setup mode (an upload that broke off, the setup page): after a
                    # short wait, back to keyboard mode so the keys work again.
                    setup_since = setup_since or time.time()
                    if time.time() - setup_since > 8:
                        setup_since = None
                        sp = find_port(PID_SETUP)
                        if sp and self._open(sp, setup=True):
                            self.log("card was in setup mode: back to keyboard mode")
                            self.request("PR:USBNORMAL", ["USBMODE"], 3)
                            self._close()
                else:
                    setup_since = None
            time.sleep(1.0)

    def _open(self, port, setup=False):
        try:
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout, ser.write_timeout = port, 115200, 0.2, 2
            if setup:
                # USB-Serial/JTAG resets the chip on some DTR / RTS changes: leave both alone.
                ser.dtr, ser.rts = False, False
            ser.open()
        except (serial.SerialException, OSError) as e:
            msg = "port %s: %s" % (port, e)
            if msg != getattr(self, "_last_port_error", None):  # not every second
                self._last_port_error = msg
                self.log(msg)
            return False
        self._last_port_error = None
        self.ser, self.port, self.hello = ser, port, None
        self._reader = threading.Thread(target=self._read_loop, args=(ser,), name="device-read", daemon=True)
        self._reader.start()
        for _ in range(8):
            r = self.request("PR:HELLO", ["HELLO"], 1.5)
            if r:
                self.hello = r[1]
                self.log("card on %s, %s, USB %s%s" % (port, self.hello.get("fw"), self.hello.get("usb"),
                                                      ", USB link reconnected %s times" % self.hello["usbfix"]
                                                      if self.hello.get("usbfix") else ""))
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
        st = self._stream
        if st and tag in st[0]:
            st[1].put((tag, data))
            return
        p = self.pending
        if p and tag in p[0]:
            p[2].append((tag, data))
            p[1].set()
            return
        if tag == "PREFS":  # range / units changed on the card: the feed reads it from hello
            if self.hello is not None:
                self.hello["prefs"] = data
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

    def send_batch(self, lines, end=None, ack=None, timeout=0):
        """Short lines, then an end line the card answers (PR:ABEGIN .. PR:AEND -> AOK).
        Not in the middle of a cover / key upload: the card's USB link loses data when
        they mix. Returns the answer (True without an end line) or None (skipped, no answer)."""
        if not self.stream_lock.acquire(timeout=5):
            return None
        try:
            for i, line in enumerate(lines):
                if not self.send(line):
                    return None
                if i % 6 == 5:
                    time.sleep(0.04)  # the radar_kopru pacing: CDC drops long bursts
            return self.request(end, [ack], timeout) if end else True
        finally:
            self.stream_lock.release()

    # ------------------------------------------------------------------ radar settings
    RADAR_KEYS = ("lat", "lon", "tz", "clock", "weather", "miles", "runways", "rings")
    MAP_KEYS = ("hw", "map", "mapv", "elevv")  # read only: what the map for this card needs

    def radar_settings(self):
        """The card's location and screen options (from HELLO), None without a system 2 card."""
        h = self.hello if self.connected else None
        if not h or h.get("sys") != 2:
            return None
        return {k: h.get(k) for k in self.RADAR_KEYS + self.MAP_KEYS}

    def save_radar(self, s):
        """PR:SET without the Wi-Fi fields (docs/PROTOCOL.md). Returns "" or an error code."""
        try:
            lat, lon = float(s["lat"]), float(s["lon"])
        except (KeyError, TypeError, ValueError):
            return "bad_location"
        if not (-90 <= lat <= 90 and -180 <= lon <= 180):
            return "bad_location"
        tz = s.get("tz")
        if not isinstance(tz, str) or not 0 < len(tz) <= 63:  # kTzMaxLen on the card
            return "bad_timezone"
        payload = {"lat": round(lat, 6), "lon": round(lon, 6), "tz": tz}
        for k in ("clock", "weather", "miles", "runways", "rings"):
            if k in s:
                payload[k] = bool(s[k])
        if not self.radar_settings():
            return "no_card"
        r = self.request("PR:SET " + json.dumps(payload, separators=(",", ":")), ["OK", "ERR"], 10)
        if not r:
            return "no_answer"
        if r[0] == "ERR":
            return r[1].get("msg") or "error"
        h = self.hello
        if h is not None:
            h.update(payload)  # the feed uses the new place at once (aircraft and weather)
        return ""

    def send_json(self, tag, obj):
        return self.send("PR:%s %s" % (tag, json.dumps(obj, ensure_ascii=False, separators=(",", ":"))))

    # ------------------------------------------------------------------ uploads
    def _transfer(self, prefix, blob, chunk, window, extra=None, abort=lambda: False,
                  progress=lambda f: None, end_timeout=10):
        """BEGIN / numbered chunks / END for COVER or DECK. With window > 1 several chunks
        are on the way at once; the card answers each with <prefix>ACK, a repeated one
        again, a gap with <prefix>NAK {"need": n} (firmware 0.32+)."""
        ack, nak = prefix + "ACK", prefix + "NAK"
        meta = {"len": len(blob), "crc": zlib.crc32(blob) & 0xFFFFFFFF}
        if extra:
            meta.update(extra)
        with self.stream_lock:
            r = self.request("PR:%sBEGIN %s" % (prefix, json.dumps(meta)), [ack, "ERR"], 5)
            if not r or r[0] != ack:
                raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
            parts = [base64.b64encode(blob[o:o + chunk]).decode("ascii") for o in range(0, len(blob), chunk)]
            n = len(parts)
            q = queue.Queue()
            self._stream = ({ack, nak, "ERR"}, q)
            try:
                done, nxt, rewound, stalls = 0, 1, 0, 0  # done = highest chunk the card has
                while done < n:
                    if abort():
                        raise RuntimeError("aborted")
                    while nxt <= n and nxt - done <= window:
                        if not self.send("PR:%s %d %s" % (prefix, nxt, parts[nxt - 1])):
                            raise RuntimeError("bağlantı koptu")
                        nxt += 1
                    try:
                        tag, data = q.get(timeout=3 if window > 1 else 5)
                    except queue.Empty:
                        stalls += 1
                        if stalls > 4 or (window == 1 and stalls > 2):
                            raise RuntimeError("yanıt yok")
                        nxt = done + 1  # send the rest again
                        continue
                    if tag == "ERR":
                        raise RuntimeError(data.get("msg", "hata"))
                    if tag == ack:
                        stalls = 0
                        seq = int(data.get("seq", 0))
                        if seq > done:
                            done = seq
                            progress(done / n)
                    elif tag == nak:
                        need = int(data.get("need", done + 1))
                        done = max(done, need - 1)
                        if need != rewound:  # once per gap (the chunks behind it say it too)
                            rewound, nxt = need, need
            finally:
                self._stream = None
            r = self.request("PR:%sEND" % prefix, ["OK", "ERR"], end_timeout)
            if not r or r[0] != "OK":
                raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
            return r[1]

    def send_cover(self, jpeg, track_id, w=0, h=0, abort=lambda: False):
        """Album cover in keyboard mode: small chunks, each answered before the next (the
        TinyUSB port stalls on bursts; 0.32.1+ redraws less meanwhile, so it is quick)."""
        if not self.connected or self.busy:
            return False
        win = 1  # keyboard mode: one chunk at a time (the TinyUSB port stalls on bursts)
        try:
            t0 = time.time()
            self._transfer("COVER", jpeg, 180, win, {"id": track_id, "w": w, "h": h},
                           abort=lambda: abort() or self.busy)
            self.log("cover: %d KB in %.1f s" % (len(jpeg) // 1024, time.time() - t0))
            return True
        except RuntimeError as e:
            if str(e) != "aborted":
                self.log("cover: %s" % e)
            return False

    def send_queue(self, rows, gen, source):
        """The queue for the card's panel: QBEGIN, one acknowledged QITEM per row, QEND."""
        if not self.connected or self.busy:
            return False
        try:
            with self.stream_lock:
                for line in (["PR:QBEGIN " + json.dumps({"gen": gen, "n": len(rows), "src": source}, ensure_ascii=False)] +
                             ["PR:QITEM " + json.dumps(dict(r, k=k), ensure_ascii=False, separators=(",", ":"))
                              for k, r in enumerate(rows)] + ["PR:QEND"]):
                    r = self.request(line, ["QACK", "ERR"], 3)
                    if not r or r[0] != "QACK":
                        raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
                    if self.busy:
                        raise RuntimeError("aborted")
            return True
        except RuntimeError as e:
            if str(e) != "aborted":
                self.log("queue: %s" % e)
            return False

    def clear_queue(self):
        return bool(self.connected and self.request("PR:QCLEAR", ["QACK", "ERR"], 3))

    def send_thumb(self, jpeg, k, gen, abort=lambda: False):
        """A small cover for row k of queue gen (like the album cover, one chunk at a time)."""
        if not self.connected or self.busy:
            return False
        try:
            self._transfer("QART", jpeg, 180, 1, {"k": k, "gen": gen}, abort=lambda: abort() or self.busy)
            return True
        except RuntimeError as e:
            if str(e) != "aborted":
                self.log("queue picture: %s" % e)
            return False

    def _in_setup_mode(self, work, progress):
        """Card to setup mode, work(), back to keyboard mode (uploads: key set, map)."""
        if self.busy:
            raise RuntimeError("Başka bir gönderim sürüyor")
        self.busy = True  # the cover transfer stops, the watcher keeps out
        step = "başlangıç"
        try:
            with self.stream_lock:
                pass  # a cover on its way has noticed busy and stopped
            progress(2, "Kart kurulum moduna geçiyor…")
            step = "kurulum moduna geçiş"
            self.log("upload: card %s" % ("connected" if self.connected else "not connected"))
            already_setup = self.connected and (self.hello or {}).get("usb") == "setup"
            if self.connected and not already_setup:
                r = self.request("PR:USBSETUP", ["USBMODE", "ERR"], 3)
                self.log("upload: USBSETUP -> %s" % (r,))
            if not already_setup:
                self._close()
                port = None
                for _ in range(40):
                    time.sleep(0.5)
                    port = find_port(PID_SETUP)
                    if port:
                        break
                if not port:
                    raise RuntimeError("Kart kurulum modunda görünmedi (USB 303A:1001 yok)")
                self.log("upload: setup port %s" % port)
                time.sleep(0.8)
                step = "kurulum portu"
                opened = False
                for _ in range(3):
                    if self._open(port, setup=True):
                        opened = True
                        break
                    time.sleep(1.0)
                if not opened:
                    raise RuntimeError("Kart kurulum modunda yanıt vermedi (%s)" % port)
            step = "gönderim"
            t0 = time.time()
            work()
            self.log("upload: sent in %.1f s" % (time.time() - t0))
            progress(97, "Kart klavye moduna dönüyor…")
            step = "klavye moduna dönüş"
            self.request("PR:USBNORMAL", ["USBMODE"], 3)
            self._close()
        except RuntimeError as e:
            self.log("upload failed at %s: %s" % (step, e))
            if self.ser is not None and (self.hello or {}).get("usb") == "setup":
                self.request("PR:USBNORMAL", ["USBMODE"], 3)  # do not leave the card in setup mode
                self._close()
            raise
        finally:
            self.busy = False  # the watcher reconnects in keyboard mode
        for _ in range(30):
            time.sleep(0.5)
            if self.connected:
                break

    def upload_firmware(self, blob, progress=lambda pct, text: None):
        """New firmware into the card's second app slot, as the setup page's usbUpdate (firmware
        services/serial_ota.cpp): the card switches to it only when all of it arrived intact.
        Returns the card's firmware version after its restart."""
        self.log("upload: firmware %d KB" % (len(blob) // 1024))
        ch, total = 480, (len(blob) + 479) // 480
        meta = json.dumps({"len": len(blob), "crc": zlib.crc32(blob) & 0xFFFFFFFF})

        new_fw = []

        def work():
            progress(8, "Yazılım karta gönderiliyor…")
            with self.stream_lock:  # no aircraft batch or cover in the middle of the stream
                send_all()
            # The card restarts on the new firmware, still in setup mode: reopen it there (its
            # HELLO tells the new version) so _in_setup_mode sends it back to keyboard mode.
            progress(95, "Kart yeniden başlıyor…")
            self._close()
            end = time.time() + 60
            while time.time() < end:
                time.sleep(1.0)
                port = find_port(PID_SETUP)
                if port and self._open(port, setup=True):
                    new_fw.append(self.hello.get("fw") or "")
                    return
            raise RuntimeError("Kart yeniden başladıktan sonra görünmedi")

        def send_all():
            for attempt in range(1, 6):  # a busy card can miss the start line
                r = self.request("PR:OTABEGIN " + meta, ["OTAACK", "ERR"], 20)
                if r and r[0] == "OTAACK":
                    break
                why = r[1].get("msg", "hata") if r else "yanıt yok"
                if (r and why != "bad_json") or attempt == 5:
                    raise RuntimeError(why)
                time.sleep(1.5 * attempt)
            q = queue.Queue()
            self._stream = ({"OTAACK", "OTANAK", "ERR"}, q)
            try:
                seq, tries = 1, 0
                while seq <= total:
                    part = blob[(seq - 1) * ch:seq * ch]
                    while not q.empty():  # answers that came after their time ran out
                        q.get_nowait()
                    # One chunk at a time: while the chip writes flash it takes in no more bytes.
                    if not self.send("PR:OTA %d %d %s" % (seq, len(part), base64.b64encode(part).decode("ascii"))):
                        raise RuntimeError("bağlantı koptu")
                    tag, data, wait_end = None, {}, time.time() + 4
                    while time.time() < wait_end:
                        try:
                            tag, data = q.get(timeout=max(0.01, wait_end - time.time()))
                        except queue.Empty:
                            tag, data = None, {}
                            break
                        if not (tag == "OTAACK" and int(data.get("seq", -1)) < seq):
                            break  # else a late answer to an earlier chunk: keep waiting
                    if tag == "OTAACK" and int(data.get("seq", -1)) == seq:
                        seq, tries = seq + 1, 0
                        if seq % 8 == 0:
                            pct = int(100 * (seq - 1) / total)
                            progress(10 + int(0.8 * pct), "Yazılım karta gönderiliyor: %%%d. Kabloyu çıkarmayın." % pct)
                        continue
                    if tag == "ERR":
                        raise RuntimeError(data.get("msg", "hata"))
                    if tag == "OTANAK":
                        seq = max(1, int(data.get("seq", seq)))
                    tries += 1
                    if tries > 8:
                        raise RuntimeError("kart yanıt vermedi")
            except RuntimeError:
                self.send("PR:OTAABORT")
                raise
            finally:
                self._stream = None
            progress(92, "Doğrulanıyor…")
            r = self.request("PR:OTAEND", ["OTADONE", "ERR"], 30)
            if not r or r[0] != "OTADONE":
                raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
        self._in_setup_mode(work, progress)
        progress(100, "Kart güncellendi.")
        return new_fw[0] if new_fw else ""

    def upload_deck(self, blob, progress=lambda pct, text: None):
        """The key set: to setup mode, upload, back to keyboard mode."""
        self.log("upload: key set %d KB" % (len(blob) // 1024))

        def work():
            progress(10, "Tuşlar gönderiliyor…")
            win = 4 if int((self.hello or {}).get("deckv", 1)) >= 2 else 1
            self._transfer("DECK", blob, 360, win,
                           progress=lambda f: progress(10 + int(85 * f), "Tuşlar gönderiliyor…"))
        self._in_setup_mode(work, progress)
        progress(100, "Tuşlar kartta.")

    def _ack(self, text, tag, timeout=5):
        r = self.request(text, [tag, "ERR"], timeout)
        if not r or r[0] != tag:
            raise RuntimeError((r[1].get("msg") if r else "yanıt yok") or "hata")
        return r[1]

    def upload_map(self, blob, elev, progress=lambda pct, text: None):
        """Map blob and terrain {n, half, lat, lon, h} from web/mapdata.js, in setup mode as on
        the setup page (PR:MAP*, PR:ELEV*; firmware map_store.h, terrain.h). blob None clears
        the map and the terrain; elev None leaves the terrain as it is."""
        self.log("upload: map %s, terrain %s" % ("%d KB" % (len(blob) // 1024) if blob else "clear",
                                                 "yes" if elev else "no"))

        def work():
            if not blob:
                self._ack("PR:MAPCLEAR", "OK")
                self._ack("PR:ELEVCLEAR", "OK")
                return
            progress(10, "Harita karta yazılıyor…")
            self._transfer("MAP", blob, 270, 1, progress=lambda f: progress(10 + int(75 * f), "Harita karta yazılıyor…"))
            if elev:
                progress(88, "Arazi yükseklikleri yazılıyor…")
                n, h = int(elev["n"]), [int(v) for v in elev["h"]]
                if len(h) != n * n:
                    raise RuntimeError("arazi verisi eksik")
                self._ack("PR:ELEVBEGIN " + json.dumps({k: elev[k] for k in ("n", "half", "lat", "lon")}), "ELEVACK")
                for r in range(n):
                    self._ack("PR:ELEVROW %d %s" % (r, ",".join(map(str, h[r * n:(r + 1) * n]))), "ELEVACK")
                self._ack("PR:ELEVEND", "OK")
        self._in_setup_mode(work, progress)
        progress(100, "Harita kartta.")
