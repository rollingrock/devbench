"""The pytest hooks and fixtures of devbench.testing: the run's setup and teardown around a mod's suite."""

from __future__ import annotations

import json
import os
import socket
import time
import types
from collections.abc import Callable, Generator, Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path, PureWindowsPath
from typing import Any, NoReturn

import pytest

from devbench import vr
from devbench.checks import (
    RequirementError,
    require,
    require_actions,
    require_min_version,
    require_tools,
    require_version,
)
from devbench.client import HOST, Devbench, DevbenchError, ModTool, find_port
from devbench.events import Events
from devbench.files import Backup, delete_file, read_bytes
from devbench.games import fallout4
from devbench.log import LogMark
from devbench.menus import message_box_text
from devbench.process import launch, quit_game
from devbench.testing._config import Config, Dependency
from devbench.testing._picture import bmp_to_png
from devbench.testing._record import Failure, Record
from devbench.testing.report import write
from devbench.waits import wait_until

# what setup failures exit the session with
_EXIT_CODE = 2

# the log lines since the test started that a failure keeps
_LOG_TAIL = 100

# how long a connect to the game's port may take before nothing counts as listening there
_CONNECT_TIMEOUT = 1.0

# --devbench-quit: the quit tool's watchdog terminates a game whose shutdown hangs this long, and the wait for its
# process outlasts it
_QUIT_KILL_AFTER = 60.0
_QUIT_TIMEOUT = 90.0

# how often Config.ready is polled, in seconds
_READY_INTERVAL = 0.1

# the longest side of a failure's dump, in pixels: a third of FO4VR's 6144-wide frame, so each eye is 1024 wide
_PICTURE_SIZE = 2048

# the game setup of each game: enter a game the tests can run in, and say what was done
_ENTER: dict[str, Callable[[Devbench, str, Sequence[str] | str | None], fallout4.Entered]] = {
    "fallout4": lambda bench, cell, cells: fallout4.enter_game(bench, cell, cells=cells),
}


