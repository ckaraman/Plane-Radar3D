"""Queue pictures follow their song (stage 10b-3): after a song change only the pictures
the card does not have yet are sent. Runs on Windows too, without the card."""

import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deck import queue_card  # noqa: E402


def test_carry_moves_by_picture():
    rows = [{"img": u} for u in ("b", "c", "d")]
    assert queue_card.carry_pics({0: "a", 1: "b", 2: "c"}, rows) == ([1, 2, -1], {0: "b", 1: "c"})


def test_carry_same_picture_twice():
    rows = [{"img": "a"}, {"img": "a"}]
    assert queue_card.carry_pics({0: "a"}, rows) == ([0, -1], {0: "a"})


def test_carry_limit():
    rows = [{"img": "a"}, {"img": "b"}]
    assert queue_card.carry_pics({0: "a", 1: "b"}, rows, limit=1) == ([0, -1], {0: "a"})


class FakeDev:
    def __init__(self, qkeep=True, fail=()):
        self.hello = {"queuev": 1, "qthumb": 40}
        if qkeep:
            self.hello["qkeep"] = 1
        self.lists, self.thumbs, self.fail = [], [], set(fail)

    def send_queue(self, rows, gen, source):
        self.lists.append(rows)
        return True

    def clear_queue(self):
        return True

    def send_thumb(self, jpeg, k, gen, abort=lambda: False):
        self.thumbs.append(k)
        return k not in self.fail


def sender(urls):
    core = SimpleNamespace(config=SimpleNamespace(settings={}), link=SimpleNamespace(queue=None), now={})
    q = queue_card.QueueSender(core, log=lambda m: None)
    q.set_rows = lambda us: setattr(q, "rows", lambda: [{"x": i, "t": u, "a": "", "d": "", "img": u}
                                                        for i, u in enumerate(us)])
    q.set_rows(urls)
    for u in "abcd":
        q.cache[(u, 40)] = b"j"
    return q


def run(q, dev):
    for _ in range(20):
        q.pump(dev)


def song_change(q, dev, urls):
    q.set_rows(urls)
    q.last_send, q.dirty = 0.0, True
    dev.thumbs.clear()
    run(q, dev)


def test_song_change_sends_only_new_pictures():
    dev = FakeDev()
    q = sender("abc")
    run(q, dev)
    assert sorted(dev.thumbs) == [0, 1, 2], dev.thumbs
    song_change(q, dev, "bcd")
    assert [r.get("p") for r in dev.lists[-1]] == [1, 2, None], dev.lists[-1]
    assert dev.thumbs == [2], dev.thumbs


def test_no_qkeep_sends_all_again():
    dev = FakeDev(qkeep=False)
    q = sender("abc")
    run(q, dev)
    song_change(q, dev, "bcd")
    assert all("p" not in r for r in dev.lists[-1]), dev.lists[-1]
    assert sorted(dev.thumbs) == [0, 1, 2], dev.thumbs


def test_failed_picture_not_carried():
    dev = FakeDev(fail={0})
    q = sender("abc")
    run(q, dev)
    assert dev.thumbs.count(0) == 1  # not tried again
    dev.fail.clear()
    song_change(q, dev, "abd")
    assert "p" not in dev.lists[-1][0], dev.lists[-1]  # the card never had "a"
    assert sorted(dev.thumbs) == [0, 2], dev.thumbs  # sent as new (tried again with a new list)


def test_aborted_picture_tried_again():
    dev = FakeDev(fail={0})
    q = sender("abc")
    real = dev.send_thumb

    def send_thumb(jpeg, k, gen, abort=lambda: False):
        if k == 0:
            q.dirty = True  # a new list came while it went: the transfer stops
        return real(jpeg, k, gen, abort)

    dev.send_thumb = send_thumb
    q.pump(dev)
    assert "a" not in q.failed, q.failed
    dev.fail.clear()
    song_change(q, dev, "abc")
    assert 0 in dev.thumbs, dev.thumbs


def test_uncertain_list_send_carries_nothing():
    # The card may have taken the list (and moved its pictures) with its answer lost: the
    # next list must not say "p" against rows the card no longer has.
    dev = FakeDev()
    q = sender("abc")
    run(q, dev)
    dev.send_queue = lambda rows, gen, source: dev.lists.append(rows) and False
    q.set_rows("bcd")
    q.last_send, q.dirty = 0.0, True
    q.pump(dev)
    dev.send_queue = lambda rows, gen, source: dev.lists.append(rows) or True
    q.last_send, q.dirty = 0.0, True
    dev.thumbs.clear()
    run(q, dev)
    assert all("p" not in r for r in dev.lists[-1]), dev.lists[-1]
    assert sorted(dev.thumbs) == [0, 1, 2], dev.thumbs


def test_failed_picture_tried_with_the_next_list():
    dev = FakeDev(fail={0})
    q = sender("abc")
    run(q, dev)
    dev.fail.clear()
    song_change(q, dev, "bca")
    assert 2 in dev.thumbs, dev.thumbs  # "a" again, now row 2


if __name__ == "__main__":
    test_carry_moves_by_picture()
    test_carry_same_picture_twice()
    test_carry_limit()
    test_song_change_sends_only_new_pictures()
    test_no_qkeep_sends_all_again()
    test_failed_picture_not_carried()
    test_aborted_picture_tried_again()
    test_uncertain_list_send_carries_nothing()
    test_failed_picture_tried_with_the_next_list()
    print("queue thumbs ok")
