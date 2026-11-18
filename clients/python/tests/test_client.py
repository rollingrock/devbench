"""Devbench, ModTool and DevbenchError against the fake devbench."""

import http.client
import time

import pytest

from devbench import Devbench, DevbenchError, ModTool


def test_tool_call_returns_the_answer_and_sends_the_arguments(fake, bench):
    fake.tools["console"] = lambda args: {"ok": True, "lines": [f"ran {args['command']}"]}

    answer = bench.tool("console", command="GetInCell QASmoke")

    assert answer["lines"] == ["ran GetInCell QASmoke"]
    assert fake.calls == [("POST", "/api/tool/console", {"command": "GetInCell QASmoke"})]


def test_tool_name_is_positional_only(fake, bench):
    fake.tools["nodes"] = lambda args: {"ok": True, "args": args}

    assert bench.tool("nodes", name="Weapon")["args"] == {"name": "Weapon"}


def test_refusal_raises_with_the_error(fake, bench):
    fake.tools["menu"] = lambda args: {"ok": False, "error": "no such menu"}

    with pytest.raises(DevbenchError, match="menu close: no such menu") as raised:
        bench.tool("menu", action="close")

    assert raised.value.tool == "menu"
    assert raised.value.status == 200
    assert raised.value.answer == {"ok": False, "error": "no such menu"}


def test_non_200_raises(fake, bench):
    fake.tools["inspect"] = lambda args: (500, {"error": "boom"})

    with pytest.raises(DevbenchError, match="inspect: boom") as raised:
        bench.tool("inspect")

    assert raised.value.status == 500


def test_unknown_tool_raises(bench):
    with pytest.raises(DevbenchError, match="unknown tool missing") as raised:
        bench.tool("missing")

    assert raised.value.status == 404


def test_body_that_is_not_json_raises(fake, bench):
    fake.tools["broken"] = lambda args: (502, b"<html>bad gateway</html>")

    with pytest.raises(DevbenchError, match="HTTP 502, not JSON") as raised:
        bench.tool("broken")

    assert raised.value.tool == "broken"
    assert raised.value.status == 502


def test_reconnects_once_when_the_server_dropped_the_connection(fake, bench):
    fake.tools["ping"] = lambda args: {"ok": True}

    bench.tool("ping")
    fake.drop_next()
    bench.tool("ping")
    bench.tool("ping")

    assert fake.connections == 2
    assert len(fake.calls) == 3


def test_refused_fresh_connection_is_not_retried(fake, monkeypatch):
    connects = count_connects(monkeypatch)
    port = fake.port
    fake.close()

    # refused on Linux; on Windows the OS retries the SYN for ~2 s, so the short timeout fires first
    with Devbench(port, timeout=0.5) as gone, pytest.raises((ConnectionRefusedError, TimeoutError)):
        gone.tool("ping")

    assert connects == [1]


def test_timeout_is_not_retried(fake):
    def slow(args):
        time.sleep(0.5)
        return {"ok": True}

    fake.tools["slow"] = slow

    with Devbench(fake.port, timeout=0.1) as impatient, pytest.raises(TimeoutError):
        impatient.tool("slow")

    assert fake.calls == [("POST", "/api/tool/slow", {})]
    assert fake.connections == 1


def count_connects(monkeypatch):
    """Count the TCP connects every HTTPConnection makes, as a one-element list."""
    connects = [0]
    connect = http.client.HTTPConnection.connect

    def counting(self):
        connects[0] += 1
        connect(self)

    monkeypatch.setattr(http.client.HTTPConnection, "connect", counting)
    return connects


def test_health(fake, bench):
    fake.health = {"ok": True, "game": "fallout4", "vr": True}

    assert bench.health() == {"ok": True, "game": "fallout4", "vr": True}
    assert fake.calls == [("GET", "/api/health", None)]


def test_tools_lists_the_names(fake, bench):
    fake.tools["ping"] = lambda args: {"ok": True}
    fake.tools["menu"] = lambda args: {"ok": True}

    assert bench.tools() == ["menu", "ping"]
    assert fake.calls == [("GET", "/api/tools", None)]


def test_health_non_200_raises(fake, bench):
    fake.health = (503, {"ok": False})

    with pytest.raises(DevbenchError, match="health: HTTP 503") as raised:
        bench.health()

    assert raised.value.status == 503


def test_close_drops_the_connection_and_the_next_call_reopens(fake, bench):
    fake.tools["ping"] = lambda args: {"ok": True}

    bench.tool("ping")
    bench.close()
    bench.tool("ping")

    assert fake.connections == 2


def test_mod_tool_calls_by_action(fake, bench):
    fake.tools["immersiveflashlightvr"] = lambda args: {"ok": True, "args": args}
    mod = ModTool(bench, "immersiveflashlightvr")

    assert mod.call("light", on=True)["args"] == {"action": "light", "on": True}
    assert mod.prefix == "immersiveflashlightvr"


def test_mod_tool_health(fake, bench):
    fake.tools["modsconfigfo4vr"] = lambda args: {"ok": True, "version": "1.2.0", "actions": ["health"], "args": args}

    health = ModTool(bench, "modsconfigfo4vr").health()

    assert health["version"] == "1.2.0"
    assert health["args"] == {"action": "health"}
