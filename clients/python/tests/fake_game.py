"""A fake game for launch's tests, run as its own process: after a delay, a FakeDevbench that answers as the game and
writes its port to runtime.json under %LOCALAPPDATA%/devbench, as devbench does.

python fake_game.py PIDFILE [--delay S] [--loading S] [--game G] [--vr 0|1] [--health-vr 0|1] [--port N] [--handoff]

The serving process appends its pid to PIDFILE so the test can kill it; its quit tool ends it. --loading keeps
LoadingMenu up for S seconds after the port opens, then MainMenu. --health-vr makes health say another vr than the
runtime.json folder's. --handoff starts the same fake again detached, without --handoff, and exits at once, as a
launcher handing off does.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fake_devbench import FakeDevbench  # noqa: E402 - found through the path set above

# the longest a fake game lives, should its test not kill it
_LIFE = 60.0


def _folder(game: str, vr: bool) -> str:
    if game == "skyrim":
        return "vr" if vr else "se"
    return game + ("vr" if vr else "")


def _hand_off() -> None:
    args = [arg for arg in sys.argv[1:] if arg != "--handoff"]
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
    subprocess.Popen(
        [sys.executable, __file__, *args],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=flags,
        start_new_session=sys.platform != "win32",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("pidfile")
    parser.add_argument("--delay", type=float, default=0.0)
    parser.add_argument("--loading", type=float, default=0.0)
    parser.add_argument("--game", default="fallout4")
    parser.add_argument("--vr", type=int, default=1)
    parser.add_argument("--health-vr", type=int)
    parser.add_argument("--port", type=int)
    parser.add_argument("--handoff", action="store_true")
    options = parser.parse_args()
    if options.handoff:
        _hand_off()
        return

    with open(options.pidfile, "a", encoding="utf-8") as pids:
        pids.write(f"{os.getpid()}\n")
    time.sleep(options.delay)

    fake = FakeDevbench(options.port or 0)
    vr = bool(options.vr)
    health_vr = vr if options.health_vr is None else bool(options.health_vr)
    fake.health = {"ok": True, "pid": os.getpid(), "port": fake.port, "game": options.game, "vr": health_vr}
    started = time.monotonic()

    def menu(args):
        loading = time.monotonic() - started < options.loading
        return {"ok": True, "openMenus": ["LoadingMenu"] if loading else ["MainMenu"], "messageBoxOpen": False}

    def quit_tool(args):
        threading.Timer(0.1, lambda: os._exit(0)).start()
        return {"ok": True, "pid": os.getpid(), "quitting": True, "killAfterMs": args.get("killAfterMs", 0)}

    fake.tools["quit"] = quit_tool
    fake.tools["ping"] = lambda args: {"ok": True, "game": options.game, "vr": health_vr}
    if options.game == "fallout4":
        fake.tools["menu"] = menu
    runtime = Path(os.environ["LOCALAPPDATA"]) / "devbench" / _folder(options.game, vr) / "runtime.json"
    runtime.parent.mkdir(parents=True, exist_ok=True)
    runtime.write_text(json.dumps({"port": fake.port}), encoding="utf-8")
    time.sleep(_LIFE)


if __name__ == "__main__":
    main()
