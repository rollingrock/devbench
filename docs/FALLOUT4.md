# devbench for Fallout 4 / Fallout 4 VR

One F4SE plugin that opens a local MCP + REST endpoint into a **running** Fallout 4, so an
AI agent, a test script, or you with `curl` can drive and measure the game without
alt-tabbing — and, on Fallout, without attaching a debugger to read engine memory.

The same DLL serves **Fallout 4** and **Fallout 4 VR**. It is the Fallout platform of
[devbench](../README.md); the tool surface, the transports, and the config file are shared
with the Skyrim build. See [MULTIGAME.md](MULTIGAME.md) for how the two fit together.

> **Status: runs on Fallout 4 VR; flat Fallout 4 not yet run.** On FO4VR the server starts,
> the bridge keeps one session across game restarts, and `console`, `menu`, `inspect`,
> `nodes` and `memory` have answered live requests. Two mods, True Scopes and FRIK, register
> their own tools through the C-ABI. **Main-thread tasks run on the main thread on FO4VR
> 1.2.72**, through devbench's own queue after `Main::Swap`; F4SE's queue had been running
> them on worker threads (see "Main thread" below). `rendertarget` `list`/`stats`/`dump`
> ran live on 1.2.72 (2026-10-28): 13 readbacks in one run, among them a 1024² target with
> True Scopes drawing into it, all on the main thread and none stalling the game. Earlier
> readbacks from worker threads hung the game and crashed the driver. On any other runtime
> `stats`/`dump` answer 501. `input` (`device='vrTrackedSet'`: `sequence`, `press`, `pose`)
> drove the HMD and both controllers live on 1.2.72 (2026-11-10/11), down to a mod's hand
> gestures (see "`input`, the short version"); `pose` replaced Skyrim's calibrated `reach`
> there. `player` moved the player in and out of power
> armor live the same day, and drew and sheathed the weapon (a gun and bare fists) live on
> 2026-11-11. `quit` quit FO4VR from the main menu and from in game on 2026-11-16 (the
> process gone 15 s and 3 s after the call); the queued `qqq` quit it from in game the same day. `measure`, `log` and `files` have no recorded
> live run yet. Flat Fallout 4 uses the same DLL but has never been loaded.

## Install

