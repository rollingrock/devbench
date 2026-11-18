"""Fallout 4 game setup: a game entered from the main menu or reused, refused elsewhere, and settled."""

import re

import pytest

from devbench import DevbenchError, RequirementError
from devbench.games.fallout4 import (
    ANY_CELL,
    Entered,
    Settled,
    at_main_menu_or_in_game,
    draw_weapon,
    enter_game,
    enter_power_armor,
    equip,
    exit_power_armor,
    in_cell,
    item_count,
    leave_power_armor,
    nearest_power_armor,
    settle,
    sheathe_weapon,
    step_into_power_armor,
    step_out_of_power_armor,
    unequip,
)

# a poll fast enough that a test waits milliseconds
FAST = {"interval": 0.01}


class FakeGame:
    """
    A scripted game behind the menu, inspect and console tools. menus are the open menus; cell is the player's cell.
    A coc shows LoadingMenu for load_polls menu lists, then the HUD in the target cell. boxes is the queue of open
    message boxes; pop_up(n) is asked at the nth menu list and returns a box text to open, or None. commands records
    the console commands.
    """

    def __init__(
        self,
        menus=("MainMenu",),
        cell=None,
        player_loaded=True,
        boxes=(),
        blocked=False,
        load_polls=2,
        pop_up=None,
        loads=True,
    ):
        self.menus = list(menus)
        self.cell = cell
        self.player_loaded = player_loaded
        self.boxes = list(boxes)
        self.blocked = blocked
        self.load_polls = load_polls
        self.pop_up = pop_up
        self.loads = loads
        self.commands = []
        self.lists = 0
        self._loading = None

    def install(self, fake):
        fake.tools["menu"] = self.menu
        fake.tools["inspect"] = lambda args: {"ok": True, "playerLoaded": self.player_loaded}
        fake.tools["console"] = self.console

    def menu(self, args):
        action = args["action"]
        if action == "list":
            self.lists += 1
            self._advance_load()
            if self.pop_up and (text := self.pop_up(self.lists)):
                self.boxes.append(text)
            return {"ok": True, "openMenus": list(self.menus), "messageBoxOpen": bool(self.boxes)}
        if action == "describe":
            return {"ok": True, "headerText": "", "bodyText": self.boxes[0]}
        if action == "accept":
            self.boxes.pop(0)
            return {"ok": True}
        raise AssertionError(f"unexpected menu action {action}")

    def _advance_load(self):
        if self._loading is None:
            return
        target, left = self._loading
        if left > 0:
            self.menus = ["LoadingMenu"]
            self._loading = (target, left - 1)
        elif self.loads:
            self.menus = ["HUDMenu"]
            self.cell = target
            self._loading = None

    def console(self, args):
        command = args["command"]
        self.commands.append(command)
        verb, _, operand = command.partition(" ")
        if verb == "player.GetInCell":
            value = 1 if operand == self.cell else 0
            return {"ok": True, "compiled": True, "captured": True, "lines": [f"GetInCell >> {value:.2f}"]}
        if verb == "coc":
            if self.blocked:
                return {"ok": True, "executed": True, "captured": False, "blocked": True}
            self._loading = (operand, self.load_polls)
            return {"ok": True, "executed": True, "captured": False, "blocked": False}
        raise AssertionError(f"unexpected console command {command}")


def test_enter_game_from_the_main_menu_starts_one_in_the_cell(fake, bench):
    game = FakeGame(pop_up=lambda n: "Mod installed" if n == 3 else None)
    game.install(fake)

    entered = enter_game(bench, quiet=0.05, **FAST)

    assert entered == Entered(started=True, dialogs=["Mod installed"], quiet=True, cell="QASmoke")
    assert game.commands == ["coc QASmoke"]
    assert game.cell == "QASmoke"


def test_enter_game_answers_a_main_menu_dialog_before_the_coc(fake, bench):
    game = FakeGame(boxes=["Welcome"])
    game.install(fake)

    entered = enter_game(bench, cell="TestCell", quiet=0.05, **FAST)

    assert entered.started
    assert entered.dialogs == ["Welcome"]
    assert game.commands == ["coc TestCell"]


def test_enter_game_raises_when_the_coc_is_blocked(fake, bench):
    FakeGame(blocked=True).install(fake)

    with pytest.raises(DevbenchError, match="blocked"):
        enter_game(bench, **FAST)


