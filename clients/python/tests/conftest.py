"""Fixtures shared by the client's unit tests."""

import json
import os
import signal
import socket
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest
from fake_devbench import FakeDevbench

from devbench import Devbench
from devbench import process as process_module

# the fake game the launch tests run as its own process
_FAKE_GAME = str(Path(__file__).resolve().parent / "fake_game.py")

# the plugin's tests run inner pytest sessions
pytest_plugins = ["pytester"]


@pytest.fixture
def fake() -> Iterator[FakeDevbench]:
    """A fake devbench on a free port, stopped after the test."""
    server = FakeDevbench()
    yield server
    server.close()


@pytest.fixture
def bench(fake: FakeDevbench) -> Iterator[Devbench]:
    """A client connected to the fake devbench."""
    with Devbench(fake.port, timeout=5.0) as client:
        yield client


def free_port():
    """A port nothing listens on now."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class _Launching:
    """
    The place launch's tests run in: LOCALAPPDATA in a temporary folder, the runtime.json of each game naming a port
    nothing listens on, and a pid file each fake game appends to; the fake games are killed after the test.
    """

    def __init__(self, folder):
        self.folder = folder
        self.pidfile = folder / "pids.txt"

    def runtime(self, folder="fallout4vr"):
        return self.folder / "devbench" / folder / "runtime.json"

    def name_port(self, port, folder="fallout4vr"):
        path = self.runtime(folder)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"port": port}), encoding="utf-8")

    def game(self, *args):
        """The command of a fake game with the given options."""
        return [sys.executable, _FAKE_GAME, str(self.pidfile), *args]

    def pids(self):
        if not self.pidfile.exists():
            return []
        return [int(line) for line in self.pidfile.read_text(encoding="utf-8").split()]


@pytest.fixture
def launching(tmp_path, monkeypatch):
    """Where launch's tests run (_Launching), with launch polling fast; the fake games it starts are killed after."""
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    monkeypatch.setattr(process_module, "_LAUNCH_INTERVAL", 0.05)
    # a connect to a port nothing listens on waits out this timeout on Windows; the fakes accept at once
    monkeypatch.setattr(process_module, "_CONNECT_TIMEOUT", 0.2)
    monkeypatch.setattr("devbench.testing._plugin._CONNECT_TIMEOUT", 0.2)
    place = _Launching(tmp_path)
    for folder in ("fallout4vr", "fallout4", "vr"):
        place.name_port(free_port(), folder)
    yield place
    for pid in place.pids():
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
