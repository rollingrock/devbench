"""Fallout 4 (and Fallout 4 VR) game setup and player helpers.

Setup: a game in a test cell, started from the main menu or reused, then settled. Player: items through the console,
the weapon drawn or sheathed and power armor entered or left through devbench's player tool; a switch whose outcome is
neither switched nor already raises DevbenchError with the tool's answer.

A setup cell is named by the editor ID of an interior cell: GetInCell takes no other, and one it does not know does not
compile (DevbenchError). Nothing here prints: setup returns what it did and the dialogs it closed.
"""

from __future__ import annotations

import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import Any

from devbench.checks import RequirementError
from devbench.client import Devbench, DevbenchError
from devbench.console import console, console_number
from devbench.menus import close_dialogs, open_menus
from devbench.waits import wait_until

# the wait after each answered message box, as close_dialogs takes it, when the poll interval is longer
_DIALOG_PAUSE = 0.5

# enter_game's cells for a game that runs in whatever cell is loaded
ANY_CELL = "any"


@dataclass(frozen=True)
class Settled:
    """
    What settle did: the dialogs it closed (what each said), and whether the game went quiet (False: it was still
    popping up dialogs at the limit).
    """

    dialogs: list[str]
    quiet: bool


@dataclass(frozen=True)
class Entered:
    """
    What enter_game did: started is False when it reused a game already loaded; dialogs holds every dialog it
    closed; quiet is the settle's outcome after a start, None on a reuse (nothing was settled). cell is the cell the
    game is in: the start cell after a start, the accepted cell it matched on a reuse, None on a reuse under ANY_CELL
    (the cell was not looked at).
    """

    started: bool
    dialogs: list[str]
    quiet: bool | None
    cell: str | None = None


def at_main_menu_or_in_game(menus: Sequence[str]) -> bool:
    """
    Whether a launched game is ready, from its open menus: the main menu up, or a game loaded (the HUD up) for a setup
    that loads a save at start. Never while the loading screen shows.
    """
    return "LoadingMenu" not in menus and ("MainMenu" in menus or "HUDMenu" in menus)


def _player_in(bench: Devbench, cell: str) -> bool:
    """Whether the console's GetInCell says the player is in the cell (by its editor ID)."""
    return int(console_number(bench, f"player.GetInCell {cell}")) == 1


def _as_cells(cells: Sequence[str] | str | None) -> Sequence[str]:
    """
    The accepted cells as a list: None is none, and a bare cell name is one cell, not its letters. ANY_CELL is
    handled before, so a string that reaches here is a cell's editor ID.
    """
    if cells is None:
        return []
    return [cells] if isinstance(cells, str) else cells


def in_cell(bench: Devbench, cell: str) -> bool:
    """
    Whether a game is loaded with the player in the cell (by its editor ID). False at the main menu, where the player
    exists too and GetInCell reads whatever it last held.
    """
    if "MainMenu" in open_menus(bench):
        return False
    return _player_in(bench, cell)


def settle(bench: Devbench, quiet: float = 10, limit: float = 120, interval: float = 1.0) -> Settled:
    """
    Wait until no message box has popped up for quiet seconds, answering each one that does, as a new game's mods
    pop them up while their quests start. Gives up after limit seconds with Settled.quiet False; that is no error.
    """
    pause = min(_DIALOG_PAUSE, interval)
    dialogs: list[str] = []
    start = time.monotonic()
    quiet_since = start
    while True:
        closed = close_dialogs(bench, pause=pause)
        now = time.monotonic()
        if closed:
            dialogs.extend(closed)
            quiet_since = now
        elif now - quiet_since >= quiet:
            return Settled(dialogs, True)
        if now - start >= limit:
            return Settled(dialogs, False)
        time.sleep(interval)