def test_enter_game_raises_when_the_hud_never_comes(fake, bench):
    FakeGame(loads=False).install(fake)

    with pytest.raises(TimeoutError, match="the game in QASmoke"):
        enter_game(bench, timeout=0.1, **FAST)


def test_enter_game_reuses_a_game_in_the_cell(fake, bench):
    game = FakeGame(menus=["HUDMenu"], cell="QASmoke", boxes=["Hello"])
    game.install(fake)

    entered = enter_game(bench, **FAST)

    assert entered == Entered(started=False, dialogs=["Hello"], quiet=None, cell="QASmoke")
    assert game.commands == ["player.GetInCell QASmoke"]


def test_enter_game_reuses_a_game_in_one_of_the_accepted_cells(fake, bench):
    game = FakeGame(menus=["HUDMenu"], cell="CTest")
    game.install(fake)

    entered = enter_game(bench, cells=["Other", "CTest"], **FAST)

    assert entered == Entered(started=False, dialogs=[], quiet=None, cell="CTest")
    assert game.commands == ["player.GetInCell QASmoke", "player.GetInCell Other", "player.GetInCell CTest"]


def test_enter_game_takes_a_bare_cell_name_as_one_cell(fake, bench):
    FakeGame(menus=["HUDMenu"], cell="CTest").install(fake)

    assert enter_game(bench, cells="CTest", **FAST).cell == "CTest"


def test_enter_game_reuses_a_game_in_any_cell(fake, bench):
    game = FakeGame(menus=["HUDMenu"], cell="SanctuaryExt", boxes=["Hello"])
    game.install(fake)

    entered = enter_game(bench, cells=ANY_CELL, **FAST)

    assert entered == Entered(started=False, dialogs=["Hello"], quiet=None, cell=None)
    assert game.commands == []


def test_enter_game_from_the_main_menu_starts_in_the_cell_whatever_cells_says(fake, bench):
    game = FakeGame()
    game.install(fake)

    entered = enter_game(bench, cell="CTest", cells=ANY_CELL, quiet=0.05, **FAST)

    assert entered.started and entered.cell == "CTest"
    assert game.commands == ["coc CTest"]


def test_enter_game_names_the_accepted_cells_when_it_refuses(fake, bench):
    FakeGame(menus=["HUDMenu"], cell="SanctuaryExt").install(fake)

    with pytest.raises(RequirementError, match="outside any of QASmoke, CTest is loaded"):
        enter_game(bench, cells=["CTest"], **FAST)


def test_enter_game_refuses_a_game_in_another_cell(fake, bench):
    game = FakeGame(menus=["HUDMenu"], cell="SanctuaryExt")
    game.install(fake)

    with pytest.raises(RequirementError, match="outside QASmoke.*quit to the main menu"):
        enter_game(bench, **FAST)
    assert not any(command.startswith("coc") for command in game.commands)


@pytest.mark.parametrize(
    ("menus", "player_loaded"), [(["LoadingMenu"], True), (["FaderMenu"], False)], ids=["loading", "no-player"]
)
def test_enter_game_refuses_neither_menu_nor_game(fake, bench, menus, player_loaded):
    game = FakeGame(menus=menus, player_loaded=player_loaded)
    game.install(fake)

    with pytest.raises(RequirementError, match="neither at the main menu nor in a game"):
        enter_game(bench, **FAST)
    assert game.commands == []


def test_settle_returns_once_quiet(fake, bench):
    FakeGame(menus=["HUDMenu"]).install(fake)

    assert settle(bench, quiet=0.05, limit=5, **FAST) == Settled(dialogs=[], quiet=True)


def test_settle_waits_out_the_quiet_period_after_each_dialog(fake, bench):
    FakeGame(menus=["HUDMenu"], pop_up=lambda n: f"popup {n}" if n in (2, 4) else None).install(fake)

    settled = settle(bench, quiet=0.05, limit=5, **FAST)

    assert settled == Settled(dialogs=["popup 2", "popup 4"], quiet=True)


def test_settle_gives_up_at_the_limit(fake, bench):
    FakeGame(menus=["HUDMenu"], pop_up=lambda n: f"popup {n}").install(fake)

    settled = settle(bench, quiet=0.2, limit=0.1, **FAST)

    assert not settled.quiet
    assert settled.dialogs


