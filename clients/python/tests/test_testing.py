"""The pytest plugin: inner pytest sessions against the fake devbench, scripted as a game already in the test cell."""

import json
import re
import subprocess
from pathlib import Path
from urllib.parse import unquote, urlsplit

import pytest
from fake_devbench import FakeFiles, make_bmp, read_png

from devbench import Backup
from devbench.testing import Area, Config, Dependency

MOD_FILE = ("mygames", "Mods_Config/MyMod/MyMod.toml")

# where the fake rendertarget tool says it dumped a target, and that file in the files tool
DUMP_FILE = r"C:\Games\Fallout 4 VR\Data\F4SE\Plugins\devbench\captures\rt-{n}.bmp"
DUMP_KEY = ("data", "F4SE/Plugins/devbench/captures/rt-{n}.bmp")

CONFTEST = """
import devbench.testing

pytest_plugins = ["devbench.testing"]
DEVBENCH = devbench.testing.Config(
    game="fallout4",
    vr=True,
    tool="mymod",
    actions=["light", "zones"],
    version=lambda: "1.2.3",
    backup=[("mygames", "Mods_Config/MyMod/*")],
    log="MyMod.log",
    areas={AREAS},
    title="My Mod",
    picture=94,
    handedness={HANDEDNESS},
)
"""


def _dumped(x, y, n=1):
    """The fake dump's pixel: its column, row and the dump's number."""
    return bytes([x, y, n])


class FakeGame:
    """
    The fake devbench as Fallout 4 VR with a game loaded in QASmoke, the HUD and no message box open, the mod's tool
    (mymod) offering actions at version (its "say" action writes a line to its log, its "menus" action sets the menu
    state), its config file in the files tool, its log, and render targets that dump into the files tool.
    """

    def __init__(self, fake, actions=("light", "zones"), version="1.2.3"):
        self.files = FakeFiles({MOD_FILE: b"original"})
        self.log = ["started"]
        self.dumps = []
        # the game's handedness, the sets the input tool took, and the value a set of fails (504)
        self.left_handed = False
        self.handedness_sets = []
        self.refuse_handedness = None
        # the open menus, whether a message box is open, its describe answer (None: one that cannot be read), and
        # whether the menu list fails (the mod's "menus" action sets it)
        self.menus = ["HUDMenu"]
        self.box_open = False
        self.box = None
        self.menus_broken = False
        fake.tools["ping"] = lambda args: {"ok": True, "game": "fallout4", "vr": True}
        fake.tools["menu"] = self._menu
        fake.tools["inspect"] = lambda args: {"ok": True, "playerLoaded": True}
        fake.tools["console"] = lambda args: {"ok": True, "compiled": True, "lines": ["GetInCell >> 1.00"]}
        fake.tools["mymod"] = lambda args: self._mod(args, version, actions)
        fake.tools["files"] = self.files
        fake.tools["log"] = lambda args: {"ok": True, "linesScanned": len(self.log), "lines": list(self.log)}
        fake.tools["rendertarget"] = self._dump
        fake.tools["input"] = self._input

    def _mod(self, args, version, actions):
        if args["action"] == "say":
            self.log.append(args["text"])
        if args["action"] == "menus":
            # set from a test, after the setup answered any box: names, box_open, box, broken
            for key in ("names", "box_open", "box", "broken"):
                if key in args:
                    setattr(self, {"names": "menus", "broken": "menus_broken"}.get(key, key), args[key])
        return {"ok": True, "version": version, "actions": list(actions)}

    def _dump(self, args):
        assert (args["action"], args["maxDimension"]) == ("dump", 2048)
        self.dumps.append(args["index"])
        n = len(self.dumps)
        self.files.files[(DUMP_KEY[0], DUMP_KEY[1].format(n=n))] = make_bmp(6, 2, lambda x, y: _dumped(x, y, n))
        return {"ok": True, "file": DUMP_FILE.format(n=n)}

    def _input(self, args):
        assert args["action"] == "handedness"
        if "leftHanded" not in args:
            return {"leftHanded": self.left_handed}
        if args["leftHanded"] == self.refuse_handedness:
            return 504, {"ok": False, "error": "the wands did not swap"}
        self.handedness_sets.append(args["leftHanded"])
        was, self.left_handed = self.left_handed, args["leftHanded"]
        return {"leftHanded": self.left_handed, "was": was, "changed": was != self.left_handed, "elapsedMs": 200}

    def _menu(self, args):
        if args["action"] == "describe":
            if self.box is None:
                return 400, {"ok": False, "error": "currentMessage did not validate"}
            return {"ok": True, "messageBoxOpen": True, **self.box}
        assert args["action"] == "list"
        if self.menus_broken:
            return 501, {"ok": False, "error": "menu: off on this runtime"}
        return {"ok": True, "openMenus": list(self.menus), "messageBoxOpen": self.box_open}


