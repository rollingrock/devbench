"""Checks that the running game is the one a run needs: the game, its tools, a mod's actions and build.

Each failure raises RequirementError with a message that says what was found and what to do about it.
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from typing import Any

from devbench.client import Devbench, DevbenchError, ModTool

# the name a message shows for each (game, vr)
_GAME_NAMES = {
    ("skyrim", False): "Skyrim SE",
    ("skyrim", True): "Skyrim VR",
    ("fallout4", False): "Fallout 4",
    ("fallout4", True): "Fallout 4 VR",
}


# the leading dotted number of a version, which require_min_version compares
_VERSION = re.compile(r"\d+(?:\.\d+)*")


class RequirementError(Exception):
    """
    The running game is not what a run needs: the wrong game, a missing tool or action, another build of a mod.
    The message says what to do.
    """


def require(bench: Devbench, game: str, vr: bool) -> dict[str, Any]:
    """
    Check through ping that devbench answers and runs game ("skyrim" or "fallout4"; anything else raises ValueError)
    in VR or not, and return the ping answer.
    """
    key = (game, bool(vr))
    if key not in _GAME_NAMES:
        raise ValueError(f"unknown game {game!r}: expected 'skyrim' or 'fallout4'")
    try:
        ping = bench.tool("ping")
    except (OSError, DevbenchError) as error:
        raise RequirementError(
            f"devbench does not answer on port {bench.port}: {error}: start the game with devbench deployed"
        ) from error
    found_game, found_vr = ping.get("game"), bool(ping.get("vr"))
    if found_game != game or found_vr != bool(vr):
        found = f"{found_game} ({'VR' if found_vr else 'not VR'})"
        raise RequirementError(
            f"devbench on port {bench.port} is {found}, expected {_GAME_NAMES[key]}: start the right game"
        )
    return ping


def require_tools(bench: Devbench, names: Iterable[str]) -> None:
    """Check that devbench lists each of the named tools; the message names every one it lacks."""
    listed = set(bench.tools())
    missing = [name for name in names if name not in listed]
    if missing:
        raise RequirementError(f"devbench lacks tools {', '.join(missing)}: deploy the mods that register them")


def _mod_health(mod_tool: ModTool, field: str) -> dict[str, Any]:
    """
    The mod tool's health answer, which must hold field. A tool that does not answer, or an answer without field,
    raises RequirementError.
    """
    try:
        health = mod_tool.health()
    except (OSError, DevbenchError) as error:
        raise RequirementError(
            f"the mod's devbench tool {mod_tool.name} does not answer: {error}: deploy this build"
        ) from error
    if field not in health:
        raise RequirementError(
            f"the mod's devbench tool {mod_tool.name} answers health without {field}: {health}: deploy this build"
        )
    return health


def require_actions(mod_tool: ModTool, actions: Iterable[str]) -> dict[str, Any]:
    """
    Check that the mod tool's health lists each of the actions, and return the health answer. The message names
    every action it lacks.
    """
    health = _mod_health(mod_tool, "actions")
    offered = health["actions"]
    if not isinstance(offered, list):
        raise RequirementError(
            f"the mod's devbench tool {mod_tool.name} answers health with actions that are not a list: {offered!r}: "
            "deploy this build"
        )
    missing = [action for action in actions if action not in offered]
    if missing:
        raise RequirementError(f"{mod_tool.name} lacks actions {', '.join(missing)}: deploy this build")
    return health


def version_numbers(text: str) -> tuple[int, ...] | None:
    """The leading dotted number of a version as a tuple without trailing zeros ("0.79.0-beta": (0, 79)), or None."""
    found = _VERSION.match(text)
    if found is None:
        return None
    numbers = [int(part) for part in found.group(0).split(".")]
    while len(numbers) > 1 and numbers[-1] == 0:
        numbers.pop()
    return tuple(numbers)


def require_min_version(mod_tool: ModTool, minimum: str) -> dict[str, Any]:
    """
    Check that the mod tool's health reports minimum or a later version, and return the health answer. Versions
    compare by their leading dotted number ("0.79.2-beta" is 0.79.2); a running version without one raises
    RequirementError, a minimum without one ValueError.
    """
    health = _mod_health(mod_tool, "version")
    running = str(health["version"])
    wanted = version_numbers(minimum)
    if wanted is None:
        raise ValueError(f"minimum version {minimum!r} is not a dotted number")
    numbers = version_numbers(running)
    if numbers is None:
        raise RequirementError(f"{mod_tool.name} reports version {running!r}, not a dotted number: deploy a release")
    if numbers < wanted:
        raise RequirementError(
            f"the game runs {mod_tool.name} {running}, this run needs {minimum} or later: deploy a newer build"
        )
    return health


def require_version(mod_tool: ModTool, expected: str) -> dict[str, Any]:
    """
    Check that the mod tool's health reports exactly the expected version (the checkout's), and return the health
    answer.
    """
    health = _mod_health(mod_tool, "version")
    running = str(health["version"])
    if running != expected:
        raise RequirementError(
            f"the game runs {mod_tool.name} {running}, the checkout is {expected}: deploy this build"
        )
    return health
