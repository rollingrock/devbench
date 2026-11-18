"""The game's process: launch it with the user's command, quit it and wait until it is gone, restart it."""

from __future__ import annotations

import ctypes
import http.client
import os
import shlex
import socket
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from typing import Any

from devbench.client import HOST, Devbench, DevbenchError, find_port
from devbench.console import console
from devbench.games import fallout4
from devbench.menus import open_menus
from devbench.waits import poll_until, wait_until

# connecting to a port: a listener on the loopback accepts at once, while Windows retries a refused connect for
# about 2 s before failing it, so a connect that has not succeeded by then means nothing listens
_CONNECT_TIMEOUT = 1.0

# one health reply; /api/health is answered off the main thread, so a live listener replies well within it
_PROBE_TIMEOUT = 2.0

# how often wait_gone looks again
_INTERVAL = 0.25

# how often launch looks again
_LAUNCH_INTERVAL = 1.0

# the longest one call may take while launch waits: a fresh game can take seconds to answer a new connection
_LAUNCH_CALL_TIMEOUT = 30.0

# the per-call timeout of the client launch returns, Devbench's default
_CLIENT_TIMEOUT = 60.0

# when a launched game is ready, from its open menus, by game; a game not listed is ready once devbench answers
_READY: dict[str, Callable[[Sequence[str]], bool]] = {
    "fallout4": fallout4.at_main_menu_or_in_game,
}


def _accepts(port: int) -> bool:
    """
    Whether something accepts a connection on the port now, whatever it then does with it.
    """
    try:
        socket.create_connection((HOST, port), timeout=_CONNECT_TIMEOUT).close()
    except OSError:
        return False
    return True


def _probe(port: int) -> tuple[bool, Any]:
    """
    Whether something still holds the port, and what it said: the health answer, or why there was none. A connect
    that fails or a connection dropped unanswered counts as gone; one that connects and gets no reply is still there.
    """
    try:
        socket.create_connection((HOST, port), timeout=_CONNECT_TIMEOUT).close()
    except OSError as error:
        return False, f"no connection: {type(error).__name__}: {error}"
    try:
        with Devbench(port, timeout=_PROBE_TIMEOUT) as bench:
            return True, bench.health()
    except TimeoutError:
        return True, f"no reply within {_PROBE_TIMEOUT} s"
    except DevbenchError as error:
        return True, str(error)
    except (OSError, http.client.HTTPException) as error:
        return False, f"{type(error).__name__}: {error}"


