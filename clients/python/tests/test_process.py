"""The game's process: quitting it (the quit tool or qqq, then waiting for the process or the port to go), and
launching it with a fake game run as its own process."""

import subprocess
import sys
import threading

import pytest
from conftest import free_port
from fake_devbench import HANG_UP

from devbench import DevbenchError, launch, open_menus, quit_game, restart, wait_gone

# the console tool's answer to a command that leaves the world: queued for a later frame, never captured
_QUEUED = {"ok": True, "executed": True, "captured": False, "queued": True}


def _later(seconds, action):
    """Run action on a timer thread after seconds."""
    timer = threading.Timer(seconds, action)
    timer.daemon = True
    timer.start()
    return timer


@pytest.fixture
def child():
    """A real process that runs until killed, standing in for the game's."""
    process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    yield process
    process.kill()
    process.wait()


def test_quit_game_returns_once_the_game_stops(fake, bench):
    def qqq(args):
        _later(0.1, fake.stop)
        return {**_QUEUED, "command": args["command"]}

    fake.tools["console"] = qqq

    quit_game(bench, timeout=5)

    assert ("POST", "/api/tool/console", {"command": "qqq"}) in fake.calls


def test_quit_game_uses_the_quit_tool_and_its_pid(fake, bench, child):
    fake.health = {"ok": True, "pid": 1}
    fake.tools["console"] = lambda args: _QUEUED

    def quit_tool(args):
        _later(0.1, child.kill)
        return {"ok": True, "pid": child.pid, "quitting": True, "killAfterMs": args.get("killAfterMs", 0)}

    fake.tools["quit"] = quit_tool

    quit_game(bench, timeout=5, kill_after=30)

    assert ("POST", "/api/tool/quit", {"killAfterMs": 30000}) in fake.calls
    assert not any(path == "/api/tool/console" for _, path, _ in fake.calls)


def test_quit_game_arms_no_watchdog_by_default(fake, bench):
    def quit_tool(args):
        _later(0.1, fake.stop)
        return {"ok": True, "quitting": True}

    fake.tools["quit"] = quit_tool

    quit_game(bench, timeout=5)

    assert ("POST", "/api/tool/quit", {}) in fake.calls


def test_quit_game_times_out_while_the_port_answers(fake, bench):
    fake.tools["console"] = lambda args: _QUEUED

    with pytest.raises(DevbenchError, match="port .* still answers"):
        quit_game(bench, timeout=0.5)


def test_quit_game_on_a_gone_game_returns_at_once(fake, bench):
    fake.stop()

    quit_game(bench, timeout=5)

    assert not any(path == "/api/tool/console" for _, path, _ in fake.calls)


def test_quit_game_goes_on_waiting_when_qqq_drops_the_connection(fake, bench):
    def qqq(args):
        fake.stop()
        return HANG_UP

    fake.tools["console"] = qqq

    quit_game(bench, timeout=5)


def test_quit_game_waits_for_the_process_named_by_health(fake, bench, child):
    fake.health = {"ok": True, "pid": child.pid}
    fake.tools["console"] = lambda args: _QUEUED
    _later(0.3, child.kill)

    quit_game(bench, timeout=5)

    assert child.poll() is not None


def test_wait_gone_times_out_while_the_process_runs(fake, child):
    fake.stop()

    with pytest.raises(DevbenchError, match=f"process {child.pid} is still running; port {fake.port} is closed"):
        wait_gone(fake.port, child.pid, timeout=0.5)


def test_wait_gone_reads_the_pid_from_health(fake, child):
    fake.health = {"ok": True, "pid": child.pid}

    with pytest.raises(DevbenchError, match=f"process {child.pid} is still running"):
        wait_gone(fake.port, timeout=0.5)


def test_wait_gone_with_a_gone_port_and_no_pid_returns(fake):
    fake.stop()

    wait_gone(fake.port, timeout=0)


def test_launch_waits_for_devbench_and_the_main_menu(launching):
    with launch(launching.game("--delay", "0.3", "--loading", "0.5"), "fallout4", True, timeout=20) as bench:
        assert bench.health()["pid"] == launching.pids()[0]
        assert open_menus(bench) == ["MainMenu"]
        assert bench.timeout == 60.0


def test_launch_accepts_a_launcher_that_hands_off(launching):
    with launch(launching.game("--handoff", "--delay", "0.5"), "fallout4", True, timeout=20) as bench:
        assert bench.health()["game"] == "fallout4"


def test_launch_takes_a_command_line_string(launching):
    command = subprocess.list2cmdline(launching.game()) if sys.platform == "win32" else " ".join(launching.game())
    with launch(command, "fallout4", True, timeout=20) as bench:
        assert bench.health()["vr"] is True


def test_launch_waits_on_a_pinned_port(launching):
    port = free_port()
    with launch(launching.game("--port", str(port)), "fallout4", True, timeout=20, port=port) as bench:
        assert bench.port == port


def test_launch_of_a_game_without_a_ready_check_returns_once_devbench_answers(launching):
    with launch(launching.game("--game", "skyrim"), "skyrim", True, timeout=20) as bench:
        assert bench.health()["game"] == "skyrim"


def test_launch_times_out_when_nothing_listens(launching):
    command = [sys.executable, "-c", "import sys; sys.exit(3)"]

    with pytest.raises(DevbenchError, match=r"not up within 0.5 s: nothing listens on port \d+; .*exited with code 3"):
        launch(command, "fallout4", True, timeout=0.5)


def test_launch_times_out_at_a_game_that_is_not_ready(launching):
    with pytest.raises(DevbenchError, match=r"not ready \(open menus: \['LoadingMenu'\]\); the launcher still runs"):
        launch(launching.game("--loading", "60"), "fallout4", True, timeout=3)


def test_launch_ignores_another_game_on_the_port(launching):
    with pytest.raises(DevbenchError, match="answers as fallout4, not fallout4 VR"):
        launch(launching.game("--health-vr", "0"), "fallout4", True, timeout=3)


def test_launch_refuses_while_a_game_holds_the_port(launching, fake):
    launching.name_port(fake.port)
    marker = launching.folder / "launched"

    with pytest.raises(DevbenchError, match=f"a game may already run: port {fake.port}"):
        launch([sys.executable, "-c", f"open(r'{marker}', 'w')"], "fallout4", True, timeout=5)

    assert not marker.exists()


def test_launch_refuses_while_a_game_accepts_and_drops_the_connection(launching, fake):
    launching.name_port(fake.port)
    fake.health = HANG_UP
    marker = launching.folder / "launched"

    with pytest.raises(DevbenchError, match=f"a game may already run: port {fake.port}"):
        launch([sys.executable, "-c", f"open(r'{marker}', 'w')"], "fallout4", True, timeout=5)

    assert not marker.exists()


def test_launch_reports_a_command_that_cannot_start(launching):
    with pytest.raises(DevbenchError, match="could not start"):
        launch([str(launching.folder / "no-such-launcher.exe")], "fallout4", True, timeout=5)


def test_restart_quits_then_launches(launching, fake, bench):
    def quit_tool(args):
        _later(0.1, fake.stop)
        return {"ok": True, "quitting": True}

    fake.tools["quit"] = quit_tool

    with restart(bench, launching.game(), "fallout4", True, timeout=20) as again:
        assert ("POST", "/api/tool/quit", {}) in fake.calls
        assert again.health()["pid"] == launching.pids()[0]
