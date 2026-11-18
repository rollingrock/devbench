"""Menus: the open menus, message boxes answered until none is open, menus closed despite a refusal."""

import pytest

from devbench import DevbenchError, close_dialogs, close_menus, message_box_text, open_menus


class FakeMenus:
    """
    A scripted menu tool: boxes is the queue of open message boxes, each a describe answer or None for a box whose
    describe fails; accept pops the first one. closed records the close requests; refuse names menus whose close
    answers 400.
    """

    def __init__(self, boxes, open_names=("HUDMenu",), refuse=()):
        self.boxes = list(boxes)
        self.open_names = list(open_names)
        self.refuse = set(refuse)
        self.accepted = []
        self.closed = []

    def __call__(self, args):
        action = args.get("action", "list")
        if action == "list":
            return {"ok": True, "openMenus": self.open_names, "messageBoxOpen": bool(self.boxes)}
        if action == "describe":
            box = self.boxes[0]
            if box is None:
                return (400, {"ok": False, "error": "currentMessage did not validate"})
            return {"ok": True, "messageBoxOpen": True, **box}
        if action == "accept":
            self.accepted.append(args.get("index"))
            self.boxes.pop(0)
            return {"ok": True}
        if action == "close":
            if args["name"] in self.refuse:
                return (400, {"ok": False, "error": "refused"})
            self.closed.append(args["name"])
            return {"ok": True, "queued": True}
        raise AssertionError(f"unexpected menu action {action}")


def test_open_menus_lists_the_names(fake, bench):
    fake.tools["menu"] = FakeMenus([], open_names=["HUDMenu", "PauseMenu"])

    assert open_menus(bench) == ["HUDMenu", "PauseMenu"]


def test_message_box_text_joins_header_and_body(fake, bench):
    fake.tools["menu"] = FakeMenus([{"headerText": "Mod", "bodyText": "Welcome"}])
    assert message_box_text(bench) == "Mod Welcome"

    fake.tools["menu"] = FakeMenus([{"headerText": None, "bodyText": "Installed"}])
    assert message_box_text(bench) == "Installed"


def test_message_box_text_raises_for_a_box_it_cannot_read(fake, bench):
    fake.tools["menu"] = FakeMenus([None])

    with pytest.raises(DevbenchError, match="currentMessage did not validate"):
        message_box_text(bench)


def test_close_dialogs_answers_until_none_is_open(fake, bench):
    menus = FakeMenus([{"headerText": "Mod", "bodyText": "Welcome"}, {"headerText": "", "bodyText": "Installed"}])
    fake.tools["menu"] = menus

    assert close_dialogs(bench, pause=0) == ["Mod Welcome", "Installed"]
    assert menus.accepted == [0, 0]
    assert not menus.boxes


def test_close_dialogs_with_none_open_answers_nothing(fake, bench):
    menus = FakeMenus([])
    fake.tools["menu"] = menus

    assert close_dialogs(bench, pause=0) == []
    assert menus.accepted == []


def test_close_dialogs_answers_a_box_it_cannot_read(fake, bench):
    menus = FakeMenus([None, {"headerText": "Mod", "bodyText": "Hi"}])
    fake.tools["menu"] = menus

    said = close_dialogs(bench, pause=0)

    assert said[0].startswith("(not read: ")
    assert "currentMessage did not validate" in said[0]
    assert said[1] == "Mod Hi"
    assert menus.accepted == [0, 0]


def test_close_dialogs_stops_at_the_limit(fake, bench):
    menus = FakeMenus([{"bodyText": str(n)} for n in range(5)])
    fake.tools["menu"] = menus

    assert close_dialogs(bench, limit=3, pause=0) == ["0", "1", "2"]
    assert len(menus.boxes) == 2


def test_close_dialogs_raises_on_a_refused_accept(fake, bench):
    menus = FakeMenus([{"bodyText": "stuck"}])
    fake.tools["menu"] = lambda args: (
        (400, {"ok": False, "error": "refusing to call through an unverified layout"})
        if args.get("action") == "accept"
        else menus(args)
    )

    with pytest.raises(DevbenchError, match="unverified layout"):
        close_dialogs(bench, pause=0)


def test_close_menus_ignores_a_refusal(fake, bench):
    menus = FakeMenus([], refuse={"PauseMenu"})
    fake.tools["menu"] = menus

    close_menus(bench, ["PauseMenu", "Console"])

    assert menus.closed == ["Console"]
