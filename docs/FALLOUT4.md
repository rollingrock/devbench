# devbench for Fallout 4 / Fallout 4 VR

One F4SE plugin that opens a local MCP + REST endpoint into a **running** Fallout 4, so an
AI agent, a test script, or you with `curl` can drive and measure the game without
alt-tabbing — and, on Fallout, without attaching a debugger to read engine memory.

The same DLL serves **Fallout 4** and **Fallout 4 VR**. It is the Fallout platform of
[devbench](../README.md); the tool surface, the transports, and the config file are shared
with the Skyrim build. See [MULTIGAME.md](MULTIGAME.md) for how the two fit together.

> **Status: builds, not yet run in-game.** The plugin compiles and exports the F4SE entry
> points; no endpoint has answered a live request yet. Treat the endpoint list below as
> the intended contract, not as a tested one.

## Install

Drop `devbench.dll` in `Data/F4SE/Plugins/`. On first run it writes a self-documenting
`Data/F4SE/Plugins/devbench/config.json`.

Default ports — distinct per game *and* runtime, so Skyrim and Fallout can be up at the
same time without either moving:

| | flat | VR |
| --- | --- | --- |
| Fallout 4 | **8930** | **8931** |
| Skyrim | 8920 | 8921 |

If the port is busy the server iterates upward and writes the port it actually bound to
`Data/F4SE/Plugins/devbench/runtime.json`.

## Build

```powershell
git clone --recursive <your fork>
$env:CommonLibF4Path = "C:\path\to\CommonLibF4"   # or leave it; extern/CommonLibF4 is a submodule
cmake --preset fallout4
cmake --build build --config Release --target devbench
```

Needs the **VR-capable** [rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4);
the upstream fork builds but cannot load in Fallout 4 VR. `vcpkg` supplies the rest.

To deploy on build, set `FalloutPluginTargets` to one or more game `Data` directories,
separated by `;` — the twin of the Skyrim build's `SkyrimPluginTargets`.

## Tools

Reachable over both MCP (`tools/call` on `/mcp`) and REST (`POST /api/tool/<name>`).

| Tool | What it does |
| --- | --- |
| `ping` | Self-test. Returns `{ ok, game, exe, vr }`. |
| `inspect` | Live game state, on the main thread, returned synchronously. `kind='state'` → `{ plugin, version, playerLoaded, frame, pid, port, exe, vr, game, extender }`. `'health'` → the same identity plus `{ frame, lastTaskFrame, pendingTasks, blocking }`, and it is the **only** kind answered *without* the main thread — so it still replies when a busy or hung main thread would 504. `'ui'` → `{ openMenus, messageBoxOpen, blocking }`, **also** answered without the main thread, for the same reason: `blocking` has to be trustworthy exactly when the game is stuck behind a modal, not just when it's healthy. `'scene'` → `{ position, cell, cellFormId, interior, worldspace?, gameHour, daysPassed }`. `'player'` → `{ formId, level, name }`. |
| `console` | Runs a console command on the main thread. **Fire-and-forget:** command output is not captured on Fallout yet, and the result says so rather than returning an empty line list. Also reports `blocked` — true if a modal was open when the command was submitted, since many state-machine commands (`coc`, …) silently no-op behind one instead of erroring. |
| `menu` | Detect and answer in-game menus — mirrors the Skyrim `menu` tool's shape. `action='list'` (default) → `{ openMenus, messageBoxOpen }`. `'describe'` → the active `MessageBoxMenu`'s `{ headerText, bodyText, buttons, modal }`. `'accept'` (`index`, default 0) → answers it by button index and closes it. `'open'`/`'close'` (`name`) → show/hide an engine menu via the UI message queue. |
| `memory` | Read / resolve / write process memory by address expression. See below. |
| `log` | `action='tail'` (default) returns the last N lines of a plugin log from `Documents/My Games/Fallout4[VR]/F4SE/`, optional `grep` substring; `action='list'` enumerates them. Any plugin's log, not just devbench's. |
| `rendertarget` | `action='list'` every render target the engine owns; `'stats'` copies one back and reports `{ nonFinitePct, darkPct, meanLuma, maxChannel }`; `'dump'` also writes a BMP. See below. |
| `measure` | Frame-time percentiles over a window: `{ fps, meanMs, minMs, p50Ms, p95Ms, p99Ms, maxMs, frames, missedTransitions }`. No engine hook. |

