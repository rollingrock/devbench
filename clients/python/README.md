# devbench-client

Python client for devbench's REST port: call a running game's tools from a script, a REPL or a test. Import package
`devbench`; it never imports pytest. MIT licensed (see [LICENSE](LICENSE)), unlike the GPL plugin.

## Install

From a Fallout release tag, in a mod's `requirements.txt`:

```
devbench-client @ git+https://github.com/ArthurHub/devbench@fo4-v1.32.0#subdirectory=clients/python
```

The package's version is the tag's (`1.32.0`), the same version the matching devbench DLL reports in `ping`.

From a local checkout, editable, when working on devbench and a mod together:

```
pip install -e <devbench>/clients/python
```

## Use

```python
from devbench import Devbench, ModTool, find_port

with Devbench(find_port("fallout4", vr=True)) as bench:
    print(bench.health())
    print(bench.tool("console", command="GetInCell QASmoke"))
    print(ModTool(bench, "immersiveflashlightvr").health())
```

Check that the running game is the one a run needs before using it. Each check raises `RequirementError` with a
message that says what was found and what to do ("deploy this build"):

```python
from devbench import require, require_actions, require_tools, require_version

require(bench, "fallout4", vr=True)  # the ping answer; no answer or another game raises
require_tools(bench, ["frik"])  # from devbench's tool listing (bench.tools())
mod = ModTool(bench, "immersiveflashlightvr")
require_actions(mod, ["gesture", "zones", "light"])  # from the mod tool's health
require_version(mod, "1.4.2")  # exact match with the health's version; returns the health
```