def enter_game(
    bench: Devbench,
    cell: str = "QASmoke",
    timeout: float = 300,
    quiet: float = 10,
    limit: float = 120,
    interval: float = 1.0,
    cells: Sequence[str] | str | None = None,
) -> Entered:
    """
    A game in the cell, so nothing a test does reaches a save. From the main menu: coc to it, wait up to timeout
    seconds for the HUD with no MainMenu or LoadingMenu, then settle (quiet, limit). A game already loaded is reused
    when the player is in the cell or in one of cells (None: the cell only); cells=ANY_CELL reuses it whatever its
    cell, which also lets a run reach a save. Another loaded game, or none (mid-load), raises RequirementError.
    Dialogs are answered throughout.
    """
    pause = min(_DIALOG_PAUSE, interval)
    menus = open_menus(bench)
    # the player exists at the main menu too: the main menu is what tells it from a game
    if "MainMenu" not in menus:
        if "LoadingMenu" in menus or not bench.tool("inspect", kind="state")["playerLoaded"]:
            raise RequirementError(
                f"the game is neither at the main menu nor in a game (open menus: {menus}): "
                "wait for the load to end, or quit to the main menu"
            )
        found: str | None = None
        if cells != ANY_CELL:
            accepted = [cell, *(name for name in _as_cells(cells) if name != cell)]
            found = next((name for name in accepted if _player_in(bench, name)), None)
            if found is None:
                where = cell if len(accepted) == 1 else "any of " + ", ".join(accepted)
                raise RequirementError(
                    f"a game outside {where} is loaded: quit to the main menu, and a new game is started in {cell}"
                )
        return Entered(False, close_dialogs(bench, pause=pause), None, found)

    # a coc behind a modal does nothing, so a mod's popup at the main menu is answered first
    dialogs = close_dialogs(bench, pause=pause)
    answer = bench.tool("console", command=f"coc {cell}")
    if answer.get("blocked"):
        raise DevbenchError(
            f"console: coc {cell} was blocked by an open modal (open menus: {open_menus(bench)})",
            tool="console",
            answer=answer,
        )

    def in_game() -> bool:
        dialogs.extend(close_dialogs(bench, pause=pause))
        menus = open_menus(bench)
        return "HUDMenu" in menus and "MainMenu" not in menus and "LoadingMenu" not in menus

    # a coc is neither a new game nor a load to F4SE: the game is in once the main menu and the loading screen are
    # gone and the HUD is up
    wait_until(in_game, timeout, interval=interval, what=f"the game in {cell}")
    settled = settle(bench, quiet=quiet, limit=limit, interval=interval)
    return Entered(True, dialogs + settled.dialogs, settled.quiet, cell)


def item_count(bench: Devbench, form: str) -> int:
    """How many of the form (its form ID, as the console takes it) the player carries."""
    return int(console_number(bench, f"player.getitemcount {form}"))


def equip(bench: Devbench, form: str) -> None:
    """
    Equip the form on the player through the console, adding one first when the player has none. Returns once the
    command ran: the engine equips it a moment later.
    """
    if item_count(bench, form) == 0:
        console(bench, f"player.additem {form} 1")
    console(bench, f"player.equipitem {form}")


def unequip(bench: Devbench, form: str) -> None:
    """Unequip the form from the player through the console; the player keeps it."""
    console(bench, f"player.unequipitem {form}")


def _switch(
    bench: Devbench, action: str, state: Callable[[dict[str, Any]], str], timeout: float, **args: Any
) -> dict[str, Any]:
    """
    Run a player tool switch with the arguments that are set, waiting up to timeout seconds (the tool takes at most
    60), and return its answer when the outcome is switched or already. Any other raises DevbenchError naming the
    outcome, the state (state reads it from the answer) and the note.
    """
    args = {key: value for key, value in args.items() if value is not None}
    answer = bench.tool("player", action=action, timeoutMs=round(timeout * 1000), **args)
    outcome = answer.get("outcome")
    if outcome not in ("switched", "already"):
        note = f" ({answer['note']})" if answer.get("note") else ""
        raise DevbenchError(f"player {action}: {outcome}, {state(answer)}{note}", tool="player", answer=answer)
    return answer