`memory`, `log`, `rendertarget` and `measure` are game-agnostic — they live in the core and
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

Dumps land in `Data/F4SE/Plugins/devbench/captures`. `maxDimension` (default 1024, `0` =
native) integer-downsamples so a 4K buffer stays openable; sampling is nearest-neighbour,
**not** averaged, because averaging hides the single-pixel artefact you are hunting.

**Read `nonFinitePct` before `darkPct`.** A buffer full of NaN displays as black but reads
as *bright* to any exponent-threshold test — a NaN's exponent is the largest possible, not
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
that turned out *not* to hold text answered `500` — indistinguishable from the server being
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

## Known gaps

- **Console output capture.** Skyrim fences a command between markers and slices
  `ConsoleLog`'s buffer. The Fallout equivalent is not wired up, so `console` is
  fire-and-forget.
- **GPU stage timers** (per-pass GPU+CPU ms) are not ported. They need instrumentation
  points, which means exposing them through the cross-plugin C-ABI so a mod can bracket
  its own passes. The ABI is no longer the blocker (it works on Fallout now); the
  instrumentation points still have to be designed.
- **`rendertarget` covers 2D colour targets only.** No depth/stencil, no cube maps. Target
  *names* are not reported: Fallout addresses targets by logical id through
  RenderTargetManager's remap table, a different index space from the physical slots
  enumerated here, and printing a name that might belong to a different buffer is worse
  than printing none.
- **`record` / `replay` / `scenario` / `capture` / `game` / `papyrus`** are Skyrim-only
  so far. They are mostly pure logic around a few engine calls (`Recording.cpp` is 1014
  lines with 11 game references), so porting them is bounded work, not a rewrite.
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
    open-menu *answers* no longer depend on the sink at all — they read
    `RE::UI::menuMap` under the engine's own `GetMenuMapRWLock()`, which is ground truth,
    still needs no main-thread hop, and is correct for menus that opened before devbench
    subscribed. `source` in the result says which one answered, so a `false` can be told
    apart from a blind instrument.
  - **`MessageBoxMenu::currentMessage` is at `+0xF8` on VR, not CommonLibF4's `+0xE8`**
    (a 0x10 base-class shift). `MessageBoxData`'s *own* layout is byte-identical on both
    binaries — `headerText` `+0x18`, `bodyText` `+0x28`, `buttonText` `+0x38` (a
    `BSTArray`: `capacity` at `+0x08`, **`size` at `+0x10`**, since `sizeof(BSTArray)` is
    0x18), `warningContext` `+0x50`, `callback` `+0x58`, `modal` `+0x64`. The wrong
    offset points at a run of small integers with no vtable, and dereferencing it took
    the process down. So `describe`/`accept` try both offsets, identify the winner by
    *content* (a vtable **and** a `bodyText` that reads as text), report which matched in
    `currentMessageOffset`, and on failure return a hex dump rather than crashing —
    a wrong offset here yields plausible-looking qwords, exactly as `core/gfx/Validate.h`
    warns for the render-target seam.
- **`game::CurrentFrame()` on VR** reads `BSGraphics::State::frameCount` at a measured
  image offset, since the VR address library does not cover that id. It is
  plausibility-gated and returns `-1` rather than a wrong number; `-1` costs
  `inspect kind='health'` its hung-vs-busy discrimination and nothing else.
- ~~**The cross-plugin C-ABI (`DevBenchAPI.h`) is Skyrim-typed.**~~ **Closed** — see
  "Registering your own tools" below. Builds on both platforms and is compile-checked by
  the extender-free unit-test target; **no live consumer has exercised it on Fallout yet.**

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
  most common way a registered tool crashes a game.
- **Check `GetBuildNumber()` before calling a late vtable slot.** `RegisterMenuHandler`
  needs `>= 10400`, `RegisterToolExtension` `>= 10500`. Calling a slot an older host does
  not have is not a graceful failure.

Registrations are visible at runtime through `inspect kind='registrants'`, which lists both
who requested the interface and what they registered through it.

## Safety

Bound to `127.0.0.1`, always, and not configurable. There is no auth, and the bench can
run console commands and read process memory — correct for a local dev bench, unacceptable
on a reachable address. Memory *writes* need the two gates above.
