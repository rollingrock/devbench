"""
VR hands: the token kept and passed to stop, stop on an exception, a refused stop, the pose arguments, turning a
controller in place, presses on one hand or several, the hand names, and the handedness getter and setter.
"""

import pytest

import devbench.vr
from devbench import DevbenchError, Hands


class _Input:
    """
    A scripted input tool: pose answers a token and both hands (a world target lands exactly, with its target), stop
    and press answer ok or as refuse says.
    """

    def __init__(self):
        self.calls = []
        self.token = "t1"
        self.refuse = set()
        self.tracked = {"available": True, "ready": True}
        self.left_handed = False
        self.hands = {
            "left": {"wand": [-10.0, 20.0, 100.0], "pitch": 0.0, "yaw": 5.0},
            "right": {"wand": [10.0, 20.0, 100.0], "pitch": 0.0, "yaw": -5.0},
        }

    def __call__(self, args):
        self.calls.append(args)
        action = args.get("action")
        if action in self.refuse:
            return 409, {"ok": False, "error": f"{action} refused"}
        if action == "capabilities":
            return {"capabilities": {"vrTrackedSet": self.tracked}}
        if action == "handedness":
            if "leftHanded" not in args:
                return {"leftHanded": self.left_handed}
            was, self.left_handed = self.left_handed, args["leftHanded"]
            return {"leftHanded": self.left_handed, "was": was, "changed": was != self.left_handed, "elapsedMs": 200}
        if action == "pose":
            hands = {
                side: {key: value for key, value in hand.items() if key != "target"}
                for side, hand in self.hands.items()
            }
            if isinstance(args.get("hands"), dict):
                for side, target in args["hands"].items():
                    if side in ("primary", "offhand"):
                        primary = "left" if self.left_handed else "right"
                        side = primary if side == "primary" else ("right" if primary == "left" else "left")
                    hand = hands[side]
                    if "world" in target:
                        hand["wand"] = hand["target"] = target["world"]
                    hand.update({key: target[key] for key in ("pitch", "yaw") if key in target})
            self.hands = hands
            return {"controlToken": self.token, "fresh": True, "corrections": 1, "hands": hands}
        return {"ok": True}

    def actions(self):
        return [call["action"] for call in self.calls]


@pytest.fixture
def tool(fake):
    script = _Input()
    fake.tools["input"] = script
    return script


@pytest.fixture
def slept(monkeypatch):
    """The waits press made, without waiting."""
    waits = []
    monkeypatch.setattr(devbench.vr.time, "sleep", waits.append)
    return waits


def test_ready_reads_the_vr_tracked_set_capability(bench, tool):
    assert Hands(bench, "e2e").ready()
    tool.tracked = {"available": True, "ready": False}
    assert not Hands(bench, "e2e").ready()


def test_start_takes_the_ready_pose_and_keeps_the_token(bench, tool):
    hands = Hands(bench, "e2e")

    hands.start()

    assert tool.calls == [{"action": "pose", "owner": "e2e", "hands": "ready", "head": "ready"}]
    assert hands.token == "t1"


def test_stop_passes_the_token_and_clears_it(bench, tool):
    hands = Hands(bench, "e2e")
    hands.start()

    hands.stop()
    hands.stop()

    assert tool.calls[1:] == [{"action": "stop", "owner": "e2e", "controlToken": "t1"}]
    assert hands.token is None


def test_stop_without_a_hold_sends_nothing(bench, tool):
    Hands(bench, "e2e").stop()

    assert tool.calls == []


def test_a_refused_stop_is_ignored(bench, tool):
    hands = Hands(bench, "e2e")
    hands.start()
    tool.refuse.add("stop")

    hands.stop()

    assert hands.token is None


def test_with_starts_and_stops(bench, tool):
    with Hands(bench, "e2e") as hands:
        assert hands.token == "t1"

    assert tool.actions() == ["pose", "stop"]


def test_with_stops_on_an_exception(bench, tool):
    with pytest.raises(RuntimeError, match="boom"), Hands(bench, "e2e"):
        raise RuntimeError("boom")

    assert tool.actions() == ["pose", "stop"]
    assert tool.calls[-1]["controlToken"] == "t1"


def test_a_failed_stop_does_not_hide_the_exception(fake, bench, tool):
    with pytest.raises(RuntimeError, match="boom"), Hands(bench, "e2e"):
        fake.close()
        raise RuntimeError("boom")


def test_a_later_token_replaces_the_kept_one(bench, tool):
    hands = Hands(bench, "e2e")
    hands.start()
    tool.token = "t2"

    hands.rest()

    assert hands.token == "t2"


