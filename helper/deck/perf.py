"""perf.log: the card's timing reports (PR:PERF) and the program's send times, one line
each with the time, so a trial on the card can be read afterwards (stage 10a)."""

import os
import threading
import time


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