Read what an action caused from devbench's events. `Events` sees only events published after it was made; with a
prefix (a mod's tool name) a topic also matches as `<prefix>.<topic>`:

```python
from devbench import Events

mod = ModTool(bench, "immersiveflashlightvr")
events = Events(bench, prefix=mod.prefix)
mod.call("gesture", gesture="toggle")
event = events.wait("gesture", timeout=3.0, where=lambda data: data["gesture"] == "toggle")
print(len(events.all("gesture")), event["seq"])
```

`wait` raises `TimeoutError` naming the topic and the topics that arrived. A read raises `DevbenchError` when events
were lost (more than the 256-event ring between two reads) or the game restarted.

Run console commands and handle menus with free functions that take the client:

```python
from devbench import close_dialogs, close_menus, console, console_number, message_box_text, open_menus

console(bench, "player.GetPos z")  # the lines it printed
console_number(bench, "player.GetInCell QASmoke")  # 1.0: the number after its '>>'
"HUDMenu" in open_menus(bench)
message_box_text(bench)  # "header body" of the open box; DevbenchError when none can be read
said = close_dialogs(bench)  # answer each message box with its first button
close_menus(bench, ["PauseMenu"])  # a refused close is ignored
```

A command that does not compile raises `DevbenchError`, and so does `console_number` when the output holds no number.
`close_dialogs` returns what each box said and gives up after `limit` (20) answers.

Read and write files as the game sees them, through devbench's `files` tool. `root` is `"data"` (the Data folder,
mod manager's virtual files included) or `"mygames"` (Documents/My Games/<game>):

```python
from devbench import Backup, delete_file, list_files, read_bytes, read_text, write_bytes, write_text

[f["path"] for f in list_files(bench, "data", "MCM/Settings/*.ini")]  # glob: '*', '?', '**'; case ignored
read_text(bench, "data", "MCM/Settings/MyMod.ini")  # None when there is no file; UTF-8, else cp1252
write_text(bench, "mygames", "Mods_Config/MyMod/MyMod.toml", "key = 1\n")
read_bytes(bench, "data", "x.bin"), write_bytes(bench, "data", "x.bin", b"\x01"), delete_file(bench, "data", "x.bin")
```

A listing that is not whole (the walk failed partway, or more than 10000 files) raises `DevbenchError`.

`Backup` saves the files of some globs before a run changes them and puts them back after:

```python
backup = Backup(bench, [("mygames", "Mods_Config/MyMod/*")], "tests/e2e/.backup")
if backup.pending():  # a run that a crash ended
    backup.restore()
backup.take()  # the number of files saved
...
written, deleted = backup.restore()  # "root/path" lists
```

The manifest (`<folder>/manifest.json`) is written whole before `take` returns, so it survives a crash. `restore`
writes back each file that differs, deletes each file the globs list that the manifest lacks (case ignored), then
removes the manifest. `take` raises while a backup is pending; `restore` raises when none is.

Read what a plugin logged after a point, from its log in the extender's log folder:

```python
mark = LogMark(bench, "MyMod.log")  # the line count now; a missing file raises DevbenchError
...
mark.lines()  # the lines written since the mark
mark.errors()  # those that are error or critical lines; errors(pattern=r"^\S+ debug ") for others
mark.warnings()  # those that are warning lines
```

`errors` matches the mods' `<time> error <msg>` format and devbench's own `[<time>] [E] <msg>`, and `warnings` their
`<time> warning <msg>` and `[<time>] [W] <msg>`; `pattern` is a regex string or compiled pattern, found with
`re.search`. A log shorter than the mark was restarted with the game, and all of
it counts as new. The log tool returns at most the last 5000 lines, so more than that returns the last 5000 and
`mark.missed` says how many were not read.

Wait for a value the game reports to satisfy a condition, with the last value read in the error:

```python
from devbench import wait_for, wait_until

wait_until(lambda: "HUDMenu" in open_menus(bench), 30, what="the HUD")  # the first true value
light_on = lambda s: s["light"]["on"]
state = wait_for(lambda: mod.call("state"), light_on, 3, what="the light on", describe=lambda s: s["light"])
```

Both poll every `interval` (0.1 s) and raise `TimeoutError` after `timeout` seconds. `wait_for`'s error ends with the
last value through `describe`, or its `repr` cut to 500 characters (also when `describe` raises).

Drive the VR hands (Fallout 4 VR) through devbench's `input` pose: it holds the HMD and both controllers, places a
hand's wand at a world point and presses buttons on top, and the game sees real devices:

```python
from devbench import Hands

hands = Hands(bench, "mymod-e2e")  # the owner of the hold; hold_ms= for a hold longer than devbench's 60 s
if hands.ready():  # the vrTrackedSet capability: OpenVR found, hooks in place
    with hands:  # starts in the ready pose; always stops, handing the devices back
        answer = hands.place("left", [x, y, z], pitch=10, yaw=10)  # devbench's answer: corrections, hands
        hands.place("offhand", center, head_offset=(5, 0, -3), yaw_in=25)  # mirrors for either hand
        hands.turn("offhand", pitch=-70)  # points the controller down, its wand where it was
        hands.head("down")  # "ready", "down" or {pitch?, yaw?, lookAt?, ...}
        hands.tap("left")  # press("left", ["trigger"], 100); returns once released
        hands.long_press("right", ["grip"])  # 1000 ms
        hands.stick("primary", 0, -1, 600)  # thumbstick down 600 ms, back to center; the pose stays
        hands.press(("primary", "offhand"), ["thumbstick"], 1500)  # both hands together, one wait
        hands.rest()  # hands and head back in the ready pose
        hands.pose(hands={"right": {"node": "LArm_Hand", "front": 10}})  # any other pose arguments
```

A hand is a physical controller (`left` / `right`) or a role (`primary` / `offhand`), which devbench resolves against
the game's handedness when the call arrives; any other name raises `ValueError`. `pitch` and `yaw` pass through as the
`input` tool takes them. `head_offset` (outward, forward, up) moves the point along the head's axes as posed, outward
toward the hand's own side, and `yaw_in` replaces `yaw` with a turn toward the body (in tracking space, mirrored around
the body's forward), so one recorded placement fits either hand and either handedness. `TAP_MS` (100) and `LONG_PRESS_MS` (1000) are F4VR-CommonFramework's gesture thresholds.
`turn` changes only how a controller points: it places the hand at the last pose's target for it (else where its wand
was read, kept in `hands.placed` per physical hand) with the given `pitch` / `yaw`, the other angle unchanged; a role
costs one handedness call, and a turn before any pose raises `RuntimeError`. `press`, `stick`, `tap` and `long_press`
take a list of hands to press them together (each hand has its own press slot in devbench), checked before any is sent.
A press without a hold of this owner (it never started, or the hold ran out) raises `DevbenchError` (409). `stop`
ignores a refused stop, and on an exception inside the `with` it never hides that exception.