class _Process:
    """
    A process watched by pid. On Windows it holds a handle opened up front, so a pid reused after the exit is not
    taken for the game; elsewhere it asks os.kill(pid, 0) each time.
    """

    _SYNCHRONIZE = 0x00100000
    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    _ERROR_ACCESS_DENIED = 5
    _WAIT_TIMEOUT = 0x102

    def __init__(self, pid: int) -> None:
        self.pid = pid
        self._handle: int | None = None
        self._gone = False
        if sys.platform == "win32":
            from ctypes import wintypes

            self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            self._kernel32.OpenProcess.restype = wintypes.HANDLE
            self._kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            self._kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            self._kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
            access = self._SYNCHRONIZE | self._PROCESS_QUERY_LIMITED_INFORMATION
            self._handle = self._kernel32.OpenProcess(access, False, pid)
            # no handle: no such process, unless it exists and refused us (then it is treated as alive throughout)
            self._gone = not self._handle and ctypes.get_last_error() != self._ERROR_ACCESS_DENIED

    def alive(self) -> bool:
        """Whether the process is still running."""
        if self._gone:
            return False
        if sys.platform == "win32":
            if not self._handle:
                return True
            return self._kernel32.WaitForSingleObject(self._handle, 0) == self._WAIT_TIMEOUT
        try:
            os.kill(self.pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def close(self) -> None:
        if self._handle:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


def wait_gone(port: int, pid: int | None = None, timeout: float = 60.0) -> None:
    """
    Wait until the game is gone: its process exited when pid is known (read from health when omitted and the port
    answers), else its port stopped answering. A game still there after timeout seconds raises DevbenchError with the
    pid, whether that process runs, and what the port last said. Nothing is killed.
    """
    if pid is None:
        answers, said = _probe(port)
        if not answers:
            return
        pid = said.get("pid") if isinstance(said, dict) else None

    if pid is None:
        last: list[Any] = [None]

        def port_gone() -> bool:
            answers, last[0] = _probe(port)
            return not answers

        try:
            wait_until(port_gone, timeout, _INTERVAL, "the port to go")
        except TimeoutError:
            raise DevbenchError(
                f"the game did not quit within {timeout} s: port {port} still answers: {last[0]}", answer=last[0]
            ) from None
        return

    process = _Process(pid)
    try:
        wait_until(lambda: not process.alive(), timeout, _INTERVAL, f"process {pid} to exit")
    except TimeoutError:
        answers, said = _probe(port)
        port_state = f"port {port} still answers: {said}" if answers else f"port {port} is closed ({said})"
        raise DevbenchError(
            f"the game did not quit within {timeout} s: process {pid} is still running; {port_state}", answer=said
        ) from None
    finally:
        process.close()


def quit_game(bench: Devbench, timeout: float = 60.0, kill_after: float | None = None) -> None:
    """
    Quit the game with devbench's quit tool (the console's qqq on a devbench without it) and wait until its process
    has exited (wait_gone), then close bench. kill_after seconds arms the tool's watchdog, which terminates a game
    whose shutdown hangs; qqq has none. A game that does not answer returns at once; a connection dropped on the
    call counts as the game going.
    """
    try:
        answers, said = _probe(bench.port)
        if not answers:
            return
        pid = said.get("pid") if isinstance(said, dict) else None
        try:
            if "quit" in bench.tools():
                args = {"killAfterMs": round(kill_after * 1000)} if kill_after else {}
                pid = bench.tool("quit", **args).get("pid", pid)
            else:
                console(bench, "qqq")
        except (OSError, http.client.HTTPException):
            pass  # the game went before it replied
        wait_gone(bench.port, pid, timeout)
    finally:
        bench.close()


# the launchers launch started, kept so a Popen is not collected (and does not warn) while its process still runs
_LAUNCHERS: list[subprocess.Popen[bytes]] = []


def _name(game: Any, vr: Any) -> str:
    return f"{game} VR" if vr else str(game)


def _start(command: str | Sequence[str], cwd: str | os.PathLike[str] | None) -> subprocess.Popen[bytes]:
    """
    Start the command detached: its own process group without a console, std streams on DEVNULL, so stopping the
    caller leaves it running. On Windows a string goes to CreateProcess as is; elsewhere it is split as a shell would.
    A command that cannot start raises DevbenchError.
    """
    args: str | list[str] = command if isinstance(command, str) else list(command)
    options: dict[str, Any] = {}
    if sys.platform == "win32":
        options["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        options["start_new_session"] = True
        if isinstance(args, str):
            args = shlex.split(args)
    try:
        return subprocess.Popen(
            args,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            **options,
        )
    except OSError as error:
        raise DevbenchError(f"the launch command could not start: {error}") from error


class _LaunchWait:
    """
    What launch watches while the game starts: one kept-alive client on the game's port (pinned, else the one
    runtime.json names now), and what was last seen there, for the timeout message.
    """

    def __init__(self, game: str, vr: bool, port: int | None) -> None:
        self.game = game
        self.vr = bool(vr)
        self.port = port
        self.bench: Devbench | None = None
        self.seen = "nothing listens on the game's port yet"
        self._ready = _READY.get(game)

    def poll(self, deadline: float) -> Devbench | None:
        """
        One look: the client, handed over, once the game answers as game / vr and is ready; else None with seen
        updated. A call may take the time left to deadline, at most _LAUNCH_CALL_TIMEOUT.
        """
        port = self.port or find_port(self.game, self.vr)
        if self.bench is not None and self.bench.port != port:
            self.close()
        if self.bench is None:
            try:
                socket.create_connection((HOST, port), timeout=_CONNECT_TIMEOUT).close()
            except OSError:
                self.seen = f"nothing listens on port {port}"
                return None
            self.bench = Devbench(port)
        bench = self.bench
        bench.timeout = max(_CONNECT_TIMEOUT, min(_LAUNCH_CALL_TIMEOUT, deadline - time.monotonic()))
        try:
            health = bench.health()
            if health.get("game") != self.game or bool(health.get("vr")) != self.vr:
                found = _name(health.get("game"), health.get("vr"))
                self.seen = f"port {port} answers as {found}, not {_name(self.game, self.vr)}"
                return None
            if self._ready is not None:
                menus = open_menus(bench)
                if not self._ready(menus):
                    self.seen = f"devbench answers on port {port}, but the game is not ready (open menus: {menus})"
                    return None
        except TimeoutError:
            self.seen = f"port {port} accepts connections, but gave no answer within {bench.timeout:.0f} s"
            self.close()
            return None
        except DevbenchError as error:
            self.seen = f"devbench answers on port {port}: {error}"
            return None
        except (OSError, http.client.HTTPException) as error:
            self.seen = f"port {port} dropped the connection: {type(error).__name__}: {error}"
            self.close()
            return None
        self.bench = None
        bench.timeout = _CLIENT_TIMEOUT
        return bench

    def close(self) -> None:
        if self.bench is not None:
            self.bench.close()
            self.bench = None


def launch(
    command: str | Sequence[str],
    game: str,
    vr: bool,
    cwd: str | os.PathLike[str] | None = None,
    timeout: float = 300.0,
    port: int | None = None,
) -> Devbench:
    """
    Run the user's command detached (the game, or a launcher such as MO2's run that may hand off and exit) and wait up
    to timeout seconds for devbench to answer as game / vr and the game to be ready (Fallout 4: main menu or a loaded
    game); returns a client of it. The port is port, else the one runtime.json names on each look. A port that already
    accepts a connection launches nothing; it, and a timeout (what was last seen, the launcher's fate), raise
    DevbenchError.
    """
    held = port or find_port(game, vr)
    # a plain connect, not _probe: a game that accepts and drops the health call still holds the port
    if _accepts(held):
        _, said = _probe(held)
        raise DevbenchError(
            f"a game may already run: port {held} accepts connections ({said}): quit it, or use it", answer=said
        )
    launcher = _start(command, cwd)
    _LAUNCHERS.append(launcher)
    waiting = _LaunchWait(game, vr, port)
    deadline = time.monotonic() + timeout

    def timed_out(_: object) -> DevbenchError:
        code = launcher.poll()
        fate = "the launcher still runs" if code is None else f"the launcher exited with code {code}"
        return DevbenchError(f"the game was not up within {timeout} s: {waiting.seen}; {fate}")

    try:
        bench = poll_until(
            lambda: waiting.poll(deadline), lambda bench: bench is not None, deadline, _LAUNCH_INTERVAL, timed_out
        )
    finally:
        waiting.close()
    assert bench is not None
    return bench


def restart(
    bench: Devbench,
    command: str | Sequence[str],
    game: str,
    vr: bool,
    cwd: str | os.PathLike[str] | None = None,
    timeout: float = 300.0,
    kill_after: float | None = None,
) -> Devbench:
    """
    Quit the game (quit_game, kill_after arming its watchdog) and launch it again with command; returns the new
    client. game / vr are given, not read, so a crashed game whose devbench says nothing restarts too.
    """
    quit_game(bench, kill_after=kill_after)
    return launch(command, game, vr, cwd=cwd, timeout=timeout)