@dataclass
class _Run:
    """
    The session's state, kept on pytest's config: the mod's Config, the folder of the conftest that defines it, the
    run's record, the log mark the game fixture took, the game while that fixture is up, and the current test's log
    mark. Registered as a plugin, it records each test's result and what a failed test leaves.
    """

    config: Config
    home: Path
    record: Record
    mark: LogMark | None = None
    game: Devbench | None = None
    test_mark: tuple[str, LogMark] | None = None
    # set by the first failure picture that could not be taken: no later failure tries again
    pictures_off: bool = field(default=False)
    # the crashes of the setup failures captured so far: a session fixture's error fails every later test's setup
    # again, and only its first test takes the picture and log tail
    setup_crashes: set[tuple[str, int, str]] = field(default_factory=set)
    # the handedness the game fixture set for the run; None while it set none
    handedness: vr.Handedness | None = None

    def backup(self, bench: Devbench) -> Backup | None:
        """The backup of Config.backup in <home>/.backup, or None when the config backs nothing up."""
        return Backup(bench, list(self.config.backup), self.home / ".backup") if self.config.backup else None

    def backup_pending(self) -> bool:
        """
        Whether a backup a run left waits in <home>/.backup, checked without a connection to the game: the manifest
        Backup writes is there.
        """
        return bool(self.config.backup) and (self.home / ".backup" / "manifest.json").is_file()

    @property
    def handedness_file(self) -> Path:
        """Where the player's handedness waits while a run has changed it: <home>/.backup/handedness.json."""
        return self.home / ".backup" / "handedness.json"

    def handedness_pending(self) -> bool:
        """Whether a player's handedness waits to be put back, checked without a connection to the game."""
        return self.handedness_file.is_file()

    def keep_handedness(self, bench: Devbench) -> vr.Handedness:
        """
        Read the player's handedness and write it to handedness_file before the run changes it; returns it. The file
        goes to a .partial first and replaces handedness_file only once whole.
        """
        was = vr.handedness(bench)
        self.handedness_file.parent.mkdir(parents=True, exist_ok=True)
        partial = self.handedness_file.with_suffix(".partial")
        partial.write_text(json.dumps({"handedness": was}), encoding="utf-8")
        os.replace(partial, self.handedness_file)
        return was

    def restore_handedness(self, bench: Devbench) -> vr.Handedness:
        """Set the handedness handedness_file holds and delete the file; returns it. A set that fails keeps the file."""
        value = vr.check_handedness(json.loads(self.handedness_file.read_text(encoding="utf-8"))["handedness"])
        vr.set_handedness(bench, value)
        self.handedness_file.unlink()
        return value

    def wanted_handedness(self, config: pytest.Config) -> vr.Handedness | None:
        """The handedness the run asks for: "left" with --left-handed, else Config.handedness; None leaves it alone."""
        return "left" if config.getoption("--left-handed") else self.config.handedness

    def pytest_runtest_logreport(self, report: pytest.TestReport) -> None:
        self.record.add(report)

    @pytest.hookimpl(wrapper=True)
    def pytest_runtest_protocol(self, item: pytest.Item) -> Generator[None, object, object]:
        """Keep the record's current_test on the test while it runs, its setup and teardown included."""
        self.record.current_test = item.nodeid.partition("::")[2]
        try:
            return (yield)
        finally:
            self.record.current_test = None

    @pytest.hookimpl(wrapper=True)
    def pytest_runtest_makereport(
        self, item: pytest.Item, call: pytest.CallInfo[None]
    ) -> Generator[None, pytest.TestReport, pytest.TestReport]:
        """
        On the first failed phase of a test that uses the game fixture, while the game is up: take its open menus,
        failure picture and log tail, before its teardown changes what the game shows. The menus also go in a
        "devbench" section, which pytest prints under the failure. A setup failure with the crash of one already
        captured (a failed session fixture, raised again for each test) takes nothing.
        """
        report = yield
        failures = self.record.failures
        if report.failed and self.game is not None and "game" in item.fixturenames and item.nodeid not in failures:
            if not self._repeated_setup_crash(report):
                failure = self._capture(self.game, item.nodeid)
                failures[item.nodeid] = failure
                if failure.menu_lines():
                    report.sections.append(("devbench", "\n".join(failure.menu_lines())))
        return report

    def _repeated_setup_crash(self, report: pytest.TestReport) -> bool:
        """Whether a failed report is a setup failure whose crash (file, line, message) an earlier setup failure had."""
        crash = getattr(report.longrepr, "reprcrash", None)
        if report.when != "setup" or crash is None:
            return False
        key = (str(crash.path), crash.lineno, crash.message)
        if key in self.setup_crashes:
            return True
        self.setup_crashes.add(key)
        return False

    def _capture(self, game: Devbench, nodeid: str) -> Failure:
        """
        What a failed test leaves: the open menus and a message box's text, taken first as a box may close on its own;
        the log lines since it started (when Config.log is set) and a picture of Config.picture. Menus that cannot be
        read say why in the row, and the next failure tries again. The first picture that cannot be taken says why in
        its row and a note, and turns pictures off.
        """
        failure = Failure()
        try:
            listed = game.tool("menu", action="list")
            failure.menus = list(listed["openMenus"])
            if listed.get("messageBoxOpen"):
                try:
                    failure.message_box = message_box_text(game)
                except DevbenchError as error:
                    failure.message_box = f"(not read: {error})"
        except (DevbenchError, OSError, KeyError) as error:
            failure.menus = None
            failure.menus_error = str(error)
        if self.test_mark is not None and self.test_mark[0] == nodeid:
            try:
                failure.log = self.test_mark[1].lines()[-_LOG_TAIL:]
            except (DevbenchError, OSError) as error:
                failure.log = [f"the log could not be read: {error}"]
        if self.config.picture is not None and not self.pictures_off:
            try:
                failure.picture = self._picture(game, nodeid)
            except (DevbenchError, OSError, KeyError, ValueError) as error:
                failure.picture_error = str(error)
                self.pictures_off = True
                self.record.note(f"No failure pictures from {nodeid} on: {error}")
        return failure

    def _picture(self, game: Devbench, nodeid: str) -> str:
        """
        Dump Config.picture, delete the game's copy and save it in the report's pictures folder as a PNG (in VR, only
        the left eye); returns its path in the report folder. The dump lands under the game's Data folder, which the
        files tool reads as "data".
        """
        answer = game.tool("rendertarget", action="dump", index=self.config.picture, maxDimension=_PICTURE_SIZE)
        path = _data_path(answer["file"])
        data = read_bytes(game, "data", path)
        if data is None:
            raise DevbenchError(f"the dump {path} is not in the Data folder the files tool sees")
        delete_file(game, "data", path)
        local = self.record.reserve(nodeid, ".png")
        local.write_bytes(bmp_to_png(data, left_half=self.config.vr))
        return local.relative_to(self.record.folder).as_posix()