Read and set the game's handedness, the controller that is the primary hand: `"left"` or `"right"` (the
`vr.Handedness` type; `bLeftHandedMode:VR` on Fallout 4 VR):

```python
from devbench.vr import handedness, set_handedness

was = set_handedness(bench, "left")  # the one before; answers once the game's wands have swapped
assert handedness(bench) == "left"
set_handedness(bench, was, owner="mymod-e2e")  # under that owner's pose hold, when it runs one
```

devbench checks the swap under the pose hold of `owner` when it runs one, else under a short hold of its own. Another
sequence holding the devices (409) or a game that did not follow (504, the old value put back) raise `DevbenchError`.

Put Fallout 4 (or VR) in a test cell, so nothing a run does reaches a save:

```python
from devbench.games import fallout4

entered = fallout4.enter_game(bench)  # cell="QASmoke"; Entered(started, dialogs, quiet, cell)
fallout4.in_cell(bench, "QASmoke")  # False at the main menu
fallout4.settle(bench, quiet=10, limit=120)  # Settled(dialogs, quiet)
```

From the main menu `enter_game` runs `coc <cell>`, waits up to `timeout` (300 s) for the HUD with no main menu or
loading screen, then settles: it waits until no message box has popped up for `quiet` seconds, answering each with its
first button, and gives up after `limit` seconds with `quiet` False. A game already in the cell is reused
(`started` False, `quiet` None); a game in another cell, or a load in progress, raises `RequirementError`. A suite
that needs no particular cell passes `cells`: a game already in the cell or in one of `cells` is reused (`Entered.cell`
says which), and `cells=fallout4.ANY_CELL` reuses a loaded game whatever its cell, which can reach a save. `cell` is
still where a game started from the main menu goes. A `coc`
that a modal blocked raises `DevbenchError`. The cell is an interior cell's editor ID; an unknown one raises
`DevbenchError`. Nothing is printed: `dialogs` holds what each closed box said.

Set up what the player wears and holds. Items go through the console; the weapon and power armor go through
devbench's `player` tool, which waits for the engine state:

```python
fallout4.equip(bench, "0000463F")  # adds one first when the player has none
fallout4.unequip(bench, "0000463F")
fallout4.item_count(bench, "0000463F")
fallout4.draw_weapon(bench, timeout=10)  # what is equipped, fists when nothing
fallout4.sheathe_weapon(bench)

frame = fallout4.nearest_power_armor(bench, radius=500)  # { formId, baseFormId, name, distance } or None
fallout4.enter_power_armor(bench, form_id=frame["formId"])  # at once: setup, no animation
fallout4.exit_power_armor(bench)
fallout4.step_into_power_armor(bench, form_id=frame["formId"])  # VR: aim and press A, the game's own walk-in
fallout4.step_out_of_power_armor(bench)  # VR: the exit gesture
fallout4.leave_power_armor(bench)  # the exit, or stepping out when a mod refuses it
```

Each switch returns the `player` tool's answer when its outcome is `switched` or `already`; any other outcome
(`refused`, `notAimed`, ...) raises `DevbenchError` with the outcome, the weapon or power armor state and the tool's
note, and the answer on `error.answer`. `equip` and `unequip` return once the command ran: wait on what your mod sees.

Launch, quit and restart the game. The client knows nothing about MO2, Steam or SteamVR: `launch` runs the command you
give it, the same one you would type in a terminal:

```python
from devbench import launch, quit_game, restart

MO2 = r'"C:\Games\MO2\ModOrganizer.exe" -p Default run -e "Fallout 4 VR (F4SEVR)"'
bench = launch(MO2, "fallout4", vr=True)  # timeout=300; returns a client once the game is up
quit_game(bench, kill_after=60)  # the quit tool, then waits for the process to exit
bench = restart(bench, MO2, "fallout4", vr=True)  # quit_game, then launch
```

