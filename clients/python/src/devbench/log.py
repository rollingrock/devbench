"""A mark in a plugin's log through devbench's log tool: the lines written after it, and the errors and warnings among
them."""

from __future__ import annotations

import re
from typing import Any

from devbench.client import Devbench

# the most lines one log tool call returns
_TAIL_MAX = 5000

# an error or critical line in either log format in use: the mods' "<time> error <msg>" (F4VR-CommonFramework) and
# devbench's own "[12:00:00.000] [E] <msg>"
ERROR_PATTERN = re.compile(r"^\S+ (error|critical) |^\[[^\]]*\] \[[EC]\] ")

# a warning line in the same two formats: "<time> warning <msg>" (or "warn") and "[12:00:00.000] [W] <msg>"
WARNING_PATTERN = re.compile(r"^\S+ warn(ing)? |^\[[^\]]*\] \[W\] ")


class LogMark:
    """
    A line count in one log under the extender's log folder, taken when the mark is made. lines() returns what was
    written after it, and errors() and warnings() the lines among those that match a pattern. A missing file raises
    DevbenchError.
    """

    def __init__(self, bench: Devbench, file: str) -> None:
        self.bench = bench
        self.file = file
        self.start: int = self._tail()["linesScanned"]
        # the new lines the last read could not return, because the tool returns at most the last 5000
        self.missed = 0

    def _tail(self) -> dict[str, Any]:
        return self.bench.tool("log", action="tail", file=self.file, tail=_TAIL_MAX)

    def lines(self) -> list[str]:
        """
        The lines written since the mark, from one tool call so none slips between counting and reading. A log
        shorter than the mark was restarted (the game truncates it at launch), so all of it counts as new; a restart
        that already grew past the mark cannot be told apart. More than 5000 new lines returns the last 5000 and
        sets missed.
        """
        answer = self._tail()
        scanned: int = answer["linesScanned"]
        lines: list[str] = answer["lines"]
        new = scanned - self.start if scanned >= self.start else scanned
        self.missed = max(0, new - len(lines))
        return lines[len(lines) - min(new, len(lines)) :]

    def errors(self, pattern: str | re.Pattern[str] = ERROR_PATTERN) -> list[str]:
        """
        The lines since the mark that pattern finds (re.search). The default matches error and critical lines in the
        mods' log format and in devbench's own.
        """
        regex = re.compile(pattern) if isinstance(pattern, str) else pattern
        return [line for line in self.lines() if regex.search(line)]

    def warnings(self, pattern: str | re.Pattern[str] = WARNING_PATTERN) -> list[str]:
        """
        The lines since the mark that pattern finds (re.search). The default matches warning lines in the mods' log
        format and in devbench's own; errors are not among them.
        """
        return self.errors(pattern)
