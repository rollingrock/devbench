"""The game's console through devbench's console tool: run a command, read the lines or the number it printed."""

from __future__ import annotations

import re
from typing import Any

from devbench.client import Devbench, DevbenchError

# the value a console function prints after its '>>', as GetInCell and GetGlobalValue print it
_NUMBER = re.compile(r">>\s*(-?\d+(?:\.\d+)?)")


def _run(bench: Devbench, command: str) -> dict[str, Any]:
    """
    The console tool's answer to a command. One that does not compile raises DevbenchError: the engine prints its
    complaint and the tool still answers ok.
    """
    answer = bench.tool("console", command=command)
    if not answer.get("compiled", True):
        raise DevbenchError(
            f"console: {command!r} did not compile: {answer.get('lines')}", tool="console", answer=answer
        )
    return answer


def console(bench: Devbench, command: str) -> list[str]:
    """
    Run a console command and return the lines it printed; [] when devbench could not capture them (the answer's
    captured is false). A command that does not compile raises DevbenchError.
    """
    return _run(bench, command).get("lines", [])


def console_number(bench: Devbench, command: str) -> float:
    """
    The number a console command prints after its '>>' (the last one, when it prints several). Output that holds
    none, or that devbench could not capture, raises DevbenchError with the tool's answer.
    """
    answer = _run(bench, command)
    if answer.get("captured") is False:
        raise DevbenchError(
            f"console: {command!r} ran but its output was not captured: {answer.get('note')}",
            tool="console",
            answer=answer,
        )
    lines = answer.get("lines", [])
    for line in reversed(lines):
        if match := _NUMBER.search(line):
            return float(match.group(1))
    raise DevbenchError(f"console: {command!r} printed no number: {lines}", tool="console", answer=answer)
