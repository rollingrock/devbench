"""The events devbench publishes: start an Events before an action, then read or wait for what the action caused."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from devbench.client import Devbench, DevbenchError
from devbench.waits import wait_until


class Events:
    """
    The events devbench publishes from the moment this is made. A topic is named bare; with a prefix (a mod's tool
    name, ModTool.prefix) it also matches as <prefix>.<topic>. A read that finds events lost to the ring (more than
    it holds between two reads) or a restarted game (its seq counting again from 1) raises DevbenchError.
    """

    def __init__(self, bench: Devbench, prefix: str | None = None) -> None:
        self.bench = bench
        self.prefix = prefix
        self.start = bench.head_seq()
        self._seen: list[dict[str, Any]] = []

    def _last_seq(self) -> int:
        return self._seen[-1]["seq"] if self._seen else self.start

    def _read(self) -> list[dict[str, Any]]:
        """
        Add the events published since the last read to the ones seen and return them all. Seqs have no holes, so a
        first new seq past last + 1 means the ring dropped events, and a head below last means the game restarted.
        """
        last = self._last_seq()
        head, events = self.bench._events(last)
        if head < last:
            raise DevbenchError(f"events: devbench's seq went back from {last} to {head}; the game restarted")
        if events and events[0]["seq"] > last + 1:
            lost = events[0]["seq"] - last - 1
            raise DevbenchError(f"events: {lost} event(s) after seq {last} left the ring before they were read")
        self._seen.extend(events)
        return self._seen

    def _matches(self, topic: str | None, event: dict[str, Any]) -> bool:
        if topic is None:
            return True
        return event["topic"] == topic or (self.prefix is not None and event["topic"] == f"{self.prefix}.{topic}")

    def all(self, topic: str | None = None) -> list[dict[str, Any]]:
        """The events since the start, oldest first: those of one topic, or all of them when topic is None."""
        return [event for event in self._read() if self._matches(topic, event)]

    def wait(self, topic: str, timeout: float = 5.0, where: Callable[[Any], Any] | None = None) -> dict[str, Any]:
        """
        The first event of a topic since the start whose data where accepts (any, without where), polling up to
        timeout seconds. The TimeoutError names the topic, the prefix and the topics that did arrive.
        """

        def found() -> dict[str, Any] | None:
            return next(
                (event for event in self.all(topic) if where is None or where(event.get("data"))),
                None,
            )

        try:
            return wait_until(found, timeout, what=f"the event {topic}")
        except TimeoutError:
            prefix = f" (prefix {self.prefix})" if self.prefix else ""
            topics = list(dict.fromkeys(event["topic"] for event in self._seen))
            raise TimeoutError(
                f"no event {topic!r}{prefix} matched within {timeout} s; topics seen: {topics}"
            ) from None
