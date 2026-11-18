"""Checks: the game and VR from ping, the listed tools, a mod's actions and version from its health."""

import pytest

from devbench import (
    Devbench,
    DevbenchError,
    ModTool,
    RequirementError,
    require,
    require_actions,
    require_min_version,
    require_tools,
    require_version,
)

MOD = "immersiveflashlightvr"


def ping(game="fallout4", vr=True):
    return lambda args: {"ok": True, "game": game, "exe": "Fallout4VR.exe", "vr": vr}


def mod_health(fake, bench, version="1.3.0", actions=("health", "gesture", "zones", "light")):
    fake.tools[MOD] = lambda args: {"ok": True, "version": version, "actions": list(actions)}
    return ModTool(bench, MOD)


def test_require_passes_and_returns_the_ping(fake, bench):
    fake.tools["ping"] = ping()

    assert require(bench, "fallout4", vr=True)["exe"] == "Fallout4VR.exe"


def test_require_wrong_game(fake, bench):
    fake.tools["ping"] = ping(game="skyrim")

    with pytest.raises(RequirementError, match=r"is skyrim \(VR\), expected Fallout 4 VR: start the right game"):
        require(bench, "fallout4", vr=True)


def test_require_not_vr(fake, bench):
    fake.tools["ping"] = ping(vr=False)

    with pytest.raises(RequirementError, match=rf"port {fake.port} is fallout4 \(not VR\), expected Fallout 4 VR"):
        require(bench, "fallout4", vr=True)


def test_require_vr_when_flat_is_wanted(fake, bench):
    fake.tools["ping"] = ping(vr=True)

    with pytest.raises(RequirementError, match=r"expected Fallout 4:"):
        require(bench, "fallout4", vr=False)


def test_require_no_answer(fake):
    port = fake.port
    fake.close()

    with Devbench(port, timeout=5.0) as gone, pytest.raises(RequirementError) as raised:
        require(gone, "fallout4", vr=True)

    assert f"devbench does not answer on port {port}" in str(raised.value)
    assert "start the game with devbench deployed" in str(raised.value)


def test_require_refused_ping(fake, bench):
    fake.tools["ping"] = lambda args: (500, {"ok": False, "error": "boom"})

    with pytest.raises(RequirementError, match="does not answer.*boom") as raised:
        require(bench, "fallout4", vr=True)

    assert isinstance(raised.value.__cause__, DevbenchError)


def test_require_unknown_game(bench):
    with pytest.raises(ValueError, match="unknown game 'fallout5'"):
        require(bench, "fallout5", vr=True)


def test_require_tools_passes(fake, bench):
    fake.tools["ping"] = ping()
    fake.tools["frik"] = lambda args: {"ok": True}

    require_tools(bench, ["ping", "frik"])


def test_require_tools_names_each_missing_one(fake, bench):
    fake.tools["ping"] = ping()

    with pytest.raises(RequirementError, match="devbench lacks tools frik, menu: deploy the mods that register them"):
        require_tools(bench, ["ping", "frik", "menu"])


def test_require_actions_passes_and_returns_the_health(fake, bench):
    mod = mod_health(fake, bench)

    assert require_actions(mod, ["gesture", "light"])["version"] == "1.3.0"


def test_require_actions_names_each_missing_one(fake, bench):
    mod = mod_health(fake, bench, actions=["health", "gesture"])

    with pytest.raises(RequirementError, match=f"{MOD} lacks actions zones, light: deploy this build"):
        require_actions(mod, ["gesture", "zones", "light"])


def test_require_actions_without_an_actions_list(fake, bench):
    fake.tools[MOD] = lambda args: {"ok": True, "version": "1.3.0"}
    mod = ModTool(bench, MOD)

    with pytest.raises(RequirementError, match="answers health without actions.*deploy this build"):
        require_actions(mod, ["gesture"])


def test_require_actions_when_the_mod_tool_does_not_answer(fake, bench):
    mod = ModTool(bench, MOD)

    with pytest.raises(RequirementError, match=f"the mod's devbench tool {MOD} does not answer.*deploy this build"):
        require_actions(mod, ["gesture"])


def test_require_version_passes(fake, bench):
    mod = mod_health(fake, bench)

    assert require_version(mod, "1.3.0")["actions"][0] == "health"


def test_require_version_mismatch(fake, bench):
    mod = mod_health(fake, bench, version="1.2.0")

    with pytest.raises(RequirementError, match=f"the game runs {MOD} 1.2.0, the checkout is 1.3.0: deploy this build"):
        require_version(mod, "1.3.0")


def test_require_version_without_a_version(fake, bench):
    fake.tools[MOD] = lambda args: {"ok": True, "actions": ["health"]}
    mod = ModTool(bench, MOD)

    with pytest.raises(RequirementError, match="answers health without version"):
        require_version(mod, "1.3.0")


@pytest.mark.parametrize("running", ["0.79", "0.79.0", "0.80.1", "1.0", "0.79.2-beta"])
def test_require_min_version_passes_at_or_above(fake, bench, running):
    mod = mod_health(fake, bench, version=running)

    assert require_min_version(mod, "0.79")["version"] == running


@pytest.mark.parametrize("running", ["0.78.9", "0.9", "0.7"])
def test_require_min_version_below(fake, bench, running):
    mod = mod_health(fake, bench, version=running)

    with pytest.raises(RequirementError, match=f"the game runs {MOD} {running}, this run needs 0.79 or later"):
        require_min_version(mod, "0.79")


def test_require_min_version_that_is_not_a_version(fake, bench):
    mod = mod_health(fake, bench, version="dev")

    with pytest.raises(RequirementError, match=f"{MOD} reports version 'dev', not a dotted number"):
        require_min_version(mod, "0.79")
