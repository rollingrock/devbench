"""Polling: wait for a condition of the running game to hold."""

from __future__ import annotations

import time
from collections.abc import Callable
from typing import TypeVar

T = TypeVar("T")

# the most characters of the last value's repr a wait_for timeout shows
_DESCRIBE_MAX = 500


def wait_until(check: Callable[[], T], timeout: float, interval: float = 0.1, what: str = "the condition") -> T:
    """
    Call check until it returns a true value, and return that value; raise TimeoutError after timeout seconds.
    check runs at least once, also with a timeout of 0; what names the condition in the error.
    """
    return poll_until(
        check,
        bool,
        time.monotonic() + timeout,
        interval,
        lambda _: TimeoutError(f"{what} did not hold within {timeout} s"),
    )


def wait_for(
    read: Callable[[], T],
    condition: Callable[[T], object],
    timeout: float,
    interval: float = 0.1,
    what: str = "the value",
    describe: Callable[[T], object] | None = None,
) -> T:
    """
    Call read until condition accepts what it returned, and return that value; after timeout seconds raise
    TimeoutError with the last value read, through describe when given, else its repr cut to 500 characters. read runs
    at least once; an exception from read or condition propagates, one from describe falls back to the repr.
    """
    return poll_until(
        read,
        condition,
        time.monotonic() + timeout,
        interval,
        lambda value: TimeoutError(f"{what} did not hold within {timeout} s: {_describe(value, describe)}"),
    )


def poll_until(
    read: Callable[[], T],
    condition: Callable[[T], object],
    deadline: float,
    interval: float,
    timed_out: Callable[[T], BaseException],
) -> T:
    """
    The polling loop under every wait: call read until condition accepts its value and return it; once
    time.monotonic() passes deadline, raise timed_out(last value). read runs at least once, also past the deadline.
    """
    while True:
        value = read()
        if condition(value):
            return value
        if time.monotonic() > deadline:
            raise timed_out(value)
        time.sleep(interval)


def _describe(value: T, describe: Callable[[T], object] | None) -> str:
    """The last value for a timeout's message: describe(value), or its cut repr (noting a describe that raised)."""
    text = repr(value)
    if len(text) > _DESCRIBE_MAX:
        text = text[:_DESCRIBE_MAX] + "..."
    if describe is None:
        return text
    try:
        return str(describe(value))
    except Exception as error:
        return f"{text} (describe failed: {type(error).__name__} {error})"
