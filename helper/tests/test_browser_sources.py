"""The browser link keeps one queue per tab and shows the one whose song plays."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from deck import browser_link, queue_card  # noqa: E402


class Conn:
    def __init__(self):
        self.sent = []

    def send(self, text):
        self.sent.append(text)


def make_link():
    got = []
    link = browser_link.BrowserLink.__new__(browser_link.BrowserLink)
    link._init_state(lambda q: got.append(q), lambda m: None)
    return link, got


def yt(tab=1):
    return {"t": "queue", "tab": tab, "cur": 0, "now": "Gece Yolu",
            "art": "https://i.ytimg.com/vi/a/maxresdefault.jpg",
            "items": [{"t": "Gece Yolu", "a": "Deniz Ay", "d": "3:58", "img": ""},
                      {"t": "Kıyıda", "a": "Mavi Tren", "d": "4:12", "img": ""}]}


def sp(tab=2):
    return {"t": "queue", "tab": tab, "src": "Spotify", "cur": 0, "now": "Bak Sen Keyfine",
            "art": "https://i.scdn.co/image/ab67big",
            "items": [{"t": "Bak Sen Keyfine", "a": "Şaban Gedik", "d": "", "img": "https://i.scdn.co/image/ab67"},
                      {"t": "The Barrel Of Her Gun", "a": "X", "d": "", "img": ""}]}


def test_two_sources_pick_the_playing_one():
    link, _ = make_link()
    c = Conn()
    link._queue(c, yt())
    link._queue(c, sp())
    link.set_now("Gece Yolu")
    assert link.queue["now"] == "Gece Yolu" and link.queue_tab == 1
    assert link.art["art"].startswith("https://i.ytimg.com")
    link.set_now("Bak Sen Keyfine")
    assert link.queue_tab == 2 and link.queue["src"] == "Spotify"
    assert link.art == {"now": "Bak Sen Keyfine", "art": "https://i.scdn.co/image/ab67big"}


def test_no_match_keeps_newest_and_card_rows_none():
    link, got = make_link()
    c = Conn()
    link._queue(c, yt())
    link._queue(c, sp())
    link.set_now("Başka")
    n = len(got)
    for _ in range(5):
        link.set_now("Başka")
    assert link.queue_tab == 2
    assert queue_card.card_rows(link.queue, {"t": "Başka"}) is None
    assert len(got) == n  # no flipping between the two


def test_tab_closed_drops_only_that_source():
    link, _ = make_link()
    c = Conn()
    link._queue(c, yt())
    link._queue(c, sp())
    link._queue(c, {"t": "queue", "items": [], "cur": -1, "tab": 2})
    link.set_now("Bak Sen Keyfine")
    assert link.queue_tab == 1 and len(link.sources) == 1


def test_source_art_only():
    link, _ = make_link()
    c = Conn()
    link._queue(c, {"t": "queue", "tab": 3, "src": "Spotify", "items": [], "cur": -1,
                    "now": "X", "art": "https://i.scdn.co/image/ab"})
    assert link.queue is None
    assert link.art == {"now": "X", "art": "https://i.scdn.co/image/ab"}
    assert link.queue_conn is c


def test_connection_closed_drops_its_sources():
    link, got = make_link()
    a, b = Conn(), Conn()
    link._queue(a, yt(1))
    link._queue(b, sp(2))
    link._drop_conn(b)
    assert link.queue_conn is a and link.queue_tab == 1
    link._drop_conn(a)
    assert link.queue is None and link.art is None and link.queue_conn is None
    assert got[-1] is None


def test_old_extension_without_tab():
    import json

    link, _ = make_link()
    c = Conn()
    msg = yt()
    del msg["tab"]
    link._queue(c, msg)
    assert link.queue_tab is None and link.queue["src"] == "YouTube Music"
    assert link.play(1, "Kıyıda")
    assert json.loads(c.sent[-1]) == {"t": "play", "i": 1, "title": "Kıyıda"}


def test_play_goes_to_the_chosen_tab():
    import json

    link, _ = make_link()
    c = Conn()
    link._queue(c, yt())
    link._queue(c, sp())
    link.set_now("Bak Sen Keyfine")
    assert link.play(1, "The Barrel Of Her Gun")
    assert json.loads(c.sent[-1]) == {"t": "play", "i": 1, "title": "The Barrel Of Her Gun", "tab": 2}


def test_spotify_pictures_allowed():
    assert queue_card._allowed("https://i.scdn.co/image/ab67") is True
    assert queue_card._allowed("https://i.ytimg.com/vi/a/hqdefault.jpg") is True
    assert queue_card._allowed("https://evil.example/x.jpg") is False
    assert queue_card._allowed("https://scdn.co.evil.example/x.jpg") is False


def test_queue_source_name():
    from types import SimpleNamespace

    class Dev:
        hello = {"queuev": 1, "qthumb": 40}
        sources = []

        def send_queue(self, rows, gen, source):
            self.sources.append(source)
            return True

        def clear_queue(self):
            return True

        def send_thumb(self, jpeg, k, gen, abort=lambda: False):
            return True

    for q_src, want in (("Spotify", "Spotify"), (None, "YouTube Music")):
        queue = {"items": [{"t": "A", "a": "", "d": "", "img": ""}], "cur": 0, "now": "A"}
        if q_src:
            queue["src"] = q_src
        core = SimpleNamespace(config=SimpleNamespace(settings={}), link=SimpleNamespace(queue=queue), now={"t": "A"})
        qs = queue_card.QueueSender(core, log=lambda m: None)
        dev = Dev()
        dev.sources = []
        for _ in range(3):
            qs.pump(dev)
        assert dev.sources and dev.sources[0] == want, dev.sources


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("tests ok")