def test_in_cell_is_false_at_the_main_menu(fake, bench):
    game = FakeGame(cell="QASmoke")
    game.install(fake)

    assert not in_cell(bench, "QASmoke")
    assert game.commands == []


def test_in_cell_reads_get_in_cell(fake, bench):
    FakeGame(menus=["HUDMenu"], cell="QASmoke").install(fake)

    assert in_cell(bench, "QASmoke")
    assert not in_cell(bench, "SanctuaryExt")


@pytest.mark.parametrize(
    "menus, ready",
    [
        (["MainMenu"], True),
        (["HUDMenu"], True),
        (["MainMenu", "LoadingMenu"], False),
        (["HUDMenu", "LoadingMenu"], False),
        (["LoadingMenu"], False),
        ([], False),
    ],
)
def test_at_main_menu_or_in_game(menus, ready):
    assert at_main_menu_or_in_game(menus) is ready


class FakePlayer:
    """
    A scripted player behind the console and player tools. counts maps a form to the item count GetItemCount prints;
    answers maps a player action to the list of answers it gives in turn (the last one repeats). commands records the
    console commands, calls the player tool's arguments.
    """

    def __init__(self, counts=None, answers=None):
        self.counts = dict(counts or {})
        self.answers = {action: list(queue) for action, queue in (answers or {}).items()}
        self.commands = []
        self.calls = []

    def install(self, fake):
        fake.tools["console"] = self.console
        fake.tools["player"] = self.player

    def console(self, args):
        command = args["command"]
        self.commands.append(command)
        verb, _, operand = command.partition(" ")
        if verb == "player.getitemcount":
            count = self.counts.get(operand, 0)
            return {"ok": True, "compiled": True, "captured": True, "lines": [f"GetItemCount >> {count:.2f}"]}
        if verb == "player.additem":
            form, count = operand.split()
            self.counts[form] = self.counts.get(form, 0) + int(count)
        return {"ok": True, "compiled": True, "captured": True, "lines": []}

    def player(self, args):
        self.calls.append(args)
        queue = self.answers[args["action"]]
        answer = queue.pop(0) if len(queue) > 1 else queue[0]
        return {"ok": True, "action": args["action"], **answer}


def test_equip_adds_one_first_when_the_player_has_none(fake, bench):
    player = FakePlayer()
    player.install(fake)

    equip(bench, "0000463F")

    assert player.commands == [
        "player.getitemcount 0000463F",
        "player.additem 0000463F 1",
        "player.equipitem 0000463F",
    ]


def test_equip_a_held_item_adds_none(fake, bench):
    player = FakePlayer(counts={"0000463F": 2})
    player.install(fake)

    equip(bench, "0000463F")

    assert player.commands == ["player.getitemcount 0000463F", "player.equipitem 0000463F"]


def test_unequip_and_item_count(fake, bench):
    player = FakePlayer(counts={"0000463F": 3})
    player.install(fake)

    unequip(bench, "0000463F")

    assert item_count(bench, "0000463F") == 3
    assert player.commands == ["player.unequipitem 0000463F", "player.getitemcount 0000463F"]


@pytest.mark.parametrize("outcome", ["switched", "already"])
def test_draw_weapon_returns_the_answer_when_drawn(fake, bench, outcome):
    answer = {"outcome": outcome, "drawn": True, "weaponState": "drawn", "equipped": None}
    player = FakePlayer(answers={"drawWeapon": [answer]})
    player.install(fake)

    assert draw_weapon(bench, timeout=5) == {"ok": True, "action": "drawWeapon", **answer}
    assert player.calls == [{"action": "drawWeapon", "timeoutMs": 5000}]


@pytest.mark.parametrize("outcome", ["refused", "notWaited"])
def test_sheathe_weapon_raises_when_not_sheathed(fake, bench, outcome):
    answer = {"outcome": outcome, "drawn": True, "weaponState": "sheathing", "note": "the state did not settle"}
    FakePlayer(answers={"sheatheWeapon": [answer]}).install(fake)

    with pytest.raises(
        DevbenchError, match=re.escape(f"sheatheWeapon: {outcome}, the weapon is sheathing (the state did not settle)")
    ) as raised:
        sheathe_weapon(bench)
    assert raised.value.tool == "player"
    assert raised.value.answer["outcome"] == outcome


FRAME = {"formId": "0x0001F2A3", "baseFormId": "0x00134C5C", "name": "Power Armor Frame", "distance": 212.5}