`launch` starts the command detached (stopping the script leaves the game running), then waits for devbench to answer
as that game and, on Fallout 4, for the main menu (or a game a save loaded at start). A launcher that hands off and
exits is fine: an MO2 already open takes the `run` and the new `ModOrganizer.exe` exits. A port that already accepts a
connection launches nothing and raises `DevbenchError`; so does a timeout, saying what it last saw (nothing listening,
another game, the menus open while not ready) and the launcher's exit code. MO2's `run` needs `-e` to run one of its
executables by its title; without it, the title is taken for a file.

`quit_game` waits for the game's process to exit (the pid from the `quit` tool or health), not just for the port to
close. `kill_after` arms the tool's watchdog, which terminates a game whose shutdown hangs. `wait_gone(port, pid)`
waits for a game you quit some other way.

## pytest plugin

`devbench.testing` runs a mod's end-to-end suite in the game: it connects, enters a game in the test cell, checks the
mod, backs up the files a run changes and puts them back after. It is opt-in (no entry point), so it never loads into
an unrelated pytest run. The mod's **rootdir** conftest (the one beside its `pytest.ini`; pytest honors
`pytest_plugins` nowhere else) turns it on and configures it:

```python
import devbench.testing

pytest_plugins = ["devbench.testing"]
DEVBENCH = devbench.testing.Config(
    game="fallout4",
    vr=True,
    tool="immersiveflashlightvr",  # the mod's devbench tool; None skips the mod check
    actions=["gesture", "zones", "light"],  # the actions the tests call
    version=lambda: read_checkout_version(),  # a string or a callable; None skips the version check
    backup=[("mygames", "Mods_Config/ImmersiveFlashlightVR/*")],  # (root, glob) pairs; none: no backup
    log="ImmersiveFlashlightVR.log",  # for the log_mark fixture
    areas={  # run order, by test-file name: a title, or an Area with its -m marker
        "test_light.py": devbench.testing.Area("Light action", marker="light"),
        "test_health.py": "Health",
    },
    requires=[devbench.testing.Dependency("frik", actions=["probe"], min_version="0.79")],  # other mods' tools
    ready=lambda mod: mod.call("state")["ready"],  # polled on the mod's tool before the first test
    ready_timeout=30.0,
    title="Immersive Flashlight VR",
    cell="QASmoke",  # the interior cell a game started from the main menu goes to
    cells=None,  # more cells a loaded game may be in: ["CTest"], or fallout4.ANY_CELL for whatever cell it is in
    picture=6,  # the render target dumped when a test fails (FO4VR: 6, the lit frame); None: no picture
    report_dir=None,  # where the report's run folders go; None: build/e2e under the repo root
    handedness="right",  # VR: "left" / "right" for the run, the player's put back after; None: leave it
)
```

Fixtures:

| Fixture                | Gives                                                                                                       |
| ---------------------- | ----------------------------------------------------------------------------------------------------------- |
| `bench` (session)      | the `Devbench`, checked to be the configured game and VR; connects only when a test needs it                |
| `game` (session)       | the `Devbench`, once the game is set up (below); the backup is put back at the end, also when the run fails |
| `mod` (session)        | `ModTool` of `tool`                                                                                         |
| `events`               | `Events` from the start of the test, with the mod's tool as prefix                                          |
| `log_mark` (session)   | the `LogMark` in `log`, set just before the first test                                                      |
| `handedness` (session) | the run's handedness (`"left"` / `"right"`): what `game` set, else the game's own                           |
| `report` (session)     | the run's record, which the HTML report renders (below)                                                     |

`game` puts back a backup a crashed run left, runs `enter_game(cell, cells=cells)`, checks the mod (`require_tools`, then
`require_actions`, then `require_version`) and each of `requires` (`require_tools`, its `actions`, then
`require_min_version`, which compares dotted numbers: `0.79.2-beta` is 0.79.2), polls `ready` with the mod's `ModTool`
until it returns a true value, puts back a handedness a crashed run left, takes the backup into `.backup/` beside the
conftest, sets the run's handedness, and sets the log mark. The report notes each version it checked.
A setup failure (another game, a missing tool or action, another build, a required mod too old, a mod not ready
within `ready_timeout`, a game outside the accepted cells) exits the run with code 2 and the message that says what to do.