def test_hold_ms_goes_on_every_pose(bench, tool):
    hands = Hands(bench, "e2e", hold_ms=300000)

    hands.start()
    hands.head("down")
    hands.pose(holdMs=1000)

    assert [call["holdMs"] for call in tool.calls] == [300000, 300000, 1000]


def test_place_sends_a_world_target_for_one_hand(bench, tool):
    answer = Hands(bench, "e2e").place("right", (1, 2.5, 3), pitch=123, yaw=-14, tolerance=0.5, move_ms=0)

    assert tool.calls[-1] == {
        "action": "pose",
        "owner": "e2e",
        "hands": {"right": {"world": [1.0, 2.5, 3.0], "pitch": 123, "yaw": -14}},
        "toleranceUnits": 0.5,
        "moveMs": 0,
    }
    assert answer["corrections"] == 1


def test_place_sends_a_head_offset_and_an_inward_yaw(bench, tool):
    Hands(bench, "e2e").place("offhand", [1, 2, 3], head_offset=(4, 5, -6), yaw_in=25)

    assert tool.calls[-1]["hands"] == {
        "offhand": {"world": [1.0, 2.0, 3.0], "headOffset": [4.0, 5.0, -6.0], "yawIn": 25}
    }


def test_place_leaves_out_what_is_not_given(bench, tool):
    Hands(bench, "e2e").place("left", [0, 0, 0])

    assert tool.calls[-1] == {"action": "pose", "owner": "e2e", "hands": {"left": {"world": [0.0, 0.0, 0.0]}}}


def test_place_and_press_pass_a_role_through(bench, tool, slept):
    hands = Hands(bench, "e2e")

    hands.place("primary", [0, 0, 0])
    hands.tap("offhand")

    assert list(tool.calls[0]["hands"]) == ["primary"]
    assert tool.calls[1]["hand"] == "offhand"


@pytest.mark.parametrize("hand", ["primaryHand", "Left", ""])
def test_place_and_press_reject_an_unknown_hand(bench, tool, slept, hand):
    hands = Hands(bench, "e2e")

    with pytest.raises(ValueError, match="left, right, primary, offhand"):
        hands.place(hand, [0, 0, 0])
    with pytest.raises(ValueError, match="left, right, primary, offhand"):
        hands.press(hand)

    assert tool.calls == []
    assert slept == []


def test_turn_keeps_the_wand_and_changes_the_given_angle(bench, tool):
    hands = Hands(bench, "e2e")
    hands.start()

    hands.turn("right", pitch=-70, move_ms=0)

    assert tool.calls[-1] == {
        "action": "pose",
        "owner": "e2e",
        "hands": {"right": {"world": [10.0, 20.0, 100.0], "pitch": -70, "yaw": -5.0}},
        "moveMs": 0,
    }
    assert hands.placed["right"]["pitch"] == -70


def test_turn_keeps_the_last_target_not_the_reading(bench, tool):
    hands = Hands(bench, "e2e")
    hands.place("left", [1, 2, 3])
    tool.hands["left"]["wand"] = [1.2, 2.0, 3.0]  # the next reading lands off the target

    hands.turn("left", yaw=30)
    hands.turn("left", yaw=40)

    assert [call["hands"]["left"]["world"] for call in tool.calls[1:]] == [[1.0, 2.0, 3.0], [1.0, 2.0, 3.0]]


@pytest.mark.parametrize(
    ("left_handed", "hand", "side"),
    [(False, "primary", "right"), (True, "primary", "left"), (False, "offhand", "left"), (True, "offhand", "right")],
)
def test_turn_resolves_a_role_through_the_handedness(bench, tool, left_handed, hand, side):
    tool.left_handed = left_handed
    hands = Hands(bench, "e2e")
    hands.start()

    hands.turn(hand, pitch=10)

    assert tool.calls[-2] == {"action": "handedness"}
    assert list(tool.calls[-1]["hands"]) == [side]
    assert tool.calls[-1]["hands"][side]["world"] == tool.hands[side]["wand"]


def test_turn_needs_a_pose_first(bench, tool):
    hands = Hands(bench, "e2e")

    with pytest.raises(RuntimeError, match="needs a pose"):
        hands.turn("left", pitch=0)
    hands.start()
    hands.stop()
    with pytest.raises(RuntimeError, match="needs a pose"):
        hands.turn("left", pitch=0)

    assert tool.actions() == ["pose", "stop"]


