"""perf.log for the card's timing reports (stage 10a). Plain Python: runs on Windows too."""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deck import perf  # noqa: E402


def test_writes_and_rotates():
    p = os.path.join(tempfile.mkdtemp(), "perf.log")
    log = perf.PerfLog(p, limit=200)
    log.write('card {"fps":30}')
    line = open(p, encoding="utf-8").read()
    assert re.match(r'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d card \{"fps":30\}\n$', line), line
    for _ in range(20):
        log.write("x" * 20)
    assert os.path.exists(p + ".1") and os.path.getsize(p) <= 200


def test_unwritable_is_quiet():
    perf.PerfLog(os.path.join(tempfile.mkdtemp(), "yok", "yok", "perf.log")).write("a")  # no exception


def test_window_line_only_when_worth_it():
    # The window log keeps 300 lines: a report every 5 s would push the rest out.
    a = {"scr": "radar", "page": "", "turn_max": 40}
    assert perf.window_line(a, None) is not None  # the first one
    assert perf.window_line(dict(a, turn_max=60), a) is None  # same screen, no slow turn
    assert perf.window_line(dict(a, turn_max=200), a) is not None  # a slow turn (0.2 s)
    assert perf.window_line(dict(a, scr="deck", page="music"), a) is not None  # screen changed


if __name__ == "__main__":
    test_writes_and_rotates()
    test_unwritable_is_quiet()
    test_window_line_only_when_worth_it()
    print("tests ok")