_RUN = pytest.StashKey[_Run]()


def _data_path(file: str) -> str:
    """
    A file's path under the game's Data folder, '/' separated, from its absolute path: what follows the last folder
    named Data. ValueError when no folder is.
    """
    parts = PureWindowsPath(file).parts
    data = [i for i, part in enumerate(parts) if part.lower() == "data"]
    if not data:
        raise ValueError(f"the dump {file} is not under the game's Data folder")
    return "/".join(parts[data[-1] + 1 :])


def _report_root(devbench: Config, home: Path) -> Path:
    """
    The folder the report's run folders go in: Config.report_dir (relative: to home), or build/e2e under the first
    folder from home up that holds a .git, or under home when none does.
    """
    if devbench.report_dir is not None:
        return home / devbench.report_dir
    repo = next((folder for folder in (home, *home.parents) if (folder / ".git").exists()), home)
    return repo / "build" / "e2e"


def _find_config(config: pytest.Config) -> tuple[Config, Path]:
    """
    The DEVBENCH object of the one registered module (the rootdir conftest) that defines it, and that module's folder.
    None, several, or one that is not a Config raises pytest.UsageError.
    """
    found = {
        id(plugin): plugin
        for plugin in config.pluginmanager.get_plugins()
        if isinstance(plugin, types.ModuleType) and hasattr(plugin, "DEVBENCH")
    }
    modules = list(found.values())
    if not modules:
        raise pytest.UsageError(
            "devbench.testing: no conftest defines DEVBENCH = devbench.testing.Config(...): define it in the rootdir "
            "conftest, beside pytest_plugins"
        )
    if len(modules) > 1:
        names = ", ".join(str(getattr(module, "__file__", module.__name__)) for module in modules)
        raise pytest.UsageError(f"devbench.testing: DEVBENCH is defined in more than one conftest: {names}")
    module = modules[0]
    devbench = module.DEVBENCH
    if not isinstance(devbench, Config):
        raise pytest.UsageError(
            f"devbench.testing: DEVBENCH in {module.__file__} is a {type(devbench).__name__}, not a "
            "devbench.testing.Config"
        )
    return devbench, Path(module.__file__).resolve().parent


def _handed(handedness: vr.Handedness) -> str:
    return f"{handedness}-handed"


def _run(config: pytest.Config) -> _Run:
    return config.stash[_RUN]


def _exit(message: str) -> NoReturn:
    pytest.exit(message, returncode=_EXIT_CODE)


def _listening(port: int) -> bool:
    """Whether something accepts a connection on the port now."""
    try:
        socket.create_connection((HOST, port), timeout=_CONNECT_TIMEOUT).close()
    except OSError:
        return False
    return True


def _say(config: pytest.Config, line: str) -> None:
    """Write a line to the terminal at once, also from a fixture whose output pytest captures."""
    reporter = config.pluginmanager.get_plugin("terminalreporter")
    if reporter is None:
        return
    capture = config.pluginmanager.get_plugin("capturemanager")
    if capture is None:
        reporter.write_line(line)
        return
    with capture.global_and_fixture_disabled():
        reporter.write_line(line)


def _launch(config: pytest.Config, command: str, port: int | None) -> Devbench:
    """Launch the game with the user's command and wait for it (devbench.process.launch); a failure exits the run."""
    run = _run(config)
    _say(config, "devbench: no game answers: launching it with the launch command")
    start = time.monotonic()
    try:
        bench = launch(command, run.config.game, run.config.vr, port=port)
    except DevbenchError as error:
        _exit(f"launching the game: {error}")
    took = time.monotonic() - start
    _say(config, f"devbench: the game is up after {took:.0f} s")
    run.record.note(f"Launched the game: up after {took:.0f} s.")
    return bench


