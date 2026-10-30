"""perf.log: the card's timing reports (PR:PERF) and the program's send times, one line
each with the time, so a trial on the card can be read afterwards (stage 10a)."""

import json
import os
import sys
import threading
import time
import traceback

SLOW_TURN_MS = 200  # a loop turn this long is a tap the card answers late (spec: 0.2 s)
STALL_S = 15        # the card says PR:PERF every 5 s: this long without a line, the link stalled


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


def thread_stacks():
    """Where every thread of the program waits now (its name and last calls)."""
    frames = sys._current_frames()
    out = []
    for th in threading.enumerate():
        f = frames.get(th.ident)
        if f is None:
            continue
        out.append("-- %s" % th.name)
        out.extend(x.rstrip() for x in traceback.format_stack(f)[-6:])
    return "\n".join(out)


class Stall:
    """Notices the card going quiet (stage 10b-3): once a stall, with every thread's stack
    so perf.log shows where it hangs, and once when the lines come back."""

    def __init__(self):
        self.since = None  # last_line of the stall being reported

    def check(self, now, last_line, watching):
        if not watching:
            self.since = None
            return None
        quiet = now - last_line
        if quiet >= STALL_S:
            if self.since is None:
                self.since = last_line
                return "stall: no line from the card for %d s\n%s" % (quiet, thread_stacks())
            return None
        if self.since is not None:
            gap, self.since = last_line - self.since, None
            return "lines again after %d s" % gap
        return None
