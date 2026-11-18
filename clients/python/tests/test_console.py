"""Console: the lines a command printed, the number after its '>>', and the refusals."""

import pytest

from devbench import DevbenchError, console, console_number


def _console_answering(fake, lines, **extra):
    """Script the fake's console tool to answer every command with lines and the extra fields."""
    fake.tools["console"] = lambda args: {
        "ok": True,
        "executed": True,
        "command": args["command"],
        "compiled": True,
        "captured": True,
        "lines": lines,
        **extra,
    }


def test_console_returns_the_lines(fake, bench):
    _console_answering(fake, ["GetInCell >> 1.00"])

    assert console(bench, "player.GetInCell QASmoke") == ["GetInCell >> 1.00"]
    assert fake.calls[-1] == ("POST", "/api/tool/console", {"command": "player.GetInCell QASmoke"})


def test_console_not_compiled_raises(fake, bench):
    _console_answering(fake, ['Script command "nope" not found.'], compiled=False)

    with pytest.raises(DevbenchError, match="did not compile") as error:
        console(bench, "nope")
    assert error.value.tool == "console"


def test_console_not_captured_returns_no_lines(fake, bench):
    fake.tools["console"] = lambda args: {"ok": True, "compiled": True, "captured": False, "note": "unreadable"}

    assert console(bench, "coc qasmoke") == []


def test_console_number_parses_the_last_number(fake, bench):
    _console_answering(fake, ["player.GetInCell QASmoke", "GetInCell >> 0.00", "GetInCell >> 1.00"])

    assert console_number(bench, "player.GetInCell QASmoke") == pytest.approx(1.0)


def test_console_number_parses_a_negative_decimal(fake, bench):
    _console_answering(fake, ["GetGlobalValue >> -12.50"])

    assert console_number(bench, "GetGlobalValue x") == pytest.approx(-12.5)


def test_console_number_without_a_number_raises(fake, bench):
    _console_answering(fake, ["Item not found"])

    with pytest.raises(DevbenchError, match="printed no number") as error:
        console_number(bench, "GetGlobalValue missing")
    assert error.value.answer["lines"] == ["Item not found"]


def test_console_number_not_captured_says_so(fake, bench):
    fake.tools["console"] = lambda args: {"ok": True, "compiled": True, "captured": False, "note": "unreadable"}

    with pytest.raises(DevbenchError, match="not captured: unreadable"):
        console_number(bench, "player.GetInCell QASmoke")