def _connect(config: pytest.Config) -> Devbench:
    """
    A client on --devbench-port or the port the game's runtime.json names, checked to be the Config's game. When
    nothing listens there and a launch command is set (--devbench-launch, else DEVBENCH_LAUNCH), the game is launched.
    """
    devbench = _run(config).config
    pinned = config.getoption("--devbench-port")
    port = pinned or find_port(devbench.game, devbench.vr)
    command = config.getoption("--devbench-launch") or os.environ.get("DEVBENCH_LAUNCH")
    bench = _launch(config, command, pinned) if command and not _listening(port) else Devbench(port)
    try:
        require(bench, devbench.game, devbench.vr)
    except RequirementError as error:
        bench.close()
        _exit(str(error))
    return bench


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("devbench")
    group.addoption("--devbench-port", type=int, help="devbench's port; by default the one its runtime.json has")
    group.addoption(
        "--e2e-restore",
        action="store_true",
        help="put back the files and the handedness of a backup a run left, and run no test",
    )
    group.addoption("--left-handed", action="store_true", help="run left-handed (VR), overriding Config.handedness")
    group.addoption(
        "--devbench-launch",
        metavar="COMMAND",
        help="when no game answers, run this command to launch one (default: the DEVBENCH_LAUNCH environment variable)",
    )
    group.addoption("--devbench-quit", action="store_true", help="quit the game when the session ends")


def pytest_configure(config: pytest.Config) -> None:
    devbench, home = _find_config(config)
    if config.getoption("--left-handed") and not devbench.vr:
        raise pytest.UsageError("devbench.testing: --left-handed needs a VR game (Config.vr)")
    for area in devbench.area_markers().values():
        config.addinivalue_line("markers", f"{area.marker}: {area.title}")
    record = Record(areas=devbench.area_titles(), title=devbench.title or devbench.tool or devbench.game)
    record.directory = _report_root(devbench, home) / record.started.strftime("%Y%m%d-%H%M%S")
    run = _Run(devbench, home, record)
    config.stash[_RUN] = run
    config.pluginmanager.register(run, "devbench-run")


def pytest_sessionstart(session: pytest.Session) -> None:
    """
    With --e2e-restore: put back the files and the handedness a run left and exit, before any test is collected. A
    handedness that cannot be set (no loaded game) is kept for the next try, and the exit code says so. With
    --devbench-quit the game is quit after, also when putting things back failed.
    """
    config = session.config
    if not config.getoption("--e2e-restore"):
        return
    run = _run(config)
    if not run.backup_pending() and not run.handedness_pending():
        pytest.exit("no backup is waiting to be put back", returncode=0)
    bench = _connect(config)
    said: list[str] = []
    code = 0
    try:
        if run.backup_pending():
            backup = run.backup(bench)
            assert backup is not None  # backup_pending holds only with Config.backup set
            written, deleted = backup.restore()
            said.append(f"put back the backup: {len(written)} files written, {len(deleted)} deleted")
        if run.handedness_pending():
            try:
                said.append(f"put back the handedness: {_handed(run.restore_handedness(bench))}")
            except (DevbenchError, OSError) as error:
                code = _EXIT_CODE
                said.append(f"kept the handedness to put back ({error}): run --e2e-restore again in a loaded game")
    finally:
        if config.getoption("--devbench-quit"):
            said.append(_quit(bench))
        else:
            bench.close()
    pytest.exit("; ".join(said), returncode=code)


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """
    Mark each test with its area's marker, and run the tests in the order of Config.areas (by test-file name); files
    it does not list run last. It runs first, so pytest's -m sees the markers. A test that uses mod without
    Config.tool, or log_mark without Config.log, raises pytest.UsageError before any test runs.
    """
    devbench = _run(config).config
    needs = {"mod": ("tool", devbench.tool), "log_mark": ("log", devbench.log)}
    for name, (field_name, value) in needs.items():
        users = [item.nodeid for item in items if value is None and name in getattr(item, "fixturenames", ())]
        if users:
            raise pytest.UsageError(
                f"devbench.testing: the {name} fixture needs Config.{field_name}, used by: {', '.join(users)}"
            )
    markers = devbench.area_markers()
    for item in items:
        area = markers.get(item.path.name)
        if area is not None and area.marker is not None:
            item.add_marker(area.marker)
    order = list(devbench.areas)
    if order:
        items.sort(key=lambda item: order.index(item.path.name) if item.path.name in order else len(order))