def test_nearest_power_armor_is_the_first_frame_listed(fake, bench):
    player = FakePlayer(answers={"findPowerArmor": [{"inPowerArmor": False, "radius": 800, "frames": [FRAME]}]})
    player.install(fake)

    assert nearest_power_armor(bench, radius=800) == FRAME
    assert player.calls == [{"action": "findPowerArmor", "radius": 800, "limit": 1}]


def test_nearest_power_armor_is_none_without_a_frame(fake, bench):
    FakePlayer(answers={"findPowerArmor": [{"inPowerArmor": False, "radius": 500, "frames": []}]}).install(fake)

    assert nearest_power_armor(bench) is None


def test_enter_power_armor_sends_the_frame_and_returns_the_answer(fake, bench):
    player = FakePlayer(answers={"enterPowerArmor": [{"outcome": "switched", "inPowerArmor": True}]})
    player.install(fake)

    answer = enter_power_armor(bench, form_id=FRAME["formId"], timeout=3)

    assert answer["inPowerArmor"]
    assert player.calls == [{"action": "enterPowerArmor", "formId": FRAME["formId"], "timeoutMs": 3000}]


def test_exit_power_armor_raises_when_refused(fake, bench):
    answer = {"outcome": "refused", "inPowerArmor": True, "note": "a mod can block it"}
    FakePlayer(answers={"exitPowerArmor": [answer]}).install(fake)

    with pytest.raises(DevbenchError, match=r"exitPowerArmor: refused, inPowerArmor True \(a mod can block it\)"):
        exit_power_armor(bench)


def test_step_into_power_armor_raises_when_not_aimed_with_the_aim(fake, bench):
    aim = {"matched": False, "yawDeg": 12.0, "passes": []}
    player = FakePlayer(answers={"stepIn": [{"outcome": "notAimed", "inPowerArmor": False, "aim": aim}]})
    player.install(fake)

    with pytest.raises(DevbenchError, match=r"stepIn: notAimed, inPowerArmor False, aim \{'matched': False") as raised:
        step_into_power_armor(bench, radius=300)
    assert raised.value.answer["aim"] == aim
    assert player.calls == [{"action": "stepIn", "radius": 300, "timeoutMs": 10000}]


def test_step_out_of_power_armor_returns_the_answer(fake, bench):
    FakePlayer(answers={"stepOut": [{"outcome": "switched", "inPowerArmor": False}]}).install(fake)

    assert step_out_of_power_armor(bench)["outcome"] == "switched"


def test_leave_power_armor_steps_out_when_the_exit_is_refused(fake, bench):
    player = FakePlayer(
        answers={
            "exitPowerArmor": [{"outcome": "refused", "inPowerArmor": True}],
            "stepOut": [{"outcome": "switched", "inPowerArmor": False}],
        }
    )
    player.install(fake)

    assert leave_power_armor(bench)["action"] == "stepOut"
    assert [call["action"] for call in player.calls] == ["exitPowerArmor", "stepOut"]


@pytest.mark.parametrize("outcome", ["switched", "already"])
def test_leave_power_armor_does_not_step_out_after_an_exit(fake, bench, outcome):
    player = FakePlayer(answers={"exitPowerArmor": [{"outcome": outcome, "inPowerArmor": False}]})
    player.install(fake)

    assert leave_power_armor(bench)["outcome"] == outcome
    assert [call["action"] for call in player.calls] == ["exitPowerArmor"]


def test_leave_power_armor_raises_when_stepping_out_fails_too(fake, bench):
    FakePlayer(
        answers={
            "exitPowerArmor": [{"outcome": "refused", "inPowerArmor": True}],
            "stepOut": [{"outcome": "refused", "inPowerArmor": True}],
        }
    ).install(fake)

    with pytest.raises(DevbenchError, match="stepOut: refused"):
        leave_power_armor(bench)


def test_leave_power_armor_raises_a_failed_exit_other_than_refused(fake, bench):
    player = FakePlayer(answers={"exitPowerArmor": [{"outcome": "notWaited", "inPowerArmor": True}]})
    player.install(fake)

    with pytest.raises(DevbenchError, match="exitPowerArmor: notWaited"):
        leave_power_armor(bench)
    assert [call["action"] for call in player.calls] == ["exitPowerArmor"]