def test_turn_rejects_an_unknown_hand(bench, tool):
    hands = Hands(bench, "e2e")
    hands.start()

    with pytest.raises(ValueError, match="left, right, primary, offhand"):
        hands.turn("both", pitch=0)

    assert tool.actions() == ["pose"]


def test_head_sends_a_preset_or_a_pose(bench, tool):
    hands = Hands(bench, "e2e")

    hands.head("down")
    hands.head({"lookAt": "hands"})

    assert [call["head"] for call in tool.calls] == ["down", {"lookAt": "hands"}]


def test_press_waits_until_the_buttons_are_released(bench, tool, slept):
    Hands(bench, "e2e").press("left", ["grip", "a"], 250)

    assert tool.calls[-1] == {
        "action": "press",
        "owner": "e2e",
        "hand": "left",
        "buttons": ["grip", "a"],
        "durationMs": 250,
    }
    assert slept == [pytest.approx(0.35)]


def test_stick_pushes_the_thumbstick_and_waits_until_it_is_back(bench, tool, slept):
    Hands(bench, "e2e").stick("primary", 0, -1, 600)

    assert tool.calls[-1] == {
        "action": "press",
        "owner": "e2e",
        "hand": "primary",
        "stick": [0.0, -1.0],
        "durationMs": 600,
    }
    assert slept == [pytest.approx(0.7)]


def test_stick_rejects_an_unknown_hand(bench, tool, slept):
    with pytest.raises(ValueError, match="left, right, primary, offhand"):
        Hands(bench, "e2e").stick("both", 1, 0)

    assert tool.calls == []


def test_press_on_several_hands_overlaps_and_waits_once(bench, tool, slept):
    Hands(bench, "e2e").press(("primary", "offhand"), ["thumbstick"], 1500)

    assert [(call["hand"], call["buttons"], call["durationMs"]) for call in tool.calls] == [
        ("primary", ["thumbstick"], 1500),
        ("offhand", ["thumbstick"], 1500),
    ]
    assert slept == [pytest.approx(1.6)]


def test_stick_and_tap_take_several_hands(bench, tool, slept):
    hands = Hands(bench, "e2e")

    hands.stick(["left", "right"], 0, 1)
    hands.tap(["left", "right"], ["a"])

    assert [(call["hand"], call.get("stick"), call.get("buttons")) for call in tool.calls] == [
        ("left", [0.0, 1.0], None),
        ("right", [0.0, 1.0], None),
        ("left", None, ["a"]),
        ("right", None, ["a"]),
    ]


@pytest.mark.parametrize(
    ("hands", "error"),
    [(["left", "both"], "left, right, primary, offhand"), (["left", "left"], "given twice"), ([], "no hand")],
)
def test_several_hands_are_all_checked_before_any_press(bench, tool, slept, hands, error):
    with pytest.raises(ValueError, match=error):
        Hands(bench, "e2e").press(hands)

    assert tool.calls == []
    assert slept == []


def test_tap_and_long_press_use_the_framework_thresholds(bench, tool, slept):
    hands = Hands(bench, "e2e")

    hands.tap("right")
    hands.long_press("right", ["b"])

    assert [(call["buttons"], call["durationMs"]) for call in tool.calls] == [
        (["trigger"], devbench.vr.TAP_MS),
        (["b"], devbench.vr.LONG_PRESS_MS),
    ]


def test_a_press_without_a_hold_raises_devbench_error(bench, tool, slept):
    tool.refuse.add("press")

    with pytest.raises(DevbenchError, match="press refused"):
        Hands(bench, "e2e").press("left")

    assert slept == []


def test_handedness_reads_the_game(bench, tool):
    tool.left_handed = True

    assert devbench.vr.handedness(bench) == "left"
    assert tool.calls == [{"action": "handedness"}]


def test_set_handedness_returns_the_one_before(bench, tool):
    assert devbench.vr.set_handedness(bench, "left") == "right"
    assert devbench.vr.set_handedness(bench, "right", owner="e2e") == "left"

    assert tool.calls == [
        {"action": "handedness", "leftHanded": True},
        {"action": "handedness", "leftHanded": False, "owner": "e2e"},
    ]


def test_a_set_the_game_did_not_follow_raises_devbench_error(bench, tool):
    tool.refuse.add("handedness")

    with pytest.raises(DevbenchError, match="handedness refused") as raised:
        devbench.vr.set_handedness(bench, "left")

    assert raised.value.status == 409


def test_set_handedness_rejects_an_unknown_value(bench, tool):
    with pytest.raises(ValueError, match="unknown handedness 'both'"):
        devbench.vr.set_handedness(bench, "both")  # type: ignore[arg-type]

    assert tool.calls == []
