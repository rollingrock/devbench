"""wait_until and wait_for: return the first accepted value, or raise TimeoutError naming the condition (wait_for:
and the last value)."""

import pytest

from devbench import wait_for, wait_until


def test_returns_the_first_true_value():
    answers = iter([None, 0, {"found": 1}])

    assert wait_until(lambda: next(answers), timeout=5.0, interval=0.0) == {"found": 1}


def test_checks_once_with_a_zero_timeout():
    assert wait_until(lambda: "ready", timeout=0.0) == "ready"


def test_times_out_with_the_condition_named():
    calls = []

    def never():
        calls.append(1)
        return False

    with pytest.raises(TimeoutError, match="the menu did not hold within 0.05 s"):
        wait_until(never, timeout=0.05, interval=0.01, what="the menu")

    assert len(calls) > 1


def test_wait_for_returns_the_value_the_condition_accepts():
    answers = iter([{"frame": 1}, {"frame": 2}, {"frame": 3}])

    assert wait_for(lambda: next(answers), lambda s: s["frame"] > 1, timeout=5.0, interval=0.0) == {"frame": 2}


def test_wait_for_reads_once_with_a_zero_timeout():
    assert wait_for(lambda: 7, lambda n: n == 7, timeout=0.0) == 7


def test_wait_for_times_out_with_the_last_value():
    reads = iter(range(1000))

    with pytest.raises(TimeoutError, match=r"the count did not hold within 0.05 s: \d+$") as error:
        wait_for(lambda: next(reads), lambda n: n < 0, timeout=0.05, interval=0.01, what="the count")

    assert int(str(error.value).rsplit(": ", 1)[1]) > 0


def test_wait_for_describes_the_last_value():
    with pytest.raises(TimeoutError, match=r"the light did not hold within 0 s: on=False$"):
        wait_for(
            lambda: {"on": False},
            lambda s: s["on"],
            timeout=0,
            what="the light",
            describe=lambda s: f"on={s['on']}",
        )


def test_wait_for_cuts_a_long_repr():
    with pytest.raises(TimeoutError) as error:
        wait_for(lambda: "x" * 2000, lambda s: False, timeout=0)

    shown = str(error.value).split(": ", 1)[1]
    assert shown == repr("x" * 2000)[:500] + "..."


def test_wait_for_falls_back_to_the_repr_when_describe_fails():
    with pytest.raises(TimeoutError, match=r": \{'on': False\} \(describe failed: KeyError 'light'\)$"):
        wait_for(lambda: {"on": False}, lambda s: s["on"], timeout=0, describe=lambda s: s["light"])


def test_wait_for_lets_a_read_error_through():
    def read():
        raise ConnectionError("the game went away")

    with pytest.raises(ConnectionError, match="went away"):
        wait_for(read, lambda s: True, timeout=5.0)
