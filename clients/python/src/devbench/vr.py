"""VR hands through devbench's input tool: a held pose of the HMD and both controllers, hands placed at world points,
presses on top of the hold, and the game's handedness. The game's input handling sees all of it as the real devices.

A hand is a physical controller (left / right) or a role (primary / offhand), which devbench resolves against the game's
handedness when the call arrives.
"""

from __future__ import annotations

import http.client
import time
from collections.abc import Sequence
from typing import Any, Literal, cast, get_args

from devbench.client import Devbench, DevbenchError

# the game's handedness: the physical controller that is the primary hand
Handedness = Literal["left", "right"]
HANDEDNESS: tuple[Handedness, ...] = get_args(Handedness)

# F4VR-CommonFramework's gesture thresholds, not devbench's: a tap is released within 0.3 s, a long press held over
# 0.6 s
TAP_MS = 100
LONG_PRESS_MS = 1000

# extra wait after a press's duration so its release has reached the game when press returns
_RELEASE_MARGIN = 0.1

_HANDS = ("left", "right", "primary", "offhand")


def _check_hand(hand: str) -> None:
    if hand not in _HANDS:
        raise ValueError(f"hand must be one of {', '.join(_HANDS)}, not {hand!r}")


def _hand_list(hand: str | Sequence[str]) -> list[str]:
    """
    One hand name or several as a list, each checked; a name given twice raises ValueError. Nothing is sent before
    every name has passed.
    """
    hands = [hand] if isinstance(hand, str) else list(hand)
    if not hands:
        raise ValueError("no hand given")
    for name in hands:
        _check_hand(name)
    if len(set(hands)) != len(hands):
        raise ValueError(f"a hand is given twice: {hands}")
    return hands


def check_handedness(value: str) -> Handedness:
    """Return value as a Handedness; raise ValueError for anything but "left" / "right"."""
    if value not in HANDEDNESS:
        raise ValueError(f"unknown handedness {value!r}: expected 'left' or 'right'")
    return cast(Handedness, value)


def _from_wire(left_handed: object) -> Handedness:
    """The handedness of the input tool's leftHanded bool."""
    return "left" if left_handed else "right"


def handedness(bench: Devbench) -> Handedness:
    """The game's handedness: the controller that is the primary hand (bLeftHandedMode:VR on FO4VR)."""
    return _from_wire(bench.tool("input", action="handedness")["leftHanded"])


def set_handedness(bench: Devbench, value: Handedness, *, owner: str | None = None) -> Handedness:
    """
    Set the game's handedness and return the one before. devbench answers once the game's wands have swapped
    controllers, under the pose hold of owner when it runs one, else a short hold of its own; it raises DevbenchError
    when another sequence holds the devices (409) or the game did not follow (504, the old value put back).
    """
    args: dict[str, Any] = {"action": "handedness", "leftHanded": check_handedness(value) == "left"}
    if owner is not None:
        args["owner"] = owner
    return _from_wire(bench.tool("input", **args)["was"])