@pytest.fixture
def game(fake):
    return FakeGame(fake)


def _conftest(pytester, areas=None, handedness=None):
    pytester.makeconftest(CONFTEST.replace("{AREAS}", repr(areas or {})).replace("{HANDEDNESS}", repr(handedness)))


def _record(reprec):
    """The plugin's record of an inline run."""
    config = reprec.getcall("pytest_configure").config
    return config.pluginmanager.get_plugin("devbench-run").record


def test_config_rejects_an_unknown_game():
    with pytest.raises(ValueError, match="unknown game 'starfield'"):
        Config(game="starfield", vr=False)


def test_config_rejects_a_bare_cell_name_for_cells():
    with pytest.raises(ValueError, match="neither a sequence of cells nor ANY_CELL"):
        Config(game="fallout4", vr=True, cells="CTest")
    Config(game="fallout4", vr=True, cells="any")
    Config(game="fallout4", vr=True, cells=["CTest"])


def test_config_calls_a_version_callable():
    assert Config(game="fallout4", vr=True, version=lambda: "1.2.3").expected_version() == "1.2.3"
    assert Config(game="fallout4", vr=True).expected_version() is None


def test_a_run_backs_up_the_files_and_puts_them_back(pytester, fake, game):
    _conftest(pytester)
    pytester.makepyfile(
        test_light="""
        from devbench import write_text

        def test_changes_the_config(game, mod, events, log_mark, report):
            write_text(game, "mygames", "Mods_Config/MyMod/MyMod.toml", "changed")
            write_text(game, "mygames", "Mods_Config/MyMod/MyMod.bak", "new")
            assert mod.name == "mymod" and events.prefix == "mymod" and log_mark.start == 1
            report.note("a note from the test")
        """
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert game.files.files == {MOD_FILE: b"original"}
    assert not (pytester.path / ".backup" / "manifest.json").exists()
    record = _record(reprec)
    assert record.notes == [
        "Ran in the game already in QASmoke.",
        "My Mod 1.2.3.",
        "a note from the test",
        "Backed up 1 files, and put them back after: 1 written again, 1 the run made deleted.",
    ]
    assert [(r.nodeid, r.outcome) for r in record.results] == [("test_light.py::test_changes_the_config", "passed")]


def test_a_failed_test_still_puts_the_backup_back(pytester, fake, game):
    _conftest(pytester)
    pytester.makepyfile(
        test_light="""
        from devbench import write_text

        def test_fails(game):
            write_text(game, "mygames", "Mods_Config/MyMod/MyMod.toml", "changed")
            assert False, "broke"
        """
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(failed=1)
    assert game.files.files == {MOD_FILE: b"original"}
    [result] = _record(reprec).results
    assert (result.outcome, result.phase) == ("failed", "call")
    assert "broke" in result.message


def test_a_missing_action_exits_with_its_message(pytester, fake):
    FakeGame(fake, actions=["light"])
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*mymod lacks actions zones: deploy this build*"])
    result.assert_outcomes()


def test_another_build_exits_with_the_versions(pytester, fake):
    FakeGame(fake, version="1.2.2")
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*the game runs mymod 1.2.2, the checkout is 1.2.3: deploy this build*"])


def test_another_game_exits(pytester, fake, game):
    fake.tools["ping"] = lambda args: {"ok": True, "game": "skyrim", "vr": True}
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*is skyrim (VR), expected Fallout 4 VR*"])


def test_a_leftover_backup_is_put_back_before_the_run(pytester, fake, game, bench):
    Backup(bench, [("mygames", "Mods_Config/MyMod/*")], pytester.path / ".backup").take()
    game.files.files[MOD_FILE] = b"left by a crashed run"
    _conftest(pytester)
    pytester.makepyfile(
        test_light="""
        from devbench import read_text

        def test_sees_the_original(game):
            assert read_text(game, "mygames", "Mods_Config/MyMod/MyMod.toml") == "original"
        """
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert _record(reprec).notes[0] == "Put back the backup an earlier run left: 1 files written, 0 deleted."


def test_e2e_restore_puts_the_backup_back_and_runs_no_test(pytester, fake, game, bench):
    Backup(bench, [("mygames", "Mods_Config/MyMod/*")], pytester.path / ".backup").take()
    game.files.files[MOD_FILE] = b"left by a crashed run"
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_never_runs(): assert False")

    result = pytester.runpytest("--devbench-port", str(fake.port), "--e2e-restore")

    assert result.ret == 0
    result.stdout.fnmatch_lines(["*put back the backup: 1 files written, 0 deleted*"])
    result.stdout.no_fnmatch_line("*test_never_runs*")
    assert game.files.files == {MOD_FILE: b"original"}
    assert not (pytester.path / ".backup" / "manifest.json").exists()


def test_e2e_restore_with_devbench_quit_quits_the_game_after_the_restore(pytester, fake, game, bench):
    Backup(bench, [("mygames", "Mods_Config/MyMod/*")], pytester.path / ".backup").take()
    game.files.files[MOD_FILE] = b"left by a crashed run"
    fake.tools["quit"] = lambda args: (fake.stop(), {"ok": True, "quitting": True})[1]
    _conftest(pytester)

    result = pytester.runpytest("--devbench-port", str(fake.port), "--e2e-restore", "--devbench-quit")

    assert result.ret == 0
    result.stdout.fnmatch_lines(["*put back the backup: 1 files written, 0 deleted; quit the game*"])
    assert game.files.files == {MOD_FILE: b"original"}
    assert fake.calls[-1] == ("POST", "/api/tool/quit", {"killAfterMs": 60000})


def test_e2e_restore_without_a_backup_says_so(pytester, fake):
    _conftest(pytester)

    result = pytester.runpytest("--devbench-port", str(fake.port), "--e2e-restore")

    assert result.ret == 0
    result.stdout.fnmatch_lines(["*no backup is waiting to be put back*"])
    assert fake.calls == []


def test_areas_order_the_tests_without_touching_the_game(pytester, fake):
    _conftest(pytester, areas={"test_c.py": "C", "test_a.py": "A"})
    for name in ("a", "b", "c"):
        pytester.makepyfile(**{f"test_{name}": "def test_one(): pass\ndef test_two(): pass"})

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    calls = reprec.getcalls("pytest_runtest_logreport")
    order = [call.report.nodeid for call in calls if call.report.when == "call"]
    assert order == [
        "test_c.py::test_one",
        "test_c.py::test_two",
        "test_a.py::test_one",
        "test_a.py::test_two",
        "test_b.py::test_one",
        "test_b.py::test_two",
    ]
    assert [r.area for r in _record(reprec).results][::2] == ["C", "A", "test_b.py"]
    assert fake.calls == []


def test_an_areas_marker_selects_its_files_tests(pytester, fake):
    pytester.makeconftest(
        """
        import devbench.testing

        pytest_plugins = ["devbench.testing"]
        DEVBENCH = devbench.testing.Config(
            game="fallout4",
            vr=True,
            areas={
                "test_light.py": devbench.testing.Area("Light action", marker="light"),
                "test_gestures.py": devbench.testing.Area("Gestures", marker="gestures"),
                "test_plain.py": "Plain",
            },
        )
        """
    )
    # not test_menus: the outer suite's module of that name is imported already
    for name in ("light", "gestures", "plain"):
        pytester.makepyfile(**{f"test_{name}": "def test_one(): pass\ndef test_two(): pass"})

    reprec = pytester.inline_run("--devbench-port", str(fake.port), "--strict-markers", "-m", "light")

    reprec.assertoutcome(passed=2)
    record = _record(reprec)
    assert [(r.nodeid, r.area) for r in record.results] == [
        ("test_light.py::test_one", "Light action"),
        ("test_light.py::test_two", "Light action"),
    ]
    assert len(reprec.getcall("pytest_deselected").items) == 4


def test_area_markers_are_registered(pytester, fake):
    pytester.makeconftest(
        """
        import devbench.testing

        pytest_plugins = ["devbench.testing"]
        DEVBENCH = devbench.testing.Config(
            game="fallout4", vr=True, areas={"test_light.py": devbench.testing.Area("Light action", marker="light")}
        )
        """
    )

    result = pytester.runpytest("--devbench-port", str(fake.port), "--markers")

    result.stdout.fnmatch_lines(["@pytest.mark.light: Light action"])


def test_area_rejects_a_marker_that_is_not_a_name():
    with pytest.raises(ValueError, match="'light action' is not a marker name"):
        Area("Light action", marker="light action")


REQUIRES_CONFTEST = """
import devbench.testing

pytest_plugins = ["devbench.testing"]
DEVBENCH = devbench.testing.Config(
    game="fallout4",
    vr=True,
    tool="mymod",
    requires=[devbench.testing.Dependency("frik", actions=["grip"], min_version="0.79")],
)
"""


def _frik(fake, version="0.80.1", actions=("health", "grip")):
    fake.tools["frik"] = lambda args: {"ok": True, "version": version, "actions": list(actions)}


def test_a_required_tool_is_checked_and_noted(pytester, fake, game):
    _frik(fake)
    pytester.makeconftest(REQUIRES_CONFTEST)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert "frik 0.80.1." in _record(reprec).notes


@pytest.mark.parametrize(
    ("frik", "said"),
    [
        (None, "*devbench lacks tools frik: deploy the mods that register them*"),
        ({"actions": ["health"]}, "*frik lacks actions grip: deploy this build*"),
        ({"version": "0.78.2"}, "*the game runs frik 0.78.2, this run needs 0.79 or later: deploy a newer build*"),
    ],
)
def test_a_missing_requirement_exits_with_what_is_missing(pytester, fake, game, frik, said):
    if frik is not None:
        _frik(fake, **frik)
    pytester.makeconftest(REQUIRES_CONFTEST)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == 2
    result.stdout.fnmatch_lines([said])
    result.assert_outcomes()


READY_CONFTEST = """
import devbench.testing

pytest_plugins = ["devbench.testing"]
DEVBENCH = devbench.testing.Config(
    game="fallout4",
    vr=True,
    tool="mymod",
    ready=lambda mod: mod.call("state")["ready"],
    ready_timeout=0.5,
)
"""


def _mod_ready_after(fake, game, states):
    """The mod's state action answers ready after `states` calls; the other actions are FakeGame's."""
    calls = []
    answer = fake.tools["mymod"]

    def mod(args):
        if args["action"] != "state":
            return answer(args)
        calls.append(args)
        return {"ok": True, "ready": len(calls) > states}

    fake.tools["mymod"] = mod
    return calls


def test_the_run_waits_for_the_mod_to_be_ready(pytester, fake, game):
    calls = _mod_ready_after(fake, game, states=2)
    pytester.makeconftest(READY_CONFTEST)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert len(calls) == 3


def test_a_mod_never_ready_exits(pytester, fake, game):
    _mod_ready_after(fake, game, states=10**6)
    pytester.makeconftest(READY_CONFTEST)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*waiting for mymod to be ready: mymod ready did not hold within 0.5 s*"])
    result.assert_outcomes()


def test_dependency_rejects_a_min_version_that_is_not_a_number():
    with pytest.raises(ValueError, match="min_version 'latest' of frik is not a dotted number"):
        Dependency("frik", min_version="latest")


def test_config_ready_needs_the_tool():
    with pytest.raises(ValueError, match="ready needs tool"):
        Config(game="fallout4", vr=True, ready=lambda mod: True)


def test_a_row_added_in_a_test_carries_its_name(pytester, fake):
    _conftest(pytester)
    pytester.makepyfile(
        helper="""
        def place(report, what):
            report.section("Placements", columns=["Test", "What"], rows=[[report.current_test, what]])
        """,
        test_light="""
        import pytest
        from helper import place

        def test_one(report):
            place(report, "left")

        @pytest.mark.parametrize("hand", ["right"])
        def test_two(report, hand):
            place(report, hand)

        class TestGroup:
            def test_three(self, report):
                place(report, "both")
        """,
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=3)
    record = _record(reprec)
    assert record.sections[0].rows == [
        ("test_one", "left"),
        ("test_two[right]", "right"),
        ("TestGroup::test_three", "both"),
    ]
    assert record.current_test is None


def test_a_conftest_without_devbench_is_a_usage_error(pytester):
    pytester.makeconftest('pytest_plugins = ["devbench.testing"]')
    pytester.makepyfile(test_light="def test_one(): pass")

    result = pytester.runpytest()

    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*no conftest defines DEVBENCH*"])


@pytest.mark.parametrize(
    ("fixture", "message"),
    [("mod", "*the mod fixture needs Config.tool*"), ("log_mark", "*the log_mark fixture needs Config.log*")],
)
def test_mod_and_log_mark_without_their_config_fields_are_usage_errors(pytester, fake, game, fixture, message):
    pytester.makeconftest(
        """
        import devbench.testing

        pytest_plugins = ["devbench.testing"]
        DEVBENCH = devbench.testing.Config(game="fallout4", vr=True)
        """
    )
    pytester.makepyfile(
        test_light=f"""
        def test_events(events): assert events.prefix is None
        def test_needs({fixture}): pass
        """
    )

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([message + "test_light.py::test_needs*"])
    result.assert_outcomes()


def test_events_without_config_tool_has_no_prefix(pytester, fake, game):
    pytester.makeconftest(
        """
        import devbench.testing

        pytest_plugins = ["devbench.testing"]
        DEVBENCH = devbench.testing.Config(game="fallout4", vr=True)
        """
    )
    pytester.makepyfile(test_light="def test_events(events): assert events.prefix is None")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    result.assert_outcomes(passed=1)


def test_a_mod_wraps_the_game_fixture(pytester, fake, game):
    pytester.makeconftest(
        CONFTEST.replace("{AREAS}", "{}").replace("{HANDEDNESS}", "None")
        + """
import pytest
from devbench import write_text

@pytest.fixture(scope="session")
def game(game, report):
    report.note("mod setup")
    yield game
    write_text(game, "mygames", "Mods_Config/MyMod/MyMod.toml", "teardown before the restore")
    report.note("mod teardown")
"""
    )
    pytester.makepyfile(test_light="def test_one(game): pass")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert game.files.files == {MOD_FILE: b"original"}
    notes = _record(reprec).notes
    assert notes.index("mod setup") < notes.index("mod teardown") < len(notes) - 1


def _page(result):
    """The report page a run printed the link of, and its text."""
    [line] = [line for line in result.outlines if line.startswith("Report: ")]
    page = Path(unquote(urlsplit(line.removeprefix("Report: ")).path).lstrip("/"))
    return page, page.read_text(encoding="utf-8")


def test_the_report_has_areas_failures_pictures_and_a_mod_section(pytester, fake, game):
    _conftest(pytester, areas={"test_b.py": "Area B", "test_a.py": "Area A"})
    pytester.makepyfile(
        test_a="""
        def test_breaks(mod):
            mod.call("say", text="before the failure")
            assert False, "it broke"
        """,
        test_b="""
        def test_adds_a_section(game, report, request):
            report.section("Placements", intro="where the hands went", columns=["Hand", "Landed"])
            report.section("Placements", rows=[["left", "<1 cm>"]])
            report.picture(request.node.nodeid, label="the panel").write_bytes(b"png")
            report.picture(request.node.nodeid, label="never written")
        """,
    )

    result = pytester.runpytest("--devbench-port", str(fake.port))

    result.assert_outcomes(passed=1, failed=1)
    page, text = _page(result)
    assert page.parent.parent == pytester.path / "build" / "e2e"
    headings = re.findall(r"<h2>(.*?)</h2>", text)
    assert headings == ["Failures", "Area B", "Area A", "Placements", "Pictures"]
    failures = text[text.index("<h2>Failures") : text.index("<h2>Area B")]
    assert "it broke" in failures and "<pre>before the failure</pre>" in failures
    assert "pictures/003-test_breaks.png" in failures
    width, height, pixels = read_png((page.parent / "pictures" / "003-test_breaks.png").read_bytes())
    assert (width, height) == (3, 2)
    assert pixels == [[tuple(_dumped(x, y))[::-1] for x in range(3)] for y in (1, 0)]
    assert game.dumps == [94]
    assert not any(root == "data" for root, _ in game.files.files)
    assert "where the hands went" in text and "&lt;1 cm&gt;" in text
    gallery = text[text.index("<h2>Pictures") :]
    assert "001-test_adds_a_section.png" in gallery and "the panel" in gallery and "never written" not in gallery


def test_the_first_failed_picture_turns_pictures_off(pytester, fake, game):
    fake.tools["rendertarget"] = lambda args: (501, {"ok": False, "error": "rendertarget: off on this runtime"})
    _conftest(pytester)
    pytester.makepyfile(test_a="def test_one(game): assert False\ndef test_two(game): assert False")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(failed=2)
    record = _record(reprec)
    first, second = (record.failures[f"test_a.py::test_{n}"] for n in ("one", "two"))
    assert first.picture is None and "off on this runtime" in first.picture_error
    assert second.picture is None and second.picture_error is None
    assert first.log == [] and second.log == []
    assert [note for note in record.notes if note.startswith("No failure pictures")] == [
        f"No failure pictures from test_a.py::test_one on: {first.picture_error}"
    ]
    assert sum(path == "/api/tool/rendertarget" for _, path, _ in fake.calls) == 1


def test_a_failure_records_the_open_menus_and_the_message_box(pytester, fake, game):
    _conftest(pytester)
    pytester.makepyfile(
        test_a="""
        def test_blocked(game):
            box = {"headerText": "Mod", "bodyText": "Pick <one>"}
            game.tool("mymod", action="menus", names=["HUDMenu", "MessageBoxMenu"], box_open=True, box=box)
            assert False, "inert: a game menu blocks the gestures"
        """
    )

    result = pytester.runpytest("--devbench-port", str(fake.port))

    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(
        ["*- devbench -*", "Open menus: HUDMenu, MessageBoxMenu", "Message box: Mod Pick <one>"]
    )
    _, text = _page(result)
    failures = text[text.index("<h2>Failures") :]
    assert "Open menus: HUDMenu, MessageBoxMenu" in failures
    assert "Message box: Mod Pick &lt;one&gt;" in failures
    # the menus are the first thing the failure reads, before the log and the picture
    failed = max(i for i, (_, path, body) in enumerate(fake.calls) if body and body.get("action") == "menus")
    assert [(path, body["action"]) for _, path, body in fake.calls[failed + 1 : failed + 3]] == [
        ("/api/tool/menu", "list"),
        ("/api/tool/menu", "describe"),
    ]


def test_a_message_box_that_cannot_be_read_still_lists_the_menus(pytester, fake, game):
    _conftest(pytester)
    pytester.makepyfile(
        test_a="""
        def test_one(game):
            game.tool("mymod", action="menus", names=["MessageBoxMenu"], box_open=True, box=None)
            assert False
        """
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    failure = _record(reprec).failures["test_a.py::test_one"]
    assert failure.menus == ["MessageBoxMenu"]
    assert failure.message_box.startswith("(not read: ") and "did not validate" in failure.message_box
    assert failure.menus_error is None


def test_a_menu_read_that_fails_leaves_a_note_and_the_next_failure_tries_again(pytester, fake, game):
    _conftest(pytester)
    pytester.makepyfile(
        test_a="""
        def test_one(game):
            game.tool("mymod", action="menus", broken=True)
            assert False

        def test_two(game):
            game.tool("mymod", action="menus", broken=False)
            assert False
        """
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(failed=2)
    record = _record(reprec)
    first, second = (record.failures[f"test_a.py::test_{n}"] for n in ("one", "two"))
    assert first.menus is None and "menu: off on this runtime" in first.menus_error
    assert first.menu_lines() == [f"Menus not read: {first.menus_error}"]
    assert second.menus == ["HUDMenu"] and second.menus_error is None
    assert not any("menu" in note.lower() for note in record.notes)


def test_a_failed_session_fixture_is_captured_once(pytester, fake, game):
    pytester.makeconftest(
        CONFTEST.replace("{AREAS}", "{}").replace("{HANDEDNESS}", "None")
        + """
import pytest

@pytest.fixture(scope="session")
def game(game):
    raise AssertionError("the mod's setup failed")
"""
    )
    pytester.makepyfile(test_a="def test_one(game): pass\ndef test_two(game): pass\ndef test_three(game): pass")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(failed=3)
    assert list(_record(reprec).failures) == ["test_a.py::test_one"]
    assert game.dumps == [94]


def test_a_test_without_the_game_takes_no_picture(pytester, fake, game):
    _conftest(pytester)
    pytester.makepyfile(test_a="def test_one(): assert False")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(failed=1)
    assert _record(reprec).failures == {}
    assert fake.calls == []


def test_collect_only_writes_no_report(pytester, fake):
    _conftest(pytester)
    pytester.makepyfile(test_a="def test_one(): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port), "--collect-only")

    result.stdout.no_fnmatch_line("Report: *")
    assert not (pytester.path / "build").exists()


def test_report_dir_holds_the_run_folders(pytester, fake):
    pytester.makeconftest(
        """
        import devbench.testing

        pytest_plugins = ["devbench.testing"]
        DEVBENCH = devbench.testing.Config(game="fallout4", vr=True, report_dir="out/e2e")
        """
    )
    pytester.makepyfile(test_a="def test_one(): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    page, _ = _page(result)
    assert page.parent.parent == pytester.path / "out" / "e2e"
    result.stdout.no_fnmatch_line("*PytestAssertRewriteWarning*")


def test_config_rejects_an_unknown_handedness_and_one_without_vr():
    with pytest.raises(ValueError, match="unknown handedness 'both'"):
        Config(game="fallout4", vr=True, handedness="both")
    with pytest.raises(ValueError, match="needs vr=True"):
        Config(game="fallout4", vr=False, handedness="left")


HANDED_TEST = """
from devbench.vr import handedness as game_handedness

def test_runs_left_handed(game, handedness):
    assert handedness == "left"
    assert game_handedness(game) == "left"
"""


def test_a_run_sets_the_handedness_and_puts_the_players_back(pytester, fake, game):
    _conftest(pytester, handedness="left")
    pytester.makepyfile(test_light=HANDED_TEST)

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert game.handedness_sets == [True, False]
    assert not game.left_handed
    assert not (pytester.path / ".backup" / "handedness.json").exists()
    notes = _record(reprec).notes
    assert "Ran left-handed." in notes
    assert "Put the player's handedness back: right-handed." in notes


def test_left_handed_option_overrides_the_config(pytester, fake, game):
    _conftest(pytester, handedness="right")
    pytester.makepyfile(test_light=HANDED_TEST)

    reprec = pytester.inline_run("--devbench-port", str(fake.port), "--left-handed")

    reprec.assertoutcome(passed=1)
    assert game.handedness_sets == [True, False]


def test_a_failed_session_still_puts_the_handedness_back(pytester, fake, game):
    _conftest(pytester, handedness="left")
    pytester.makepyfile(test_light="def test_fails(game): assert False")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(failed=1)
    assert not game.left_handed
    assert not (pytester.path / ".backup" / "handedness.json").exists()


def test_without_a_handedness_the_run_leaves_it_and_the_fixture_reads_the_game(pytester, fake, game):
    game.left_handed = True
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_reads(handedness): assert handedness == 'left'")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert game.handedness_sets == []
    assert not any("handed" in note for note in _record(reprec).notes)


def test_a_set_the_game_did_not_follow_exits_and_leaves_the_players(pytester, fake, game):
    game.refuse_handedness = True
    _conftest(pytester, handedness="left")
    pytester.makepyfile(test_light="def test_never_runs(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port))

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*setting the game left-handed*the wands did not swap*"])
    assert not game.left_handed
    assert not (pytester.path / ".backup" / "handedness.json").exists()
    assert game.files.files == {MOD_FILE: b"original"}


def _leave_handedness(pytester, handedness):
    """The handedness.json a crashed run leaves: the player's value, kept while the run had changed it."""
    folder = pytester.path / ".backup"
    folder.mkdir(exist_ok=True)
    (folder / "handedness.json").write_text(json.dumps({"handedness": handedness}), encoding="utf-8")


def test_a_leftover_handedness_is_put_back_before_the_run(pytester, fake, game):
    game.left_handed = True
    _leave_handedness(pytester, "right")
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_reads(handedness): assert handedness == 'right'")

    reprec = pytester.inline_run("--devbench-port", str(fake.port))

    reprec.assertoutcome(passed=1)
    assert game.handedness_sets == [False]
    assert not (pytester.path / ".backup" / "handedness.json").exists()
    assert "Put back the handedness an earlier run left: right-handed." in _record(reprec).notes


def test_e2e_restore_puts_the_handedness_back(pytester, fake, game):
    game.left_handed = True
    _leave_handedness(pytester, "right")
    _conftest(pytester)

    result = pytester.runpytest("--devbench-port", str(fake.port), "--e2e-restore")

    assert result.ret == 0
    result.stdout.fnmatch_lines(["*put back the handedness: right-handed*"])
    assert not game.left_handed
    assert not (pytester.path / ".backup" / "handedness.json").exists()


def test_e2e_restore_keeps_a_handedness_it_cannot_set_and_still_puts_the_files_back(pytester, fake, game, bench):
    Backup(bench, [("mygames", "Mods_Config/MyMod/*")], pytester.path / ".backup").take()
    game.files.files[MOD_FILE] = b"left by a crashed run"
    game.left_handed = True
    game.refuse_handedness = False
    _leave_handedness(pytester, "right")
    _conftest(pytester)

    result = pytester.runpytest("--devbench-port", str(fake.port), "--e2e-restore")

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*put back the backup: 1 files written*kept the handedness to put back*"])
    assert game.files.files == {MOD_FILE: b"original"}
    assert (pytester.path / ".backup" / "handedness.json").exists()


def test_left_handed_needs_a_vr_game(pytester):
    pytester.makeconftest(
        """
        import devbench.testing

        pytest_plugins = ["devbench.testing"]
        DEVBENCH = devbench.testing.Config(game="fallout4", vr=False)
        """
    )
    pytester.makepyfile(test_a="def test_one(): pass")

    result = pytester.runpytest("--left-handed")

    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*--left-handed needs a VR game*"])


BENCH_ONLY_CONFTEST = """
import devbench.testing

pytest_plugins = ["devbench.testing"]
DEVBENCH = devbench.testing.Config(game="fallout4", vr=True)
"""


def _pid_gone(pid):
    """Whether the process has exited, waiting up to 5 s."""
    from devbench.process import _Process
    from devbench.waits import wait_until

    process = _Process(pid)
    try:
        return wait_until(lambda: not process.alive(), 5, 0.05, f"process {pid} to exit")
    finally:
        process.close()


def test_a_run_launches_the_game_when_none_answers(pytester, launching):
    pytester.makeconftest(BENCH_ONLY_CONFTEST)
    pytester.makepyfile(test_up="def test_up(bench): assert bench.health()['game'] == 'fallout4'")

    result = pytester.runpytest("--devbench-launch", subprocess.list2cmdline(launching.game("--delay", "0.3")))

    result.assert_outcomes(passed=1)
    result.stdout.fnmatch_lines(["*no game answers: launching it*", "*the game is up after * s*"])
    assert len(launching.pids()) == 1


def test_the_launch_command_comes_from_the_environment(pytester, launching, monkeypatch):
    monkeypatch.setenv("DEVBENCH_LAUNCH", subprocess.list2cmdline(launching.game()))
    pytester.makeconftest(BENCH_ONLY_CONFTEST)
    pytester.makepyfile(test_up="def test_up(bench): pass")

    reprec = pytester.inline_run()

    reprec.assertoutcome(passed=1)
    assert _record(reprec).notes[0].startswith("Launched the game: up after")


def test_a_launch_that_fails_exits_with_why(pytester, launching):
    pytester.makeconftest(BENCH_ONLY_CONFTEST)
    pytester.makepyfile(test_up="def test_up(bench): pass")

    result = pytester.runpytest("--devbench-launch", str(launching.folder / "no-such-launcher.exe"))

    assert result.ret == 2
    result.stdout.fnmatch_lines(["*launching the game: the launch command could not start*"])


def test_a_game_that_answers_is_not_launched(pytester, fake, game, launching):
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    reprec = pytester.inline_run("--devbench-port", str(fake.port), "--devbench-launch", "no-such-launcher.exe")

    reprec.assertoutcome(passed=1)
    assert launching.pids() == []


def test_devbench_quit_quits_after_the_backup_is_put_back(pytester, fake, game):
    def quit_tool(args):
        fake.stop()
        return {"ok": True, "quitting": True, "killAfterMs": args["killAfterMs"]}

    fake.tools["quit"] = quit_tool
    _conftest(pytester)
    pytester.makepyfile(
        test_light="""
        from devbench import write_text

        def test_changes_the_config(game):
            write_text(game, "mygames", "Mods_Config/MyMod/MyMod.toml", "changed")
        """
    )

    reprec = pytester.inline_run("--devbench-port", str(fake.port), "--devbench-quit")

    reprec.assertoutcome(passed=1)
    quit_at = fake.calls.index(("POST", "/api/tool/quit", {"killAfterMs": 60000}))
    writes = [
        i for i, (_, path, body) in enumerate(fake.calls) if path == "/api/tool/files" and body["action"] == "write"
    ]
    assert writes and writes[-1] < quit_at
    assert game.files.files == {MOD_FILE: b"original"}
    assert _record(reprec).notes[-1] == "Quit the game."


def test_devbench_quit_also_quits_a_launched_game(pytester, launching):
    pytester.makeconftest(BENCH_ONLY_CONFTEST)
    pytester.makepyfile(test_up="def test_up(bench): pass")

    result = pytester.runpytest("--devbench-launch", subprocess.list2cmdline(launching.game()), "--devbench-quit")

    result.assert_outcomes(passed=1)
    assert _pid_gone(launching.pids()[0])


def test_devbench_quit_after_a_failed_setup(pytester, fake, game):
    FakeGame(fake, version="1.2.2")
    fake.tools["quit"] = lambda args: (fake.stop(), {"ok": True, "quitting": True})[1]
    _conftest(pytester)
    pytester.makepyfile(test_light="def test_uses_the_game(game): pass")

    result = pytester.runpytest("--devbench-port", str(fake.port), "--devbench-quit")

    assert result.ret == 2
    assert any(path == "/api/tool/quit" for _, path, _ in fake.calls)
