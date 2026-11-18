"""Events: reads only what was published after it started, matches a mod's prefixed topics, waits and times out."""

import threading

import pytest

from devbench import DevbenchError, Events


def test_head_seq_asks_past_the_ring(fake, bench):
    fake.publish("a")
    fake.publish("b")

    assert bench.head_seq() == 2
    assert fake.calls[-1] == ("GET", f"/api/events?since={2**64 - 1}", None)


def test_events_since_returns_the_later_events(fake, bench):
    for topic in ("a", "b", "c"):
        fake.publish(topic)

    assert [event["topic"] for event in bench.events_since(1)] == ["b", "c"]


def test_events_before_the_start_are_not_seen(fake, bench):
    fake.publish("gesture", {"gesture": "old"})
    events = Events(bench)
    fake.publish("gesture", {"gesture": "new"})
    fake.publish("zones")

    assert [event["data"] for event in events.all("gesture")] == [{"gesture": "new"}]
    assert [event["topic"] for event in events.all()] == ["gesture", "zones"]


def test_all_keeps_events_across_reads(fake, bench):
    events = Events(bench)
    fake.publish("gesture", 1)
    assert len(events.all("gesture")) == 1
    fake.publish("gesture", 2)

    assert [event["data"] for event in events.all("gesture")] == [1, 2]


def test_a_prefixed_topic_matches_bare(fake, bench):
    events = Events(bench, prefix="flashlight")
    fake.publish("flashlight.gesture", 1)
    fake.publish("gesture", 2)
    fake.publish("other.gesture", 3)

    assert [event["data"] for event in events.all("gesture")] == [1, 2]


def test_without_a_prefix_only_the_exact_topic_matches(fake, bench):
    events = Events(bench)
    fake.publish("flashlight.gesture", 1)
    fake.publish("gesture", 2)

    assert [event["data"] for event in events.all("gesture")] == [2]


def test_wait_returns_the_first_match_of_where(fake, bench):
    events = Events(bench, prefix="flashlight")
    fake.publish("flashlight.gesture", {"gesture": "a"})
    fake.publish("flashlight.gesture", {"gesture": "b"})
    fake.publish("flashlight.gesture", {"gesture": "b", "second": True})

    event = events.wait("gesture", timeout=1.0, where=lambda data: data["gesture"] == "b")

    assert event["seq"] == 2
    assert event["topic"] == "flashlight.gesture"


def test_wait_sees_an_event_published_while_waiting(fake, bench):
    events = Events(bench)
    timer = threading.Timer(0.2, fake.publish, args=("light", {"on": True}))
    timer.start()
    try:
        assert events.wait("light", timeout=5.0)["data"] == {"on": True}
    finally:
        timer.cancel()


def test_wait_times_out_with_the_topic_in_the_message(fake, bench):
    events = Events(bench, prefix="flashlight")
    fake.publish("flashlight.zones")
    fake.publish("gesture", {"gesture": "a"})

    with pytest.raises(TimeoutError) as raised:
        events.wait("gesture", timeout=0.2, where=lambda data: data["gesture"] == "b")

    message = str(raised.value)
    assert "no event 'gesture' (prefix flashlight) matched within 0.2 s" in message
    assert "['flashlight.zones', 'gesture']" in message


def test_events_lost_to_the_ring_raise(fake, bench):
    events = Events(bench)
    # the ring dropped seqs 1 and 2 before the first read
    fake.events = [{"seq": seq, "frame": 0, "topic": "t", "data": None} for seq in (3, 4)]

    with pytest.raises(DevbenchError, match="2 event"):
        events.all()


def test_a_restarted_game_raises(fake, bench):
    for _ in range(5):
        fake.publish("t")
    events = Events(bench)
    fake.events = []
    fake.publish("t")

    with pytest.raises(DevbenchError, match="restarted"):
        events.all()
