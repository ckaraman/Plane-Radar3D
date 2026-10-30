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


if __name__ == "__main__":
    test_writes_and_rotates()
    test_unwritable_is_quiet()
    print("tests ok")