Tests run in `areas` order; files it does not list run last. An `Area`'s marker is registered (it passes
`--strict-markers` and shows in `--markers`) and put on every test of its file, so `pytest -m light` runs that area
with no `pytest.ini` marker list and no `pytestmark` line. `--devbench-port` picks the port (default: the game's
`runtime.json`). `--e2e-restore` puts back a backup a run left and runs no test; a restore that fails at the end of a
run keeps the backup for it.

When no game answers and a launch command is set, `bench` launches the game (`launch`) instead of exiting. The command
is your setup (MO2 instance, profile, paths), not the mod's, so it never goes in the conftest: pass
`--devbench-launch "<command>"` or set `DEVBENCH_LAUNCH` (the option wins). The report notes how long the game took
to come up. `--devbench-quit` quits the game when the session ends (after the files and the handedness are put back,
also after a setup failure), with a 60 s watchdog for a hung shutdown, and notes it. An `--e2e-restore` run quits it
too, once the files and the handedness are put back. Together they make an unattended
run from a closed game to a report:

```powershell
$env:DEVBENCH_LAUNCH = '"C:\Games\MO2\ModOrganizer.exe" -p Default run -e "Fallout 4 VR (F4SEVR)"'
pytest tests/e2e --devbench-quit
```

With `handedness` (or `--left-handed`, which overrides it) `game` writes the player's handedness to
`.backup/handedness.json`, sets the run's with `set_handedness` (devbench answers once the game's wands have swapped)
and notes "Ran left-handed." / "Ran right-handed." in the report. At the end it puts the player's back, before the
files, also when the run fails, and deletes the file. A file a crashed run left is put back at the next run's start or
by `--e2e-restore`; one `--e2e-restore` cannot set (no loaded game) is kept, and it exits with code 2.

A mod's own session setup wraps `game`: a fixture of the same name in its conftest requests the plugin's. Its setup runs
after the backup, the handedness and the log mark (so it can wait for its mod to follow the handedness), its teardown
before the handedness and the backup are put back:

```python
@pytest.fixture(scope="session")
def game(game, handedness, report):
    wait_until(lambda: MyMod(game).handedness() == handedness, 5, what="the mod following the handedness")
    yield game
    MyMod(game).clear()
```

### Report

At the end of a run the plugin writes one HTML page and prints its link: `build/e2e/<YYYYmmdd-HHMMSS>/index.html`
under the first folder up from the `DEVBENCH` conftest that holds a `.git` (`report_dir` overrides
`<repo>/build/e2e`; relative to the conftest's folder). It shows the totals, the notes, the failures, each test's
result by area in `areas` order, the mod's sections, the game dialogs the run closed and a gallery of the mod's
pictures. Nothing is written for `--collect-only`, `--e2e-restore` or a run without results.

When a test that uses `game` fails, the failure row gets:

- the open menus and, with a message box up, its text (read first, before the box may close; a read that fails says
  why in the row). They also print under the failure in the terminal, as a `devbench` section;
- the lines `log` gained since the test started (the last 100, collapsed);
- a picture of render target `picture` (`rendertarget dump` at 2048, the game's copy deleted, saved beside the page as
  PNG; in VR only the left eye, 1024x1088 on FO4VR). The first picture that can't be taken (a 501 off the main thread,
  a wrong index) says why in its row and in a note, and no later failure tries again.

A test adds to the page through the `report` fixture:

```python
def test_reach(game, report, request):
    report.note("Ran right-handed.")  # a line about the whole run
    report.section("Placements", intro="each hand placed", columns=["Test", "Landed"])
    report.section("Placements", rows=[[request.node.name, "1.2 cm"]])  # the same title adds rows
    report.section("Perf", html="<pre>...</pre>")  # raw HTML instead of a table
    path = report.picture(request.node.nodeid, label="the panel")  # a file for the mod to write, shown if it does
    (report.folder / "perf.json").write_text("{}")  # a file beside the page
```

`report.current_test` is the running test's name (`test_reach[left]`, `TestGroup::test_reach`) from its setup to its
teardown, and None between tests, so a mod's helper can tag a row with it without a `request`.

Sections show after the areas, in the order first added; a section is a table or HTML, and a call of the other shape,
other columns or a row of another length raises `ValueError`. `devbench.testing.report.render(record)` renders a
`Record` without a pytest session.

## Develop

```
pip install -e clients/python
pytest clients/python
```