def _weapon_state(answer: dict[str, Any]) -> str:
    """The weapon's state in a player answer, for a failure message."""
    return f"the weapon is {answer.get('weaponState')}"


def draw_weapon(bench: Devbench, timeout: float = 10) -> dict[str, Any]:
    """
    Draw what the player has equipped (fists when nothing) with the engine's own call, and wait up to timeout seconds
    for it to settle drawn. Returns the player tool's answer; an outcome other than switched or already raises
    DevbenchError with the weapon state.
    """
    return _switch(bench, "drawWeapon", _weapon_state, timeout=timeout)


def sheathe_weapon(bench: Devbench, timeout: float = 10) -> dict[str, Any]:
    """
    Sheathe what the player has drawn, fists included, as draw_weapon does the other way. The weapon stays equipped.
    """
    return _switch(bench, "sheatheWeapon", _weapon_state, timeout=timeout)


def _armor_state(answer: dict[str, Any]) -> str:
    """Whether the player is in power armor, and a step-in's aim, in a player answer, for a failure message."""
    aim = f", aim {answer['aim']}" if "aim" in answer else ""
    return f"inPowerArmor {answer.get('inPowerArmor')}{aim}"


def nearest_power_armor(bench: Devbench, radius: float = 500) -> dict[str, Any] | None:
    """
    The nearest power armor frame in the player's cell within radius game units, as the player tool lists it
    ({ formId, baseFormId, name, distance }), or None when there is none.
    """
    frames = bench.tool("player", action="findPowerArmor", radius=radius, limit=1)["frames"]
    return frames[0] if frames else None


def enter_power_armor(
    bench: Devbench, form_id: str | None = None, radius: float | None = None, timeout: float = 10
) -> dict[str, Any]:
    """
    Put the player in a frame at once, without the step-in animation: setup, not the transition a player makes.
    form_id is the frame reference (default the nearest within radius, the tool's 500 units when None). Waits up to
    timeout seconds; an outcome other than switched or already raises DevbenchError.
    """
    return _switch(bench, "enterPowerArmor", _armor_state, formId=form_id, radius=radius, timeout=timeout)


def exit_power_armor(bench: Devbench, timeout: float = 10) -> dict[str, Any]:
    """
    Take the player out of power armor at once, as enter_power_armor puts them in. A mod such as KabutoVR can refuse
    it (DevbenchError, answer outcome refused); leave_power_armor falls back to stepping out.
    """
    return _switch(bench, "exitPowerArmor", _armor_state, timeout=timeout)


def step_into_power_armor(
    bench: Devbench, form_id: str | None = None, radius: float | None = None, timeout: float = 10
) -> dict[str, Any]:
    """
    Walk the player into a frame through the controllers (VR only): aim the primary wand at it and press A, so the
    game runs its own activation and animations. It stands the player behind the frame and holds a pose as the owner
    devbench.player, so a Hands pose held elsewhere makes it fail (409). form_id and radius as for enter_power_armor.
    A frame the ray never lands on (notAimed) raises DevbenchError like any other outcome, with the aim in the message.
    """
    return _switch(bench, "stepIn", _armor_state, formId=form_id, radius=radius, timeout=timeout)


def step_out_of_power_armor(bench: Devbench, timeout: float = 10) -> dict[str, Any]:
    """
    Step the player out of power armor with the exit gesture (VR only: the offhand's A/X held 2 s, on top of
    timeout). An outcome other than switched or already raises DevbenchError.
    """
    return _switch(bench, "stepOut", _armor_state, timeout=timeout)


def leave_power_armor(bench: Devbench, timeout: float = 10) -> dict[str, Any]:
    """
    Get the player out of power armor (already out is fine): the scripted exit, then the exit gesture when a mod such
    as KabutoVR refuses it. Returns the answer of the call that got the player out; a failure raises DevbenchError.
    """
    try:
        return exit_power_armor(bench, timeout)
    except DevbenchError as error:
        if not isinstance(error.answer, dict) or error.answer.get("outcome") != "refused":
            raise
    return step_out_of_power_armor(bench, timeout)
