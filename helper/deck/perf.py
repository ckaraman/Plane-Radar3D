"""perf.log: the card's timing reports (PR:PERF) and the program's send times, one line
each with the time, so a trial on the card can be read afterwards (stage 10a)."""

import json
import os
import threading
import time

SLOW_TURN_MS = 200  # a loop turn this long is a tap the card answers late (spec: 0.2 s)


def window_line(data, last):
    """The report as a line for the program window, or None: only the first one, a screen
    change and a slow turn (the window keeps few lines; perf.log keeps them all)."""
    if last is not None and (data.get("scr"), data.get("page")) == (last.get("scr"), last.get("page")) \
            and (data.get("turn_max") or 0) < SLOW_TURN_MS:
        return None
    return "perf: %s" % json.dumps(data)


class PerfLog:
    def __init__(self, path, limit=1_000_000):
        self.path = path
        self.limit = limit
        self.lock = threading.Lock()

    def write(self, text):
        line = "%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), text)
        with self.lock:
            try:
                if os.path.exists(self.path) and os.path.getsize(self.path) + len(line.encode("utf-8")) > self.limit:
                    os.replace(self.path, self.path + ".1")
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(line)
            except OSError:
                pass  # no log is better than no program