def _quit(bench: Devbench) -> str:
    """
    Quit the game (quit_game, its watchdog armed) and say how that went; a game that does not go is said, not raised.
    """
    try:
        quit_game(bench, timeout=_QUIT_TIMEOUT, kill_after=_QUIT_KILL_AFTER)
    except DevbenchError as error:
        return f"the game did not quit: {error}"
    return "quit the game"


@pytest.fixture(scope="session")
def bench(pytestconfig: pytest.Config) -> Iterator[Devbench]:
    """
    The running game's devbench, checked to be the Config's game (and VR), launched first when none answers and a
    launch command is set; anything else exits the run. With --devbench-quit, the game is quit at the end, after the
    game fixture's teardown.
    """
    bench = _connect(pytestconfig)
    yield bench
    if pytestconfig.getoption("--devbench-quit"):
        said = _quit(bench)
        _run(pytestconfig).record.note(said[0].upper() + said[1:] + ".")
    else:
        bench.close()


def _setup_step(what: str, step: Callable[[], Any]) -> Any:
    """Run one step of the game's setup; a failure exits the run with what failed and why."""
    try:
        return step()
    except RequirementError as error:
        _exit(str(error))
    except (DevbenchError, TimeoutError, OSError) as error:
        _exit(f"{what}: {error}")


def _check_mod(bench: Devbench, devbench: Config, run: _Run) -> None:
    """The mod's tool is listed, offers the Config's actions and reports the checkout's version."""
    assert devbench.tool is not None
    mod = ModTool(bench, devbench.tool)
    require_tools(bench, [devbench.tool])
    health = require_actions(mod, devbench.actions)
    expected = devbench.expected_version()
    if expected is not None:
        health = require_version(mod, expected)
    if "version" in health:
        run.record.note(f"{devbench.title or devbench.tool} {health['version']}.")


def _check_dependency(bench: Devbench, dependency: Dependency, run: _Run) -> None:
    """Another mod's tool is listed, offers the actions the suite calls and runs at least its minimum version."""
    mod = ModTool(bench, dependency.tool)
    require_tools(bench, [dependency.tool])
    health: dict[str, Any] = {}
    if dependency.actions:
        health = require_actions(mod, dependency.actions)
    if dependency.min_version is not None:
        health = require_min_version(mod, dependency.min_version)
    if "version" in health:
        run.record.note(f"{dependency.tool} {health['version']}.")


@pytest.fixture(scope="session")
def game(bench: Devbench, pytestconfig: pytest.Config) -> Iterator[Devbench]:
    """
    The game the tests run in: a leftover backup put back, the game entered in Config.cell and settled, the mod
    checked, a leftover handedness put back, the files of Config.backup backed up, the run's handedness set and the
    log mark set. The handedness, then the backup, are put back at the end, also when the session fails; a crash
    leaves them for the next run or --e2e-restore.
    """
    run = _run(pytestconfig)
    devbench = run.config
    record = run.record
    backup = run.backup(bench)
    if backup is not None and backup.pending():
        written, deleted = _setup_step("putting back the backup an earlier run left", backup.restore)
        record.note(f"Put back the backup an earlier run left: {len(written)} files written, {len(deleted)} deleted.")

    enter = _ENTER.get(devbench.game)
    if enter is None:
        _exit(f"devbench.testing has no game setup for {devbench.game}")
    entered = _setup_step(f"entering the game in {devbench.cell}", lambda: enter(bench, devbench.cell, devbench.cells))
    record.dialogs.extend(entered.dialogs)
    if entered.started:
        record.note(f"Started a new game in {entered.cell} from the main menu.")
    elif entered.cell is None:
        record.note("Ran in the game already loaded.")
    else:
        record.note(f"Ran in the game already in {entered.cell}.")
    if entered.quiet is False:
        record.note("The game still popped up dialogs at the settle limit: the tests started anyway.")

    if devbench.tool is not None:
        _setup_step(f"checking {devbench.tool}", lambda: _check_mod(bench, devbench, run))
    for dependency in devbench.requires:
        _setup_step(
            f"checking {dependency.tool}", lambda dependency=dependency: _check_dependency(bench, dependency, run)
        )
    if devbench.ready is not None:
        assert devbench.tool is not None  # Config checks that ready comes with tool
        ready, mod = devbench.ready, ModTool(bench, devbench.tool)
        _setup_step(
            f"waiting for {mod.name} to be ready",
            lambda: wait_until(lambda: ready(mod), devbench.ready_timeout, _READY_INTERVAL, f"{mod.name} ready"),
        )

    if run.handedness_pending():
        value = _setup_step("putting back the handedness an earlier run left", lambda: run.restore_handedness(bench))
        record.note(f"Put back the handedness an earlier run left: {_handed(value)}.")

    files = _setup_step("taking the backup", backup.take) if backup is not None else 0
    try:
        wanted = run.wanted_handedness(pytestconfig)
        if wanted is not None:
            _setup_step("reading the player's handedness", lambda: run.keep_handedness(bench))
            _setup_step(f"setting the game {_handed(wanted)}", lambda: vr.set_handedness(bench, wanted))
            run.handedness = wanted
            record.note(f"Ran {_handed(wanted)}.")
        if devbench.log is not None:
            log = devbench.log
            run.mark = _setup_step(f"marking {log}", lambda: LogMark(bench, log))
        run.game = bench
        yield bench
    finally:
        run.game = None
        unrestored: Exception | None = None
        if run.handedness_pending():
            try:
                record.note(f"Put the player's handedness back: {_handed(run.restore_handedness(bench))}.")
            except Exception as error:
                unrestored = error
        if backup is not None:
            try:
                written, deleted = backup.restore()
            except Exception as error:
                raise RuntimeError(
                    f"the backup was not put back: {error}: run pytest --e2e-restore once the game answers"
                ) from error
            record.note(
                f"Backed up {files} files, and put them back after: {len(written)} written again, "
                f"{len(deleted)} the run made deleted."
            )
        if unrestored is not None:
            raise RuntimeError(
                f"the handedness was not put back: {unrestored}: run pytest --e2e-restore once the game answers"
            ) from unrestored


