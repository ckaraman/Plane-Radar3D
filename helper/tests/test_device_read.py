"""The program reads the card's answers as they come (stage 10b-1). A fake serial port
stands in for pyserial, so this runs on Windows too, without the card."""

import os
import re
import sys
import threading
import time
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


class SerialException(Exception):
    pass


class FakePort:
    """Like pyserial: read(n) waits up to timeout for n bytes, then returns what it has."""

    instance = None

    def __init__(self):
        self.port = self.baudrate = self.write_timeout = None
        self.timeout, self.dtr, self.rts = 0.2, True, True
        self.buf = b""
        self.cv = threading.Condition()
        FakePort.instance = self

    def open(self):
        pass

    def close(self):
        pass

    @property
    def in_waiting(self):
        return len(self.buf)

    def feed(self, data):
        with self.cv:
            self.buf += data
            self.cv.notify_all()

    def read(self, n):
        with self.cv:
            end = time.time() + self.timeout
            while len(self.buf) < n and time.time() < end:
                self.cv.wait(end - time.time())
            out, self.buf = self.buf[:n], self.buf[n:]
            return out

    def write(self, data):
        for line in data.decode("utf-8").split("\n"):
            tag = line[3:].split(" ")[0] if line.startswith("PR:") else ""
            if tag == "HELLO":
                self.feed(b'PR:HELLO {"fw":"5in-0.42.0","usb":"kb","sys":2}\n')
            elif tag in ("QBEGIN", "QITEM", "QEND"):
                self.feed(b"PR:QACK {}\n")
            elif tag == "AEND":
                self.feed(b'PR:AOK {"n":1}\n')
        return len(data)


serial = types.ModuleType("serial")
serial.Serial = FakePort
serial.SerialException = SerialException
serial.tools = types.ModuleType("serial.tools")
serial.tools.list_ports = types.ModuleType("serial.tools.list_ports")
serial.tools.list_ports.comports = lambda: []
sys.modules.update({"serial": serial, "serial.tools": serial.tools, "serial.tools.list_ports": serial.tools.list_ports})

from deck import device  # noqa: E402

device.find_port = lambda pid: "FAKE" if pid == device.PID_KEYBOARD else None
events, perf_lines = [], []


def wait(cond, t=3):
    end = time.time() + t
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.01)
    return False


d = device.Device(lambda tag, data: events.append((tag, data)), log=lambda m: None, perf=perf_lines.append)
assert wait(lambda: d.connected), "no connection"


def test_answers_are_read_at_once():
    rows = [{"x": i, "t": "t%d" % i, "a": "a", "d": "3:00"} for i in range(30)]
    t0 = time.time()
    assert d.send_queue(rows, 1, "YouTube Music")
    took = time.time() - t0
    assert took < 1.5, "%.2f s" % took  # 32 answers; 0.2 s each before (6.5 s on the card)


def test_split_line_is_one_line():
    FakePort.instance.feed(b'PR:KEY {"i"')
    time.sleep(0.05)
    FakePort.instance.feed(b":4}\n")
    assert wait(lambda: ("KEY", {"i": 4}) in events), events


def test_batch_logged():
    assert d.send_batch(["PR:ABEGIN", "PR:A 1"], "PR:AEND 1", "AOK", 3)
    assert any(re.match(r"batch ABEGIN 2 ok \d+\.\d\d s$", l) for l in perf_lines), perf_lines
    device.BATCH_WAIT = 0.1
    with d.stream_lock:
        assert d.send_batch(["PR:ABEGIN"], "PR:AEND 1", "AOK", 3) is None
    assert any(l.startswith("batch ABEGIN 1 skipped") for l in perf_lines), perf_lines


def test_last_line_moves():
    before = d.last_line
    time.sleep(0.02)
    FakePort.instance.feed(b"PR:PERF {}\n")
    assert wait(lambda: d.last_line > before), (before, d.last_line)


if __name__ == "__main__":
    test_answers_are_read_at_once()
    test_split_line_is_one_line()
    test_batch_logged()
    test_last_line_moves()
    d.stop()
    print("tests ok")
