"""Log mark: the lines after the mark, a restarted log, more than one call returns, a missing file, errors and
warnings."""

import re

import pytest

from devbench import DevbenchError, LogMark


class _Log:
    """A scripted log tool: answers tail like devbench's, from lines the test appends to or replaces."""

    def __init__(self, lines=None):
        self.lines = list(lines or [])

    def __call__(self, args):
        if args.get("file") != "Mod.log":
            return 404, {"error": f"log: cannot open {args.get('file')}"}
        kept = self.lines[-args.get("tail", 80) :]
        return {"file": "C:/logs/Mod.log", "linesScanned": len(self.lines), "returned": len(kept), "lines": kept}


@pytest.fixture
def log(fake):
    script = _Log(["old 1", "old 2"])
    fake.tools["log"] = script
    return script


def test_mark_records_the_line_count(fake, bench, log):
    mark = LogMark(bench, "Mod.log")

    assert mark.start == 2
    assert fake.calls[-1] == ("POST", "/api/tool/log", {"action": "tail", "file": "Mod.log", "tail": 5000})


def test_lines_returns_only_what_came_after_the_mark(bench, log):
    mark = LogMark(bench, "Mod.log")
    log.lines += ["new 1", "new 2"]

    assert mark.lines() == ["new 1", "new 2"]
    assert mark.missed == 0


def test_lines_is_empty_when_nothing_was_written(bench, log):
    mark = LogMark(bench, "Mod.log")

    assert mark.lines() == []


def test_a_restarted_log_counts_whole(bench, log):
    mark = LogMark(bench, "Mod.log")
    log.lines = ["fresh"]

    assert mark.lines() == ["fresh"]


def test_more_than_one_call_returns_sets_missed(bench, log):
    mark = LogMark(bench, "Mod.log")
    log.lines += [f"new {i}" for i in range(5003)]

    lines = mark.lines()

    assert len(lines) == 5000
    assert lines[0] == "new 3"
    assert mark.missed == 3


def test_a_missing_file_raises_at_the_mark(bench, log):
    with pytest.raises(DevbenchError) as error:
        LogMark(bench, "Typo.log")
    assert error.value.status == 404


def test_a_file_gone_after_the_mark_raises(fake, bench, log):
    mark = LogMark(bench, "Mod.log")
    fake.tools["log"] = lambda args: (404, {"error": "log: cannot open Mod.log"})

    with pytest.raises(DevbenchError):
        mark.lines()


def test_errors_matches_both_log_formats(bench, log):
    log.lines.append("09:00:00.000 error before the mark")
    mark = LogMark(bench, "Mod.log")
    log.lines += [
        "09:00:01.000 info fine",
        "09:00:02.000 error broke",
        "09:00:03.000 critical worse",
        "09:00:04.000 warning meh",
        "[09:00:05.000] [E] devbench broke",
        "[09:00:06.000] [C] devbench worse",
        "[09:00:07.000] [W] devbench meh",
        "09:00:08.000 info an error in the text",
    ]

    assert mark.errors() == [
        "09:00:02.000 error broke",
        "09:00:03.000 critical worse",
        "[09:00:05.000] [E] devbench broke",
        "[09:00:06.000] [C] devbench worse",
    ]


def test_warnings_matches_both_log_formats(bench, log):
    log.lines.append("09:00:00.000 warning before the mark")
    mark = LogMark(bench, "Mod.log")
    log.lines += [
        "09:00:01.000 info fine",
        "09:00:02.000 warning meh",
        "09:00:03.000 warn short",
        "09:00:04.000 error broke",
        "[09:00:05.000] [W] devbench meh",
        "[09:00:06.000] [E] devbench broke",
        "09:00:07.000 info a warning in the text",
    ]

    assert mark.warnings() == [
        "09:00:02.000 warning meh",
        "09:00:03.000 warn short",
        "[09:00:05.000] [W] devbench meh",
    ]
    assert mark.warnings(pattern="broke") == ["09:00:04.000 error broke", "[09:00:06.000] [E] devbench broke"]


def test_errors_takes_a_pattern(bench, log):
    mark = LogMark(bench, "Mod.log")
    log.lines += ["09:00:01.000 warning meh", "09:00:02.000 info fine"]

    assert mark.errors(pattern=r"^\S+ warn(ing)? ") == ["09:00:01.000 warning meh"]
    assert mark.errors(pattern=re.compile("fine")) == ["09:00:02.000 info fine"]