@pytest.fixture(scope="session")
def mod(game: Devbench, pytestconfig: pytest.Config) -> ModTool:
    """The mod's own devbench tool (Config.tool), checked by the game fixture."""
    tool = _run(pytestconfig).config.tool
    if tool is None:
        pytest.fail("the mod fixture needs Config.tool", pytrace=False)
    return ModTool(game, tool)


@pytest.fixture
def events(game: Devbench, pytestconfig: pytest.Config) -> Events:
    """The events published from the start of the test; with Config.tool, the mod's topics match unprefixed."""
    return Events(game, prefix=_run(pytestconfig).config.tool)


@pytest.fixture(scope="session")
def log_mark(game: Devbench, pytestconfig: pytest.Config) -> LogMark:
    """The mark in Config.log the game fixture set, just before the first test."""
    mark = _run(pytestconfig).mark
    if mark is None:
        pytest.fail("the log_mark fixture needs Config.log", pytrace=False)
    return mark


@pytest.fixture(autouse=True)
def devbench_test_mark(request: pytest.FixtureRequest) -> None:
    """
    For a test that uses the game fixture, with Config.log set: a mark in the log as the test starts, which a failure's
    log tail reads from. A mark that cannot be taken leaves the failure without a tail.
    """
    run = _run(request.config)
    run.test_mark = None
    log = run.config.log
    if log is None or "game" not in request.fixturenames:
        return
    game = request.getfixturevalue("game")
    try:
        run.test_mark = (request.node.nodeid, LogMark(game, log))
    except (DevbenchError, OSError):
        pass


@pytest.fixture(scope="session", name="handedness")
def handedness_fixture(game: Devbench, pytestconfig: pytest.Config) -> vr.Handedness:
    """The run's handedness: what the game fixture set (--left-handed, Config.handedness), else the game's."""
    run = _run(pytestconfig)
    return run.handedness if run.handedness is not None else vr.handedness(game)


# named so the package's report attribute stays the devbench.testing.report module
@pytest.fixture(scope="session", name="report")
def report_fixture(pytestconfig: pytest.Config) -> Record:
    """The run's record, which a test adds notes, sections and pictures to."""
    return _run(pytestconfig).record


def pytest_terminal_summary(terminalreporter: pytest.TerminalReporter, config: pytest.Config) -> None:
    """Write the report and print its link, unless the run collected only, restored, or has no result."""
    run = config.stash.get(_RUN, None)
    if run is None or config.getoption("collectonly") or config.getoption("--e2e-restore") or not run.record.results:
        return
    terminalreporter.write_line(f"Report: {write(run.record).as_uri()}", bold=True)