Install the `devbench-fo4-vX.Y.Z.7z` release archive as a mod (see [Releasing](#releasing)),
or drop a built `devbench.dll` in `Data/F4SE/Plugins/`. On first run it writes a self-documenting
`Data/F4SE/Plugins/devbench/config.json`. The keys are the same as Skyrim's. One of them,
`alwaysActive`, is not implemented on Fallout yet: setting it logs a warning and does
nothing.

Default ports — distinct per game _and_ runtime, so Skyrim and Fallout can be up at the
same time without either moving:

|           | flat     | VR       |
| --------- | -------- | -------- |
| Fallout 4 | **8930** | **8931** |
| Skyrim    | 8920     | 8921     |

If the port is busy the server iterates upward and writes the port it actually bound to
`Data/F4SE/Plugins/devbench/runtime.json`.

## Build

Install **Visual Studio 2022 or 2026** with the **Desktop development with C++**
workload (including a Windows SDK), and set `VCPKG_ROOT` to your vcpkg checkout.
Choose the matching configure and build preset:

| Preset            | Toolset | Minimum CMake | Build directory |
| ----------------- | ------- | ------------- | --------------- |
| `fallout4-vs2022` | v143    | 3.23          | `build/vs2022`  |
| `fallout4-vs2026` | v145    | 4.2           | `build/vs2026`  |

`fallout4` is an alias for `fallout4-vs2026`. Each Visual Studio version has its own
build directory, so switching presets does not require clearing the cache.

```powershell
git clone --recursive <your fork>
$env:CommonLibF4Path = "C:\path\to\CommonLibF4"   # or leave it; extern/CommonLibF4 is a submodule
# Choose fallout4-vs2022 or fallout4-vs2026 for both commands.
cmake --preset fallout4-vs2026
cmake --build --preset fallout4-vs2026 --target devbench
```

Needs the **VR-capable** [rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4);
the upstream fork builds but cannot load in Fallout 4 VR. `vcpkg` supplies the rest.

For Ninja, use `fallout4-ninja` for both configure and build from the x64 developer
shell for your chosen Visual Studio version. If switching that shell between versions,
run `cmake --fresh --preset fallout4-ninja` to reset its shared compiler cache.

To deploy on build, set `FalloutPluginTargets` to one or more game `Data` directories,
separated by `;` — the twin of the Skyrim build's `SkyrimPluginTargets`.

## Releasing

A Fallout release uses the version already in `CMakeLists.txt` and is tagged `fo4-vX.Y.Z`;
the prefix only keeps the tag apart from the Skyrim release's `vX.Y.Z`. The two pipelines are
independent: `release-fallout.yaml` runs only on a `fo4-v*` tag push, Skyrim's `release.yaml`
only on its own repository's `main`.

1. Get the release commit onto `main`.
2. Optionally, trial-build it first: `gh workflow run _build-fallout.yaml`.
3. Tag and push, with X.Y.Z the `set(VERSION ...)` in `CMakeLists.txt`:
   `git tag -a fo4-vX.Y.Z -m "<release notes>" && git push origin fo4-vX.Y.Z`.
   The tag's message becomes the release notes; a lightweight tag gets the commit subjects
   since the previous `fo4-v` tag instead.
4. `release-fallout.yaml` checks the tag (`fo4-vX.Y.Z`, on `main`, X.Y.Z the `CMakeLists.txt`
   version), builds with the `fallout4-vs2026` preset, checks the Python client resolves the
   same version, and creates a **draft** GitHub Release. Review the draft and publish it.

The archive, `devbench-fo4-vX.Y.Z.7z`, has `F4SE/` at its root so MO2 installs it as a mod:
`F4SE/Plugins/devbench.dll` and `.pdb`, and `F4SE/Plugins/devbench/devbench-bridge.exe`.
One archive serves Fallout 4 and Fallout 4 VR.

**The version is never bumped for a Fallout release.** It is also `GetBuildNumber()`
(`MAJOR*10000 + MINOR*100 + PATCH`), which mods compare against `10500`, `12000`, ... before
calling late API slots, so it moves only with the version files (`CMakeLists.txt`,
`vcpkg.json`, `xmake.lua`) as upstream bumps them. A Fallout release that needs a new number
waits for that bump. `ping` and the load log always show the file's version; the Python
package (`devbench-client`, setuptools-scm) shows the tag's on the tag and
`X.Y.(Z+1).devN+g<hash>` past it.

## Connect an MCP client

Either connect straight to the game's streamable-HTTP endpoint:

```sh
claude mcp add --transport http --scope user devbench-fo4vr http://127.0.0.1:8931/mcp
```

or go through the [stdio bridge](../bridge/README.md) with `--game fo4vr` (`fo4` for flat).
A direct connection dies with the game and needs a reconnect after every relaunch. The
bridge keeps the session alive across restarts, answers `game not running` while the game
is down, and lists Fallout's tools even before the game starts. The Fallout build does not
ship `devbench-bridge.exe`; it is the same exe for every game, or run it from source:

```sh
claude mcp add --scope user devbench-fo4vr -- <node.exe> <repo>/bridge/dist/index.js --game fo4vr
```

## Tools

Reachable over both MCP (`tools/call` on `/mcp`) and REST (`POST /api/tool/<name>`).

| Tool           | What it does                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ping`         | Self-test. Returns `{ ok, game, exe, vr }`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `inspect`      | Live game state, on the main thread, returned synchronously. `kind='state'` → `{ plugin, version, playerLoaded, frame, pid, port, exe, vr, game, extender }`. `'health'` → the same identity plus `{ frame, lastTaskFrame, pendingTasks, blocking }`, and it is the **only** kind answered _without_ the main thread — so it still replies when a busy or hung main thread would 504. `'ui'` → `{ openMenus, messageBoxOpen, blocking }`, **also** answered without the main thread, for the same reason: `blocking` has to be trustworthy exactly when the game is stuck behind a modal, not just when it's healthy. `'scene'` → `{ position, cell, cellFormId, interior, worldspace?, gameHour, daysPassed }`. `'player'` → `{ formId, level, name }`. `'registrants'` → who requested the C-ABI interface and what they registered; `'extensions'` → kinds other plugins added, and `kind=<registered>` dispatches to that plugin's handler (see "Registering your own tools"). |
| `console`      | Runs a console command and returns what it printed: `{ executed, command, compiled, captured, count, lines, blocked }`. Several commands separated by `;` run in order. `capture=false` queues the line exactly as if typed, with no output. `blocked` is true if a modal was open when the command was submitted, since many state-machine commands (`coc`, …) silently no-op behind one instead of erroring. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `menu`         | Detect and answer in-game menus — mirrors the Skyrim `menu` tool's shape. `action='list'` (default) → `{ openMenus, messageBoxOpen }`. `'describe'` → the active `MessageBoxMenu`'s `{ headerText, bodyText, buttons, modal }`. `'accept'` (`index`, default 0) → answers it by button index and closes it. `'open'`/`'close'` (`name`) → show/hide an engine menu via the UI message queue.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| `nodes`        | Walk the live scene graph: `roots`, `tree`, `find`, `get`, `geometry`. Every node carries its real class (from RTTI) and an address that pastes into `memory`. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `memory`       | Read / resolve / write process memory by address expression. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `log`          | `action='tail'` (default) returns the last N lines of a plugin log from `Documents/My Games/Fallout4[VR]/F4SE/`, optional `grep` substring; `action='list'` enumerates them. Any plugin's log, not just devbench's.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `rendertarget` | `action='list'` every render target the engine owns, plus the threading facts below. `'stats'` (copy one back, report `{ nonFinitePct, darkPct, meanLuma, maxChannel }`) and `'dump'` (also write a BMP) are **disabled** and answer 501. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `measure`      | Frame-time percentiles over a window: `{ fps, meanMs, minMs, p50Ms, p95Ms, p99Ms, maxMs, frames, missedTransitions }`. No engine hook.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `files`        | `list` / `read` / `write` / `delete` files under `Data/` as the game sees it (a mod manager's virtual files included) or `Documents/My Games/Fallout4[VR]/`. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `quit`         | Quit to the desktop through the engine's own shutdown, answering `{ pid, quitting, killAfterMs }` first. `killAfterMs` arms a watchdog for a shutdown that hangs. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| `input`        | VR only. Synthetic HMD and controllers at the OpenVR boundary, the same `vrTrackedSet` contract as Skyrim's `input` tool. `sequence` plays timestamped frames of the whole tracked set, `press` holds named buttons over it, `pose` places and holds the head and hands at nodes or world points without calibrating. Skyrim's `reach` is not here. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `player`       | Power armor: `status` (`inPowerArmor`, the engine's own answer), `findPowerArmor` (frames in the player's cell, nearest first), `enterPowerArmor` / `exitPowerArmor` (instant setup through the console), `stepIn` / `stepOut` (VR: the transition a player makes, through the controllers). Weapon: `weapon` (`drawn`, `weaponState`, `equipped`), `drawWeapon` / `sheatheWeapon` (the engine's own call). Each switch waits for the engine state. See below.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |

`memory`, `log`, `files`, `quit`, `rendertarget` and `measure` are game-agnostic — they live in the core and
work identically on any platform that fills the relevant seam.

## `rendertarget`, the short version

A screenshot shows the final image. This shows the buffers that **produced** it — the
G-buffer, the light accumulation, the shadow map — which is where a rendering bug actually
lives.

```powershell
$rt = "http://127.0.0.1:8930/api/tool/rendertarget"
irm $rt -Method Post -ContentType application/json -Body '{"action":"list"}'
irm $rt -Method Post -ContentType application/json -Body '{"action":"stats","index":12}'
irm $rt -Method Post -ContentType application/json -Body '{"action":"dump","index":12,"label":"before"}'
```

A readback never makes the game wait on the GPU. The copy is queued in one main-thread
task and mapped with `DO_NOT_WAIT` in a later one, usually the next frame: about 20–35 ms
for a 1024² target on FO4VR. The result's `readback` reports `{ seq, stage, polls, ms,
frames, taskThreadId }`. Every D3D step is logged to `devbench.log` before and after the
call, so if a game ever dies inside one, the last line names it. `stage`
(`create` | `copy` | `map`) stops a `stats` call after that step, to bisect a failure.

`stats`/`dump` run only where `list`'s `threading.confinedToMainThread` is true
(FO4VR 1.2.72), and answer 501 anywhere else. Each task that touches D3D also checks its
own thread first. Both rules come from TS-031 (True Scopes' register), which is worth
knowing before reading a readback number:

> **Resolved (2026-10-28): readbacks hung, then crashed, FO4VR because they ran on worker
> threads.** A blocking readback hung the main thread twice (2026-10-15). The game stopped
> in the OpenVR submit's `ID3D11DeviceContext::Flush` → driver → `WaitForSingleObject`. A
> non-blocking one crashed the NVIDIA driver on its first call (2026-10-26): an access
> violation in `nvwgf2umx.dll` on a driver worker thread. Ruled out on the way, by
> measurement:
>
> - a single-threaded device (creation flags 0x0);
> - a shared or keyed-mutex target (target 94 is a plain shader-resource and render
>   target: bind 0x28, misc 0x0);
> - the off-thread `Release` of the staging texture.
>
> A build that logged every D3D step with its thread showed what was left: seven
> consecutive "main-thread" tasks on six different threads. The copy of 94 issued from one
> of them crashed the driver (2026-10-28). With devbench's own main-thread queue (see
> "Main thread" below), the same sequence ran clean: 13 readbacks on thread 0, including
> three with True Scopes drawing into 94.

Dumps land in `Data/F4SE/Plugins/devbench/captures`. `maxDimension` (default 1024, `0` =
native) integer-downsamples so a 4K buffer stays openable; sampling is nearest-neighbour,
**not** averaged, because averaging hides the single-pixel artefact you are hunting.

**Read `nonFinitePct` before `darkPct`.** A buffer full of NaN displays as black but reads
as _bright_ to any exponent-threshold test — a NaN's exponent is the largest possible, not
the smallest. In the investigation this came from, 12,000+ readbacks across five sessions
all reported "0 dark" against a visibly black screen. The two tests are deliberately
separate calls and a unit test enforces that they stay separate.

In dumps, **NaN/Inf pixels are painted magenta**, not clamped to white: clamped, a
NaN-filled buffer looks identical to a legitimately overbright one.

Decodable formats: `R11G11B10_FLOAT`, `R16G16B16A16_FLOAT`, `R8G8B8A8_UNORM(_SRGB)`,
`B8G8R8A8_UNORM(_SRGB)`, `R16G16_UNORM`. Anything else is reported as `decodable: false`
by `action='list'` and refused **before** the surface is mapped — an unknown stride is not
a decode inconvenience, it is an out-of-bounds read.

## `measure`, the short version

```powershell
irm "http://127.0.0.1:8930/api/tool/measure" -Method Post -ContentType application/json -Body '{"durationMs":5000}'
```

Takes no engine hook: it watches the engine's frame counter and timestamps each change
with QPC. Accurate to tens of microseconds, and it busies one core for the window — so
there is deliberately no background mode. `missedTransitions` tells you whether the
instrument itself dropped frames, which matters when comparing two runs. On Fallout 4 VR,
if the frame counter is unavailable this returns a 503 rather than a plausible zero.

## `memory`, the short version

```
expr := term (('+' | '-') term)*
term := '[' expr ']' | 'base' | 0xHEX | DEC
```

`base` is `Fallout4[VR].exe`'s load base, `[x]` dereferences a qword. An RVA from a Ghidra
session pastes straight in:

```powershell
# resolve only — does not touch the target
irm "http://127.0.0.1:8930/api/tool/memory" -Method Post -Body '{"action":"resolve","addr":"base+0x1d98ff0"}' -ContentType application/json

# eight bytes of a struct reached through a singleton pointer
irm "http://127.0.0.1:8930/api/tool/memory" -Method Post -Body '{"addr":"[base+0x6239340]+4","type":"u8","count":8}' -ContentType application/json
```

Types: `u8 u16 u32 u64 i32 i64 f32 f64 ptr bytes cstr`. `count` is capped at 4096 and the
response says `capped: true` when it was — never a silent truncation.

`cstr` returns `{ value, length, printable }`, with non-printable bytes escaped as `\xNN`.
That shape exists because of a real bug (fixed 2026-08-17): it used to hand the raw bytes
to the JSON serialiser, which **refuses invalid UTF-8 and throws**, so probing an address
that turned out _not_ to hold text answered `500` — indistinguishable from the server being
broken, and silently fatal to any scan that walks unknown memory looking for strings. One
did exactly that and reported "no text found" everywhere. `printable: false` is now the
answer to "that is not a string", and it is an answer, not a failure.

Things it is careful about, because each one cost somebody a session:

- **A bad address is a `400` naming the faulting address**, not a crash and not a bare
  "failed". You need to tell a wrong pointer from an absent one.
- **`rva` appears only for addresses inside the executable image.** A heap pointer gets
  `"heap": true`, because an RVA for one pastes into Ghidra as a valid-looking lie.
- **NaN and Inf come back as the strings `"NaN"` / `"Inf"`.** Never `null`. Finding a NaN
  is frequently the entire reason you are reading memory.

### Writes

Off by default. Two gates, on purpose:

1. `"allowMemoryWrites": true` in `config.json`, and
2. `confirm: true` on the request.

A write returns `{ before, after, held }`. `before` is your undo; `held` distinguishes a
write that stuck from one the engine overwrote on the next frame — which otherwise looks
exactly like a no-op.

## `files`, the short version

A test that changes a mod's settings has to read the files the mod wrote and put them back
after. Under a mod manager that can only be done from inside the game: MO2's virtual file
system shows the game one `Data/` folder made of many mod folders, and a file the game
creates lands in `overwrite/`, while one it edits is changed where it lives. `files` runs in
the game process, so its paths are the game's own and a write lands where the game's would.

```powershell
# every MCM settings file, as the game sees them
irm "http://127.0.0.1:8931/api/tool/files" -Method Post -Body '{"action":"list","root":"data","path":"MCM/Settings/*"}' -ContentType application/json

# a backup that round-trips exactly, then the restore
irm "http://127.0.0.1:8931/api/tool/files" -Method Post -Body '{"action":"read","root":"mygames","path":"FRIK_Config/FRIK.toml","encoding":"base64"}' -ContentType application/json
irm "http://127.0.0.1:8931/api/tool/files" -Method Post -Body '{"action":"write","root":"mygames","path":"FRIK_Config/FRIK.toml","encoding":"base64","content":"..."}' -ContentType application/json
```

- **Two roots, nothing else.** `data` is the game's `Data/`, `mygames` is
  `Documents/My Games/Fallout4[VR]/`. A path is relative to its root; an absolute one, a
  drive, a `:` or a `..` segment is a `400`.
- **`list`** takes a glob: `*` and `?` within a name, `**` for any number of folders, case
  ignored. It walks only as deep as the glob reaches, and returns at most 10000 files sorted
  by path, with `capped: true` past that. A walk that fails partway returns what it found
  with `incomplete: true` and a `reason`.
- **`read`** returns `text` (UTF-8, the default) or `base64`. A file that is not UTF-8 read
  as text is a `422` that says to read it as base64, never mangled text.
- **`write`** creates the file and its folders, and says whether it `created` it. It never
  writes native code: a `.dll`, `.exe`, `.asi`, `.bat`, `.cmd` or `.ps1` name, or bytes that
  start with `MZ` (a Windows executable), is a `403`. A plugin DLL is deployed by the build,
  not through the game, which holds a loaded one locked anyway. **`delete`** removes one file;
  a missing one answers `deleted: false`.
- It runs on the listener thread, so it answers at the main menu and while the game is
  stalled. Files are capped at 16 MB each way.

## `quit`, the short version

```powershell
# quit, and kill the game if it is still running 60 s later
irm "http://127.0.0.1:8931/api/tool/quit" -Method Post -Body '{"killAfterMs":60000}' -ContentType application/json
# { "killAfterMs": 60000, "ok": true, "pid": 31964, "quitting": true }
```

- **It answers first.** The tool sets `Main::quitGame` on the main thread, the flag the
  console's `qqq` sets, and replies; the engine's own shutdown runs on a later frame. The
  port then stops answering, but the process takes seconds more to exit, so wait for `pid`
  to exit rather than for the port to close. The Python client's `devbench.process.quit_game`
  does that.
- **The offset is checked by content.** `quitGame` is at `Main+0x24` in CommonLibF4's flat
  layout. Before writing it, the tool checks that the eight flags at `+0x24` each read 0 or 1
  and that `Main::hwnd` is a window, and answers `500` naming the byte otherwise. On FO4VR
  1.2.72 (2026-11-16) the flags read `0 0 0 1 1 0 0 0` at the main menu (`gameActive` and
  `onIdle` set) and in game, and `hwnd` was a window.
- **`killAfterMs` is the only way to end a hung shutdown from inside.** A shutdown that
  hangs has usually stopped this listener already, so no later call could reach it. With
  `killAfterMs` > 0 a thread started by the call terminates the process if it still runs
  that long after; `0` (the default) never kills.
- **Launching is the client's job.** The DLL cannot start the game it runs in, so the
  Python client's `devbench.process.launch` runs the user's command and waits for health
  and the main menu; its pytest plugin launches the game when none answers
  (`--devbench-launch` / `DEVBENCH_LAUNCH`) and quits it at the end (`--devbench-quit`).
  Through MO2 the command is `ModOrganizer.exe -p <profile> run -e "<executable title>"`.
  On FO4VR 1.2.72 with MO2 2.5.2 (2026-11-16) a full suite ran unattended from a closed
  game to a report, both with MO2 open (the command hands off and exits) and closed; the
  main menu was up 18-19 s after the launch.

## `nodes`, the short version

The on-demand form of the F4VR Common Framework's `sDumpDataOnceNames` dumps. There, a
dump means editing an INI, waiting a frame and grepping a log, and it only exists inside a
mod built on the framework. Here it is one call, returns JSON, and can be scoped to a subtree.

```powershell
$n = "http://127.0.0.1:8931/api/tool/nodes"
irm $n -Method Post -ContentType application/json -Body '{"action":"roots"}'
irm $n -Method Post -ContentType application/json -Body '{"action":"tree","root":"firstPerson","maxDepth":3}'
irm $n -Method Post -ContentType application/json -Body '{"action":"find","contains":"Wand"}'
irm $n -Method Post -ContentType application/json -Body '{"action":"get","name":"HMDNode"}'
irm $n -Method Post -ContentType application/json -Body '{"action":"geometry"}'
```

Starting points are `scene` (the top of the graph the player is in), `player` (third-person
3D), `firstPerson`, and on VR every `RE::VRPlayerNodes` slot (`hmdNode`, `primaryWandNode`,
`pipboyRootNIFOnlyNode`, …), or any node `address`. The framework's dump names work as
aliases: `all_nodes`, `skelly`, `fp_skelly`, `pipboy`.

Each node's `type` is its real class, read from its RTTI, and its `address` pastes into
`memory`. Caps are reported instead of silently applied: `depthCut` on a node whose children
were not expanded, `partial` on one cut by `maxNodes`, `limited` / `visitCapped` on a search.

Two layout facts it is built around, worth not rediscovering:

- **`PlayerCharacter` is typed flat-only in this CommonLibF4.** `firstPerson3D` is `+0xB78`
  on flat and `+0xFE8` on VR (VR inserts 0x470 bytes of player state ahead of it), and the VR
  node table at `+0x6E0` does not exist on flat at all. `player->firstPerson3D` compiles and
  reads the wrong field on VR. So every entry pointer is read by offset for the running
  runtime and then **identified by its MSVC RTTI (reads only, no virtual call) before
  anything trusts it**. A slot holding something else comes back as `refused`, naming what
  it actually holds.
- **`NiNode::children` is `+0x120` on flat, `+0x160` on VR.** `NiAVObject` itself is the same
  on both. The walk only reaches children through `GetRuntimeData()`, never the member.

## `console`, the short version

```powershell
irm "http://127.0.0.1:8931/api/tool/console" -Method Post -ContentType application/json -Body '{"command":"player.getpos x"}'
# { "captured": true, "compiled": true, "count": 1, "lines": ["GetPos: X >> -89.48"], ... }
```

One call returns the output: no separate `read`, no marker commands, no polling. At most
`maxLines` (default 200) of the most recent lines come back, and `omitted` counts the
rest. A command that does not compile answers `compiled: false`, with the engine's reason
in `lines`.

Three engine facts it is built around, worth not rediscovering:

- **`Console::ExecuteCommand` does not run the command.** It echoes the line, splits it
  on `;`, and queues each command for a later frame. Its output lands after any task that
  called it has returned. So a capture compiles and runs each command itself
  (`Script::CompileAndRun` with the console's compiler, against the selected reference)
  and reads the buffer before and after in the same task. `capture=false` keeps the
  queued path, which also handles a `ForEachRef[...]` block.
- **A command that leaves the world must not run inside a task.** Run that way, an in-game
  `load` entered the engine's nested load-screen wait from inside F4SE's task drain and
  hung FO4VR for good (2026-10-26, pid 42092; earlier loads just took longer than the 5 s
  task timeout). So `load`, `coc`/`CenterOnCell`, `cow`/`CenterOnWorld` and
  `qqq`/`QuitGame` are always queued, even with `capture=true`. The reply says so
  (`queued: true` and a note). Wait for the load yourself afterwards. Checked live on
  FO4VR (20b3555, 2026-10-26, FRIK's rig): a main-menu `load`, an in-game `coc` and an
  in-game reload each replied within 0.02 s, queued, and the game loaded and kept
  running; ordinary commands are still captured.
- **The Console menu drains `ConsoleLog::buffer`.** Every print appends to that buffer.
  Before the Console menu has ever been opened, the buffer keeps everything printed since
  load. After that, the menu empties it into its history on the next UI pass. That pass
  never runs inside a task, so a capture sees the output either way.
- **The buffer holds 65,534 bytes, and the engine silently drops a line that would pass
  that.** It only fills while the Console menu has never been opened, so a capture that
  finds the menu missing from the menu map first shows and hides it (`madeConsoleMenu`
  reports that call). `bufferNearlyFull` still warns when output may be missing, which now
  means the menu could not be made yet. Checked live on FO4VR 1.2.72 (2026-11-11): at the
  main menu of a fresh launch the first capture answered `madeConsoleMenu: true` with its
  output, the buffer then read size 0 (capacity 127, so it had been drained), the next
  capture did not make it again, and no Console stayed open.

The console's history shows the command line above its output, as when typed.

## `input`, the short version

Drives the VR controllers and HMD so a test can exercise what reacts to hand position and
buttons. The contract is Skyrim's `vrTrackedSet`, unchanged: one owner at a time, the whole
tracked set (HMD, left, right) replaced in every frame, the real devices passed through
whenever nothing is active. Terms (Wand, Tracked set, Sequence, Pose) are in
[GLOSSARY.md](GLOSSARY.md).

```powershell
$i = "http://127.0.0.1:8931/api/tool/input"
irm $i -Method Post -ContentType application/json -Body '{"action":"status"}'
# left hand 10 units in front of the HMD, held 60 s
irm $i -Method Post -ContentType application/json -Body '{"action":"pose","owner":"t","hands":{"left":{"node":"hmdNode","front":10}}}'
# tap the left trigger while it holds
irm $i -Method Post -ContentType application/json -Body '{"action":"press","owner":"t","hand":"left","buttons":["trigger"]}'
```

- **`sequence`** plays frames from `observe`'s shape. It returns a `controlToken` that
  `stop` needs. A `sequence` carrying the running one's token replaces its frames in place,
  so chained moves never hand the devices back to the physical controllers in between.
- **`press`** holds `trigger`, `grip`, `a`, `b` or `thumbstick` on one hand over the active
  sequence (or pose hold), as its owner; trigger and grip also drive their axis to 1.0.
  `stick: [x, y]` (−1..1, x right, y up), alone or with buttons, pushes that hand's
  thumbstick (axis 0, touched, not clicked) for `durationMs`, then lets it back to center;
  the poses do not move, so a ray stays where the pose put it. Each hand has its own press:
  presses on both hands overlap (both sticks clicked together is two calls), and a new
  press on a hand replaces the one it holds.
- **`pose`** places the hands at the player's nodes or at world points, and turns the head,
  in about a second: see below. A node is a VR node slot as `nodes` names it (`hmdNode`,
  `uprightHmdNode`, …), else the first-person skeleton, else the body skeleton; a hand's
  `root` (`player` or `firstPerson`) searches one skeleton alone, since both have a `Chest`
  and the first-person one sits near the world origin, far from the body. It reads where
  the hands land from the wand nodes (`primaryWandNode` / `secondaryWandNode`, mapped to
  physical left/right through `bLeftHandedMode:VR`, read live), so it works with or without
  FRIK.
- **Logical hands.** `press.hand`, the `pose` `hands` keys and `head.lookAt` take `primary` /
  `offhand` beside `left` / `right`, resolved live from `bLeftHandedMode:VR` (the primary hand
  is the right controller unless left-handed). Answers stay keyed `left` / `right` and carry
  `leftHanded`; two names for one controller (`left` and `primary` while left-handed) answer 400. Skyrim answers 501 for them.
- **`handedness`** reads the game's handedness, or sets it and confirms the game followed: see
  below. `capabilities.vrTrackedSet.leftHanded` reports it live.
- **`reach`** is Skyrim's: it calibrates (about 16 s per hand) because Skyrim measures the
  hand bones. On Fallout the Wands sit straight at the controllers, so `pose` needs no
  calibration, and `reach`, `reachStatus` and `reachStop` answer 400 here.
- **Hooks** go on the real IVRSystem_019 / IVRCompositor_022 objects, looked up through
  `openvr_api.dll`'s `VR_GetGenericInterface`, at kGameDataReady/kGameLoaded and retried
  until OpenVR is up. That puts devbench closest to the runtime, under the F4VR Common
  Framework's own patches, so injected input passes through other mods' controller hooks
  like real input. `status.hooks` reports `closestToRuntime` per slot; `false` means another
  mod patched that slot first and injected input skips it.
- **Lifecycle**: an active sequence or pose stops on load and new game.

### `pose`: placing without calibrating

`reach`, which Fallout had until 2026-11-11, measured how tracking space maps to the world on
every run. That map is simpler than it assumed: the game puts each Wand straight at its controller's pose, and the room node
recenters on the head, so a world point maps to tracking space through the HMD node alone:

```
world = hmdNode + turn(theta) * 70 units/m * (controller - hmd)    (tracking axes x right, y up, -z forward)
```

`theta`, the turn between tracking space and the world, is not on any scene-graph node (the
room node has no rotation). `pose` fits it from where the held Wands land against where it put
them, so placing a hand is one calculation, a short move and a check.

```powershell
# take over in the ready pose, then put the left hand at a world point with the head looking down
irm $i -Method Post -ContentType application/json -Body '{"action":"pose","owner":"t"}'
irm $i -Method Post -ContentType application/json -Body '{"action":"pose","owner":"t","head":"down","hands":{"left":{"world":[-1765,-1372,75]}}}'
irm $i -Method Post -ContentType application/json -Body '{"action":"pose","owner":"t","hands":"ready","head":"ready"}'
# look at the left hand while it goes to the chest
irm $i -Method Post -ContentType application/json -Body '{"action":"pose","owner":"t","head":{"lookAt":"left"},"hands":{"left":{"node":"Chest","root":"player","front":10}}}'
```

- **Synchronous.** It answers once the hands are placed (about 0.5–1 s), with `controlToken`
  (for `stop`), `fresh`, `eyeHeight`, `headPitch`, `headYaw`, `lookAt` (`target`, `clamped`),
  `turnDeg` (when a node or world target fitted it), `corrections` and per hand `tracking`,
  `wand`, `target` and `error`. `press` works over the hold, as for a sequence.
- **Held across calls.** A later `pose` by the same owner moves from the held one, replacing
  its sequence in place (`fresh: false`). An absent hand or head stays where it is. The hold
  lasts `holdMs` (default 60 s) after each call, so a caller that dies frees the controllers
  within a minute. It is its own sequence, under its own token: once it runs out, stops, ends
  at a load or is replaced by another `sequence` (even the same owner's), the next `pose`
  starts fresh, and while another sequence holds the devices `pose` answers 409.
- **Ready pose.** A fresh hold starts with the head pitched −22° and the hands 0.6 m in front,
  0.4 m below the eyes, 0.19 m to each side, swung 10° left so the desktop mirror (the left eye)
  shows them, the controllers pointing 10° up. It was picked in the headset. `hands: "ready"`
  puts both hands back; `head: "ready"` puts the head back (−22°, yaw 0), and `head: "down"`
  looks at the body (−75°, yaw 0, low enough to show the belt).
- **Eye height.** A body mod lifts the room node above the player's feet (FRIK's seated
  `fPlayerHMDOffsetUpSitting`, 37.4 units here), and an injected head at a fixed standing
  height stacks on that lift: under seated FRIK, the fixed 1.65 m `reach` used stretches the body. `pose`
  reads the lift and injects the eye height that puts the camera `head.cameraHeight` (default 115) units above the feet: 1.11 m seated, about 1.52 m under FRIK's standing lift.
  `head.eyeHeight` overrides it; either restarts the hold in the ready pose at that height,
  in place.
- **Controller aim.** A node or world hand takes `pitch` / `yaw` (degrees, tracking space)
  for how its controller points; otherwise it points the ready way. The Wand sits at the
  controller whatever the aim, but the body's hand hangs about 6 units behind it along the
  controller: a hand raised to the head is placed tilted back (pitch 123, yaw 14 for the
  left, as recorded in the headset), so it hangs beside the head instead of in the face.
  `yawIn` replaces `yaw` with a turn toward the body for either hand (tracking yaw `-yawIn`
  on the left, `+yawIn` on the right), so one recorded aim fits both hands. Like `yaw` it is
  in tracking space: the two hands mirror around the body's forward, which is the head's only
  at head yaw 0.
- **Head offset.** A world hand takes `headOffset: [outward, forward, up]`, world units along
  the HMD node's axes (its rotation rows, pitch included) read after the head turns, outward
  toward that hand's own side: one offset puts each hand at its mirror image around the head.
  It resolves once into the hand's `target`, which corrections then chase. A node hand
  refuses it (400).
- **The head only turns**, around the eye point, so the HMD node does not move between
  placements: `head.pitch` (negative looks down) and `head.yaw` (positive turns left), in
  tracking degrees, the yaw relative to the ready forward. There is no head position. A body
  mod leans and turns the body with the head, so `pose` turns the head first and reads node
  targets in the turned pose; a correction then removes what the body moved with the arm.
- **Looking at something.** `head.lookAt` is `"left"`, `"right"`, `"hands"` (their midpoint),
  a node (with `head.root`) or a world point `[x,y,z]`, and computes the pitch and yaw, so it
  refuses `pitch` / `yaw` beside it. The point is read before anything moves: a hand this
  call places is aimed at its target, one it leaves at its wand. A body mod turns the body
  with the head, and a hand on a node moves with it, so for a hand target `pose` aims again
  at where the wands landed (`lookAt.reaimed`) when the first aim is off by more than 0.5°.
  The hold keeps the angles, not the target, so a later call that moves the hands does not
  turn the head. A target straight above or below clamps the pitch to ±85°
  (`lookAt.clamped`); one at the eyes answers 400.
- **Pitch limit ±85°**, explicit or from `lookAt`. Chosen in the headset: closer to vertical
  the view and FRIK's neck look wrong. It is not for stability: a held head at −89° and at
  −85° kept the HMD node's and the body's heading still to 0.0° over 10 s, since an injected
  pose has none of a real headset's jitter.
- **Skyrim**: not measured; `pose` answers 501 there.

Checked live on FO4VR 1.2.72 (2026-11-11, FRIK seated, F4VR Common Framework) through a
test-side prototype of the same map, with a held `sequence`:

- the map: two physical controllers 0.36 and 0.39 m from the head landed 25.6 and 27.6 units
  from the HMD node, both matching the map with one turn of 66.71°: the physical HMD's yaw
  (75.4°) minus the HMD node's (8.55°, the player's heading);
- the room node sat 37.39 units above the feet, exactly FRIK's seated lift, and with the eyes
  injected at 1.11 m the HMD node sat 115.08 units above them;
- Immersive Flashlight's gesture zones (head, hand to hand, the belt with each hand) each took
  one move: 0.5–1.0 s, no correction, landing 0.0–1.9 units from the zone's center. The same
  test area went from 151 s with `reach` to 26 s.

Then through `pose` itself, the same day and setup:

- a fresh hold read the room node's lift and injected the eyes at 1.109 m; the HMD node sat
  114.99 units above the feet (target 115). Each call answered in 0.6–1.3 s;
- the left hand to the body grab zone's center as a world point, with the head pitched to
  −60°: one correction (the body shifts with the pitch), 0.93 units off; to `Chest`
  (`root: player`, `front: 10`): one correction, 0.60 units off; back to `ready`: no reading
  needed;
- Immersive Flashlight's controller tests ran on `pose` alone: all six passed, each zone in one
  placement, landing as with the prototype.

Head presets, yaw and `lookAt`, the same day in QASmoke (FRIK seated), measured against the
HMD node's forward row:

- `head: "down"` and `{hands: "ready", head: "ready"}` answered in about 1 s each;
- the left hand to `Chest` at head yaw 0, +45 and −45: the fitted turn stayed 135.18° at all
  three (turning the head does not turn the map), and the wand landed on the turned body's
  `Chest` with no correction;
- `lookAt` the hands, a world point ahead and `Pelvis`: 0.0–0.6° off; a point straight below
  clamped to the limit, then −89° (0.57–1.15° off; the limit is now 85°); the three refusals (at the eyes, beside `pitch`, `root` without a node)
  answered 400;
- `lookAt` a hand placed on `Chest` or `Pelvis`: the first aim missed by up to 16.75° (the
  head turn moved the body, and the hand with it); aiming again at the landed wand brought
  it to 0.0° (0.57° where the pitch clamps), each call 1.4 s, the wands 0.004–0.007 units
  from their targets. To look at its own chest the head turns to a large yaw (−150° to
  −170°) at a steep pitch: the seated body's chest sits just behind the eye point;
- a `pose`, then `stop` with its token: the game kept running (an earlier hang after a stop
  did not reproduce);
- with `reach` gone from Fallout: `capabilities.vrTrackedSet.actions` lists `pose` after the
  shared six, `reach` / `reachStatus` / `reachStop` answer 400 ("use pose"), `lookAt` at
  the left hand on its way to `Chest` still re-aimed and landed 0.004 units off in 1.6 s,
  and a `press` over the hold worked as its owner and answered 409 for another.

`headOffset` and `yawIn`, checked live on FO4VR (2026-11-16, FRIK): with the head at pitch
−40, yaw 15, the left hand and then the right to one world point with `headOffset [12, 5, -4]`,
`yawIn 25`, `pitch 20`:

- `hmdNode`'s rotation rows are right, forward, up (forward's z −sin 40°, up's z cos 40°);
- each wand landed 0.005 units from its target with no correction; projected on the head's
  axes their offsets from the point were (−12.00, 4.99, −4.00) and (12.00, 5.00, −4.00);
- the answer's tracking yaws were −25 and +25, and both wands turned toward the body, at
  exactly ±25° from the body's forward (the fitted turn, 11.68°) and up by sin 20°.

`press` with `stick`, checked live on FO4VR (2026-11-17, FRIK, QASmoke) over a ready-pose hold:

- the left stick at `[0, 1]` for 1.5 s walked the player 215 units along its heading;
- the right stick at `[1, 0]` for 0.3 s snap-turned the player 30°;
- a later `pose` answered both hands at the same tracking positions as before the pushes;
- a press on each hand (`thumbstick`, 1.6 s and 1.5 s) answered within a millisecond of each
  other, so they overlap. No in-game effect of the overlap was looked at.

Checked live on FO4VR 1.2.72 (2026-11-10, FRIK, F4VR Common Framework and FO4VRTools
installed). The `reach` runs are history: they are what showed the map is simpler, and
`reach` has left Fallout since.

- all nine hooked slots `closestToRuntime: true`; `observe` returns the physical tracked set;
- a `sequence` moved the wand 21 world units for 0.3 m of tracking (about 70 units per
  metre); a load released it;
- `reach` converged within 0.6 units on the first or second pass, in left-handed mode too
  (the wands swap and reach followed the swap); the hand stays injected from the first frame
  to the last, with no step over 3 units after calibration;
- calibration reuse and `root` (2026-11-11): a first reach to `hmdNode` measured the left
  hand and finished in 20 s with a 3 s hold; the next one, 5 units lower, `reused` it,
  converged on the first pass and finished in 10 s with a 0.5 s hold. A reach to the
  body's `Chest` (`root: player`, 15 units in front) reused it too and converged on the
  second pass (the body follows the arm) in 12 s;
- the acceptance scenario: Immersive Flashlight's head gesture, driven by devbench alone.
  `reach` put the offhand 10 units in front of `hmdNode`, a `press` tap moved the light to
  the head, a long press back to the hand. The Common Framework's input handling saw the
  injected buttons as real ones. (Under the mod's Immersive headgear restriction that
  gesture needs headgear; the run set `iFlashlightHeadgearRequirement` to 0 for the session.)

### `handedness`: setting it and seeing the game follow

```powershell
irm $i -Method Post -ContentType application/json -Body '{"action":"handedness"}'                     # { leftHanded }
irm $i -Method Post -ContentType application/json -Body '{"action":"handedness","leftHanded":true}'   # { leftHanded, was, changed, elapsedMs }
```

- **Set through `setini`.** The set runs the console's `setini "bLeftHandedMode:VR"` on the
  main thread. Writing the setting object alone changes the value (`getini` shows it) but the
  wands never swap; `setini` does more than the write. The game swaps `primaryWandNode` and
  `secondaryWandNode` between the controllers a few frames later.
- **The check.** A set waits until each wand slot sits where the other one was (within 2
  units, up to 2 s). That only shows with the controllers held still and apart, so it runs
  under the caller's own `pose` hold (same `owner`), or under a short ready-pose hold of its
  own (`devbench:handedness`) that it hands back when nothing holds the devices. Another
  owner's sequence, or a frame `sequence`, answers 409; so do held wands under 10 units apart
  (after waiting up to 1 s for a fresh hold to land). With the headset idle the physical
  controllers are untracked and both wands sit at the room origin, which is why it never
  checks against them.
- **A set that does not take** puts the old value back and answers 504 with the setting it
  read and where each wand was against where it should be. Setting the value it already has
  answers at once, `changed: false`.
- **Skyrim** answers 501.

Checked live on FO4VR 1.2.72 (2026-11-15, QASmoke, FRIK, F4VR Common Framework):

- a console `setini` under a ready-pose hold swapped the wands within 0.25 s, both ways; a
  write of the setting object alone left them unswapped for 2 s (the set answered 504 and put
  the value back);
- a set with no hold took its own and handed it back (no sequence active after), in about
  0.2 s; a set under the caller's `pose` hold, 54 ms, the hold still running after; a set to
  the value it has answered `changed: false` at once; another owner's set during a hold
  answered 409;
- left-handed: `capabilities` and the `pose` / `press` answers carried `leftHanded: true`, a
  `press` on `offhand` landed on the right controller, `pose` with `primary` at a world point
  placed the left wand 0.01 units off, and `left` + `primary` answered 400; right-handed
  again, `offhand` pressed the left controller.

## `player`, the short version

Puts the player in and out of a power armor frame so a test can set up power armor without
a headset, either instantly (`enterPowerArmor` / `exitPowerArmor`) or the way a player does it,
through the controllers (`stepIn` / `stepOut`).

```powershell
$p = "http://127.0.0.1:8931/api/tool/player"
irm $p -Method Post -ContentType application/json -Body '{"action":"findPowerArmor","radius":1000}'
irm $p -Method Post -ContentType application/json -Body '{"action":"enterPowerArmor"}'   # instant, nearest frame
irm $p -Method Post -ContentType application/json -Body '{"action":"exitPowerArmor"}'
irm $p -Method Post -ContentType application/json -Body '{"action":"stepIn"}'            # walk in, VR only
irm $p -Method Post -ContentType application/json -Body '{"action":"stepOut"}'
```

- **State** (`status`, and every wait) is the engine's own `PowerArmor::ActorInPowerArmor`
  (VR `0x9BF5D0` through the address library), not console output.
- **Frames** are furniture whose base has the `FurnitureTypePowerArmor` keyword, in the
  player's cell only: a frame across an exterior cell border is not listed.
- **`enterPowerArmor` / `exitPowerArmor`** run `player.cf "Actor.SwitchToPowerArmor" <frame>` /
  `none`, the switch Immersive Flashlight's release check uses. It is setup, not the
  transition: no activation, no walk-in, no animation, and the frame comes to the player.
  Activating a frame from the console (`<frame>.activate player`) does not get the player in.
- **`stepIn`** stands the player 70 units behind the frame, facing it, injects a standing pose
  through the `input` tool (owner `devbench.player`), turns it until the primary wand's
  activation ray is on the frame, and taps A on the primary hand. The game then runs its own
  walk-in: the player ends at the frame's position. The ray is read from the wand's
  ViewCaster (below), so the press only happens once the wand is on that frame; otherwise
  `outcome` is `notAimed` and `aim.passes` shows where it pointed and what it picked.
- **`stepOut`** holds the secondary hand's A/X button (X with the default hands) for 2 s. The
  frame stays; the player steps out of it. That is the exit gesture in a setup with KabutoVR
  loaded; a held A on the primary hand did nothing there.
- **The wait** polls short main-thread reads from the listener thread until the state flips
  or `timeoutMs` passes, so the skeleton swap is never blocked. `outcome` is `switched`,
  `already`, `refused` (the state did not change in time), `notWaited` or `notAimed`.
- **A tap is re-sent** every 1.5 s while the state has not flipped and the wand is still on the
  frame; `taps` counts them. Twice in live runs a tap with the wand on the frame started no
  walk-in, both right after a `stepOut`, but four later immediate `stepOut` / `stepIn` cycles
  switched on the first tap, so the cause is unknown. A tap that works flips the state within
  about 250 ms.
- **Aiming waits for the wand to settle**: after each turn it reads until two reads 100 ms apart
  agree. The first injection after a load takes longer than a fixed 300 ms to reach the wand.

The wand's activation target: `ViewCasterBase::QActivatePickRef` (`0x9D0CE0`, from
F4VRControlsConfig in the reference library) returns, through vtable slot 2, the record at
`+0x90` of the caster held at `[base+0x5AC72B0]` while the byte at `+0xD0` is set; the record
starts with the `ObjectRefHandle`. Slot 2 (`0x9D5780`) was disassembled on 1.2.72 and is
checked before every read, so another runtime fails with a diagnostic. The ray runs along the
wand node's local +Y, which is the second **row** of its world rotation; the second column is
the same direction mirrored across world Y.

Checked live on FO4VR 1.2.72 (2026-11-10, FRIK and KabutoVR loaded), in an interior cell with
five frames 1,290–1,490 units away:

- `findPowerArmor` listed all five; `enterPowerArmor` with no `formId` took the nearest and
  read `inPowerArmor: true` on the first poll (6 ms); `exitPowerArmor` the same (10–87 ms).
  Read in the same task as the command, the state has not changed yet: the switch lands on a
  later frame, which is why the wait polls. KabutoVR did not block the scripted exit;
- `already`, a reference that is not a frame (400) and `formId` with `wait=false` behaved as
  described;
- `stepIn` aimed and switched with the controllers switched off, on frames facing 0 and 231
  degrees, in 216–263 ms from the tap; the power armor HUD opened and FRIK rebuilt its
  power armor skeleton, as for a hand-made entry. `stepOut` switched in 438–514 ms. Four
  immediate `stepOut` / `stepIn` cycles in a row each took one tap;
- the scripted entry left FRIK warning that two forearm bones were missing
  (`RArm_ForeArm2` / `3`); the walk-in did not.

### Weapon draw and sheathe

```powershell
irm $p -Method Post -ContentType application/json -Body '{"action":"weapon"}'
irm $p -Method Post -ContentType application/json -Body '{"action":"sheatheWeapon"}'
irm $p -Method Post -ContentType application/json -Body '{"action":"drawWeapon","timeoutMs":5000}'
```

- **`weapon`** answers `{ drawn, weaponState, equipped }`. `weaponState` is `sheathed`,
  `wantToDraw`, `drawing`, `drawn`, `wantToSheathe` or `sheathing`. `drawn` is `weaponState`
  at `drawn` or later, what `GetWeaponMagicDrawn()` means, so it is true for drawn fists.
  `equipped` is the weapon in hand as `{ name, formId }`, or `null` for none (fists); VR only.
- **`drawWeapon` / `sheatheWeapon`** call `Actor::DrawWeaponMagicHands(bool)`, the engine's
  own draw / sheathe (vtable slot `0xC9`; the player's is `0xF78D10` on 1.2.72, the call
  F4VRControlsConfig and ROCK use). It plays `ActionDraw` / `ActionSheath` and, when the graph
  accepts, moves the state to `drawing` / `sheathing` in the same call. The wait then polls
  until the state settles at `drawn` / `sheathed` or `timeoutMs` passes: `outcome` is
  `switched`, `already` (nothing was called), `refused` or `notWaited` (`wait=false`).
  The graph ignores a call made right after the previous switch settled and the state stays
  at rest, so while it does the call is made again every 250 ms (`calls` counts them).
- **Drawn fists**: `player.unequipitem <weapon>` leaves the player with fists up and
  `GetWeaponMagicDrawn()` true. `sheatheWeapon` is the way to put them away; FO4's Papyrus
  has no `SheatheWeapon`, and `player.cf` with an unknown function compiles and only prints
  "not found".
- **Do not look forms up with the console's `help`** (`help ActionSheath 4` hung the main
  thread and crashed the game).

Two VR layouts differ from CommonLibF4's headers, both read from the 1.2.72 binary:

- the weapon state is `(dword at ActorState+0x0C >> 2) & 7`, one bit above the header's
  `0C:01` (`0xF78D10` tests it that way). `weapon` reads those bits itself. Through the header
  a settled state still answers `GetWeaponMagicDrawn()` correctly, but the transition states
  decode wrong;
- `MiddleHighProcessData::equippedItems` is at `+0x290`, not `+0x288` (`0xE80240`). `equipped`
  does not read the array: it calls the Papyrus native `Actor.GetEquippedWeapon` (`0x140D790`,
  the call F4VR-CommonFramework makes), checked by its first bytes before the call.

Checked live on FO4VR 1.2.72 (2026-11-11), on a character with no weapon and then a 10mm
added from the console:

- with nothing equipped, `drawWeapon` raised the fists (`drawn: true`, `equipped: null`) in
  320–333 ms and `sheatheWeapon` put them away (`drawn: false`, `sheathed`) in 432–444 ms;
- with the 10mm, `weapon` read `drawn` and `{ name: "10mm", formId: "0x00004822" }`;
  `sheatheWeapon` with `wait=false` read `sheathing` right after the call, then `sheathed`;
  `drawWeapon` switched in 221 ms. `already` answered without calling the engine;
- the state dword read `0x20020D` with fists drawn and `0x200201` sheathed: bits 2–4 hold the
  state, and the player's vtable slot `0xC9` is `base+0xF78D10`;
- `player.unequipitem` on the drawn 10mm left the state `sheathed` here, so the drawn fists
  that command can leave behind were not reproduced that way; they were made with `drawWeapon`;
- with the assault rifle, a `sheatheWeapon` straight after a `drawWeapon` took `calls: 2`
  and switched in 788 ms, twice in a row; each draw switched with one call in about 215 ms.

On flat the state comes from the header's bitfield and there is no `equipped`; flat has never
been loaded.

## Main thread

Every tool that touches the game (`console`, `inspect`, `menu`, `nodes`, `rendertarget`, …)
runs its work as a main-thread task. On Fallout 4 VR, **F4SE's task queues do not deliver
that.** F4SE VR drains `AddTask` from a detour on the _entry_ of `MessageQueueProcessTask`
(0xDA87C0), which worker threads call too. Measured 2026-10-28 on 1.2.72, against cdb's
thread 0:

- through `AddTask`, 7 consecutive tasks ran on 6 different threads;
- through `AddUITask`, all 43 tasks ran on worker threads.

So devbench keeps its own queue and drains it right after `Main::OnIdle` calls
`Main::Swap` (the call at +0xD844BC). That is thread 0, between frames, after the frame
has been drawn and submitted. The call is checked before it is patched. On another
runtime, or if the bytes do not match, devbench falls back to `AddTask` and logs why.
Checked live: every task on thread 0 at the main menu and in game.

- `rendertarget list` reports the facts: `threading.mainThreadId` (the thread F4SE loaded
  the plugin on, which is thread 0), `tasksOffMainThread` (every task checks its thread
  and counts any other) and `confinedToMainThread`.
- **One behaviour change:** a call made before the first frame is drawn through `Swap`
  (early startup, before the main menu) waits and can 504. Before, it ran on a worker.
- OnIdle's own `MessageQueueProcessTask` call (+0xD845E3) looks like the natural site and
  is the wrong one: it is skipped during play, so nothing would drain in game.

Mods have the same problem. A mod that marshals to the main thread through F4SE's queue
is on a worker thread in game, so devbench offers its queue through the C-ABI. See
"Registering your own tools".

## Known gaps

- **GPU stage timers** (per-pass GPU+CPU ms) are not ported. They need instrumentation
  points, which means exposing them through the cross-plugin C-ABI so a mod can bracket
  its own passes. The ABI is no longer the blocker (it works on Fallout now); the
  instrumentation points still have to be designed.
- **`rendertarget` covers 2D colour targets only.** No depth/stencil, no cube maps. Target
  _names_ are not reported: Fallout addresses targets by logical id through
  RenderTargetManager's remap table, a different index space from the physical slots
  enumerated here, and printing a name that might belong to a different buffer is worse
  than printing none. On FO4VR 1.2.72 the remap is `physical = [rtm + 0x13BC + id*4]`
  (u32), e.g. logical 0x61 → 93, 0x62 → 94, 4 → 6 — measured by the True Scopes
  investigation, not yet used by `rendertarget`, which still takes the physical index.
- **`input` on Fallout: what is not verified.**
  - **Skyrim was not re-run live after the VR input code moved to `src/core/`** (the Sequence
    manager and hooks are shared; Reach went back to Skyrim's platform code on 2026-11-11).
    Skyrim builds and its unit tests pass; its live path (hand bones, controller-index
    remap, Reach) is untested since the move, and `press` with `stick` was never run there.
  - **Controller indices are not remapped on Fallout.** Skyrim points the game at the
    frame's controller indices; on Fallout that seam is a no-op, so frames should carry the
    indices `observe` reports. Whether FO4VR caches indices so a frame with others is
    ignored is unchecked.
  - **One frame of physical pose at a sequence's start and stop.** The timeline probe sees a
    single sample of the real controller as a sequence begins and ends. Moves chained by
    replacing in place (as `reach` did and `pose` does) keep only the first start and the
    last stop; the cause is not investigated.
  - **A slow first `handedness` set.** The first set after launching the game and entering
    QASmoke answered after more than 30 s (it took effect and handed its hold back); every
    later one, also right after a `coc` reload, took about 0.2 s. Not reproduced or
    explained.
  - **Flat Fallout 4** registers `input` too; `capabilities` answers `available: false` and
    the actions 503 (no OpenVR). Never run.
- **`record` / `replay` / `scenario` / `capture` / `game` / `papyrus`** are Skyrim-only
  so far. They are mostly pure logic around a few engine calls (`Recording.cpp` is 1014
  lines with 11 game references), so porting them is bounded work, not a rewrite.
- **`nodes` and the new `inspect` kinds are LIVE-TESTED on Fallout 4 VR (2026-09-28).**
  `firstPerson3D` at `+0xFE8` reads a `BSFadeNode`. All 41 VR node-table slots hold nodes,
  none refused, and the nine checked slots carry the names the engine gives them (`hmdNode`
  → `HMDNode`, `pipboyRootNIFOnlyNode` → `PipboyRoot_NIF_ONLY`, …). The `hmdNode` slot and a
  graph search for `HMDNode` land on the same address. The full scene (1554 nodes) dumps in
  28 ms of main-thread time. Refusals were confirmed live too, including a real
  `bhkNPCollisionObject` named rather than followed. Flat Fallout 4 is not yet run.
- **`menu` and the `ui`/`blocking` fields on `inspect` are LIVE-TESTED on Fallout 4 VR
  (2026-08-17).** Detection, `console.blocked`, `describe` and `accept` all confirmed in
  a running game, including the whole recovery: summon the missing-masters modal →
  `describe` reads its text and buttons → `accept` → `blocking` clears → `coc` moves the
  player again. Two engine facts it cost, worth not rediscovering:
  - **`RE::UI` is NULL at `kPostLoad` on FO4VR.** Registering a `MenuOpenCloseEvent`
    sink there silently does nothing: no error, no log line, and every later "is a menu
    open" answer is a confident empty set — it reported no menus through a cell
    transition that certainly opened a `LoadingMenu`. `kGameDataReady` (~7 s later on
    this install) works, and `InstallGameEvents` is idempotent so both are called. Same
    species as the Skyrim platform's `BSInputDeviceManager`-at-`kPostLoad` note, which
    was already written down ten lines from the code that assumed the opposite. The
    open-menu _answers_ no longer depend on the sink at all — they read
    `RE::UI::menuMap` under the engine's own `GetMenuMapRWLock()`, which is ground truth,
    still needs no main-thread hop, and is correct for menus that opened before devbench
    subscribed. `source` in the result says which one answered, so a `false` can be told
    apart from a blind instrument.
  - **`MessageBoxMenu::currentMessage` is at `+0xF8` on VR, not CommonLibF4's `+0xE8`**
    (a 0x10 base-class shift). `MessageBoxData`'s _own_ layout is byte-identical on both
    binaries — `headerText` `+0x18`, `bodyText` `+0x28`, `buttonText` `+0x38` (a
    `BSTArray`: `capacity` at `+0x08`, **`size` at `+0x10`**, since `sizeof(BSTArray)` is
    0x18), `warningContext` `+0x50`, `callback` `+0x58`, `modal` `+0x64`. The wrong
    offset points at a run of small integers with no vtable, and dereferencing it took
    the process down. So `describe`/`accept` try both offsets, identify the winner by
    _content_ (a vtable **and** a `bodyText` that reads as text), report which matched in
    `currentMessageOffset`, and on failure return a hex dump rather than crashing —
    a wrong offset here yields plausible-looking qwords, exactly as `core/gfx/Validate.h`
    warns for the render-target seam.
- **`game::CurrentFrame()` on VR** reads `BSGraphics::State::frameCount` at a measured
  image offset, since the VR address library does not cover that id. It is
  plausibility-gated and returns `-1` rather than a wrong number; `-1` costs
  `inspect kind='health'` its hung-vs-busy discrimination and nothing else.
- ~~**The cross-plugin C-ABI (`DevBenchAPI.h`) is Skyrim-typed.**~~ **Closed** — see
  "Registering your own tools" below. Builds on both platforms and is compile-checked by
  the extender-free unit-test target. Live on FO4VR, True Scopes (2026-08-26) and FRIK
  (2026-09-28) both register tools through it, over the `DevBench_GetApiFunction` export.

## Registering your own tools

Another F4SE plugin can add its own tools to this bench, so a mod's own operations show up
as MCP tools and REST endpoints next to the built-in ones. Vendor `include/DevBenchAPI.h`
and `include/DevBenchAPI.cpp` into your plugin (both MIT, independent of devbench's
GPL-3.0), then, once F4SE has sent your plugin `kPostLoad`:

```cpp
#include "DevBenchAPI.h"

if (auto* api = DevBenchAPI::GetDevBenchInterface001()) {
    api->RegisterTool("scope",
        R"({"description":"Read and drive the scope render.",
            "inputSchema":{"type":"object",
              "properties":{"action":{"type":"string","enum":["state","force"]}}}})",
        +[](void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write) {
            a_write(a_sink, R"({"ok":true})");
        },
        nullptr);
}
```

`DevBenchAPI.cpp` picks SKSE or F4SE automatically from whichever extender header your
include path can see; define `DEVBENCHAPI_GAME_FALLOUT4` (or `..._SKYRIM`) to force it.
Nothing else about the ABI differs between games — the same header, message id and vtable
serve both.

Two things that bite:

- **Your handler runs on devbench's listener thread, not the main game thread.** Marshal
  yourself before touching game state. This is unchanged from Skyrim, and it is the single
  most common way a registered tool crashes a game. On Fallout 4 VR, do NOT marshal
  through F4SE's `AddTask`/`AddUITask`: they run tasks on worker threads (see "Main
  thread"). Use devbench's own queue instead:

  ```cpp
  if (const auto* mt = DevBenchAPI::GetDevBenchMainThread()) {
      mt->RunOnMainThread(+[](void* a_ctx) { /* on the main thread, between frames */ }, ctx);
      // mt->ConfinedToMainThread(): true where devbench has established it (FO4VR 1.2.72)
  }
  ```

  It is a separate service from `IDevBenchInterface001`, found by its own message, or on
  Fallout the `DevBench_GetMainThreadApi` export. A devbench without it answers null
  rather than leaving a vtable slot to call blindly. `RunOnMainThread` returns at once;
  `ctx` must stay valid until the callback has run.

- **Check `GetBuildNumber()` before calling a late vtable slot.** `RegisterMenuHandler`
  needs `>= 10400`, `RegisterToolExtension` `>= 10500`. Calling a slot an older host does
  not have is not a graceful failure.

Registrations are visible at runtime through `inspect kind='registrants'`, which lists both
who requested the interface and what they registered through it. Each consumer carries a
`route`: `message` for the F4SE handshake, named by the extender's sender name, or `export`
for `DevBench_GetApiFunction`, named by the DLL that called it. The export takes no
arguments, so the caller is identified from its return address. Before that, a plugin on
the export route (FRIK is one) registered tools while `registrants` listed no consumer.

devbench registers one tool through the same interface itself: `devbench.selftest`, which
echoes its arguments, so the C-ABI round trip can be checked with no consumer mod
installed. It used to be named `ping`, and since the C-ABI replaces a tool of the same
name, it took over the built-in `ping` on every game. The same applies to your tools: a
name that matches a built-in replaces it, and `registrants` marks that registration
`replaced`.

`RegisterToolExtension("inspect", "<kind>", …)` adds a kind to the built-in `inspect` instead
of a new top-level tool: it appears in `inspect`'s schema, in `kind='extensions'`, and
`inspect kind='<kind>'` calls your handler. It's the natural home for state only your mod can
see, such as its own UI tree. On Fallout only `inspect` routes extensions so far; `menu` and
`capture` keys are recorded but not dispatched.

## Safety

Bound to `127.0.0.1`, always, and not configurable. There is no auth, and the bench can
run console commands and read process memory — correct for a local dev bench, unacceptable
on a reachable address. Memory _writes_ need the two gates above. `files` writes and deletes
only under the game's `Data/` and `Documents/My Games/<game>/`, and never writes native code
there.

A web page in the user's browser can reach `127.0.0.1` too, so every request is checked before
routing: one with an `Origin` header that is not a loopback page (`http(s)://localhost`,
`127.0.0.1` or `[::1]`, any port), or a `Host` header that is not one of those names, is a
`403`. That covers a cross-origin POST and DNS rebinding. Local tools (the bridge, curl,
PowerShell, Python) send no `Origin` and are unaffected.
