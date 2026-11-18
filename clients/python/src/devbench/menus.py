"""The game's menus through devbench's menu tool: what is open, answer message boxes, close menus."""

from __future__ import annotations

import time
from collections.abc import Iterable

from devbench.client import Devbench, DevbenchError


def open_menus(bench: Devbench) -> list[str]:
    """The names of the menus open now, as the engine's menu map holds them (HUDMenu, MainMenu, ...)."""
    return bench.tool("menu", action="list")["openMenus"]


def message_box_text(bench: Devbench) -> str:
    """
    What the open message box says, as "header body" (either may be empty). A box that cannot be read, or none open,
    raises DevbenchError.
    """
    box = bench.tool("menu", action="describe")
    return f"{box.get('headerText') or ''} {box.get('bodyText') or ''}".strip()


def close_dialogs(bench: Devbench, limit: int = 20, pause: float = 0.5) -> list[str]:
    """
    Answer each open message box with its first button, as a new game's mod popups need, and return what each said
    ("header body", or "(not read: <error>)" when describe fails; the box is answered anyway). Stops after limit
    answers even with a box still open, and waits pause seconds after each for the next box to show. A refused
    accept raises DevbenchError.
    """
    answered: list[str] = []
    while len(answered) < limit and bench.tool("menu", action="list")["messageBoxOpen"]:
        try:
            said = message_box_text(bench)
        except DevbenchError as error:
            said = f"(not read: {error})"
        bench.tool("menu", action="accept", index=0)
        answered.append(said)
        time.sleep(pause)
    return answered


def close_menus(bench: Devbench, names: Iterable[str]) -> None:
    """
    Ask the game to close each named menu, ignoring a refusal. The close is queued for the engine, so the menu may
    still be open when this returns; one that is not open is no error.
    """
    for name in names:
        try:
            bench.tool("menu", action="close", name=name)
        except DevbenchError:
            pass