class Hands:
    """
    devbench's held pose as one owner: start() takes over the HMD and both controllers, stop() hands them back.
    A context manager that starts on entry and always stops on exit, so a failure never leaves the headset held.
    hold_ms (devbench's 60 s when None) goes on every pose; a hold that runs out makes the next press answer 409.
    """

    def __init__(self, bench: Devbench, owner: str, *, hold_ms: int | None = None) -> None:
        self.bench = bench
        self.owner = owner
        self.hold_ms = hold_ms
        self.token: str | None = None
        # the last pose answer's hands: per physical hand (left / right) its wand, pitch, yaw and, when that pose
        # placed it, target
        self.placed: dict[str, dict[str, Any]] = {}

    def __enter__(self) -> Hands:
        self.start()
        return self

    def __exit__(self, exc_type: type[BaseException] | None, *exc_info: object) -> None:
        if exc_type is None:
            self.stop()
            return
        try:
            self.stop()
        except (OSError, http.client.HTTPException):
            pass  # the exception already unwinding is the failure to report

    def ready(self) -> bool:
        """
        Whether devbench can drive the VR devices now (the vrTrackedSet capability: OpenVR found, hooks in place).
        A refused or missing input tool raises DevbenchError.
        """
        answer = self.bench.tool("input", action="capabilities")
        tracked = answer.get("capabilities", {}).get("vrTrackedSet", {})
        return bool(tracked.get("available") and tracked.get("ready"))

    def start(self) -> dict[str, Any]:
        """
        Take over the HMD and both controllers in devbench's ready pose and return its answer. It names the ready
        pose, so a hold this owner left behind (a crashed run) moves back to it too.
        """
        return self.pose(hands="ready", head="ready")

    def rest(self) -> dict[str, Any]:
        """Both hands and the head back in devbench's ready pose; returns devbench's answer."""
        return self.pose(hands="ready", head="ready")

    def stop(self) -> None:
        """
        Hand the devices back to the physical ones. Without a token it does nothing; a refused stop (the hold ran out,
        or another sequence took the devices) is ignored. The token and the placed hands are cleared either way.
        """
        self.placed = {}
        if self.token is None:
            return
        token, self.token = self.token, None
        try:
            self.bench.tool("input", action="stop", owner=self.owner, controlToken=token)
        except DevbenchError:
            pass

    def pose(self, **args: Any) -> dict[str, Any]:
        """
        One input pose call as this owner with the tool's own arguments (hands, head, holdMs, ...), keeping its
        control token for stop and its hands for turn. hold_ms fills holdMs when args leave it out. Returns devbench's
        answer.
        """
        if self.hold_ms is not None:
            args.setdefault("holdMs", self.hold_ms)
        answer = self.bench.tool("input", action="pose", owner=self.owner, **args)
        self.token = answer.get("controlToken") or self.token
        self.placed = answer.get("hands") or self.placed
        return answer

    def place(
        self,
        hand: str,
        world: Sequence[float],
        *,
        head_offset: Sequence[float] | None = None,
        pitch: float | None = None,
        yaw: float | None = None,
        yaw_in: float | None = None,
        corrections: int | None = None,
        tolerance: float | None = None,
        move_ms: int | None = None,
    ) -> dict[str, Any]:
        """
        Move a hand's wand (left / right / primary / offhand) to a world point; the other hand stays. head_offset
        (outward, forward, up; world units along the head's axes, outward toward the hand's own side) moves the point
        so one offset mirrors around the head. pitch / yaw (degrees) point the controller, passed as devbench takes
        them; yaw_in instead of yaw turns it toward the body for either hand. corrections, tolerance (world units) and
        move_ms tune the move. Returns devbench's answer: corrections made, and per hand where the wand landed and its
        error.
        """
        _check_hand(hand)
        target: dict[str, Any] = {"world": [float(c) for c in world]}
        if head_offset is not None:
            target["headOffset"] = [float(c) for c in head_offset]
        if pitch is not None:
            target["pitch"] = pitch
        if yaw is not None:
            target["yaw"] = yaw
        if yaw_in is not None:
            target["yawIn"] = yaw_in
        options = {"corrections": corrections, "toleranceUnits": tolerance, "moveMs": move_ms}
        return self.pose(hands={hand: target}, **{key: value for key, value in options.items() if value is not None})

    def turn(
        self, hand: str, *, pitch: float | None = None, yaw: float | None = None, move_ms: int | None = None
    ) -> dict[str, Any]:
        """
        Point a hand's controller (left / right / primary / offhand) to pitch / yaw (degrees; the one left out stays)
        with its wand kept where the last pose put it: that pose's target for the hand, else where its wand was read.
        A role is resolved through the game's handedness (one more call). Needs a pose of this Hands first
        (RuntimeError). Returns devbench's answer.
        """
        _check_hand(hand)
        if not self.placed:
            raise RuntimeError("turn needs a pose of this Hands first (start, place or pose)")
        side = hand
        if hand in ("primary", "offhand"):
            primary = handedness(self.bench)
            side = primary if hand == "primary" else ("left" if primary == "right" else "right")
        held = self.placed[side]
        return self.place(
            side,
            held.get("target") or held["wand"],
            pitch=held["pitch"] if pitch is None else pitch,
            yaw=held["yaw"] if yaw is None else yaw,
            move_ms=move_ms,
        )

    def head(self, preset_or_pose: str | dict[str, Any]) -> dict[str, Any]:
        """
        Turn the head: a preset ("ready", "down") or {pitch?, yaw?, lookAt?, ...} as the input tool takes it.
        Returns devbench's answer.
        """
        return self.pose(head=preset_or_pose)

    def press(
        self, hand: str | Sequence[str], buttons: Sequence[str] = ("trigger",), duration_ms: int = TAP_MS
    ) -> None:
        """
        Hold buttons (trigger, grip, a, b, thumbstick) on a hand (left / right / primary / offhand), or on several
        hands together, for duration_ms on top of the hold, and return once they are released. Without a hold of this
        owner devbench answers 409 (DevbenchError).
        """
        for name in _hand_list(hand):
            self.bench.tool(
                "input", action="press", owner=self.owner, hand=name, buttons=list(buttons), durationMs=duration_ms
            )
        time.sleep(duration_ms / 1000 + _RELEASE_MARGIN)

    def stick(self, hand: str | Sequence[str], x: float, y: float, duration_ms: int = TAP_MS) -> None:
        """
        Push a hand's (left / right / primary / offhand) thumbstick, or several hands' together, to x, y (each -1..1;
        x right, y up) for duration_ms on top of the hold, and return once it is back at center. The pose does not
        move, so a ray stays where it points. Without a hold of this owner devbench answers 409 (DevbenchError).
        """
        for name in _hand_list(hand):
            self.bench.tool(
                "input", action="press", owner=self.owner, hand=name, stick=[float(x), float(y)], durationMs=duration_ms
            )
        time.sleep(duration_ms / 1000 + _RELEASE_MARGIN)

    def tap(self, hand: str | Sequence[str], buttons: Sequence[str] = ("trigger",)) -> None:
        """A press short enough to count as a tap (TAP_MS)."""
        self.press(hand, buttons, TAP_MS)

    def long_press(self, hand: str | Sequence[str], buttons: Sequence[str] = ("trigger",)) -> None:
        """A press long enough to count as a long press (LONG_PRESS_MS)."""
        self.press(hand, buttons, LONG_PRESS_MS)
