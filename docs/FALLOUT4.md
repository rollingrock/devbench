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
| `inspect` | Live game state, on the main thread, returned synchronously. `kind='state'` → `{ plugin, version, playerLoaded, frame, pid, port, exe, vr, game, extender }`. `'health'` → the same identity plus `{ frame, lastTaskFrame, pendingTasks }`, and it is the **only** kind answered *without* the main thread — so it still replies when a busy or hung main thread would 504. `'scene'` → `{ position, cell, cellFormId, interior, worldspace?, gameHour, daysPassed }`. `'player'` → `{ formId, level, name }`. |
| `console` | Runs a console command on the main thread. **Fire-and-forget:** command output is not captured on Fallout yet, and the result says so rather than returning an empty line list. |
| `memory` | Read / resolve / write process memory by address expression. See below. |
| `log` | `action='tail'` (default) returns the last N lines of a plugin log from `Documents/My Games/Fallout4[VR]/F4SE/`, optional `grep` substring; `action='list'` enumerates them. Any plugin's log, not just devbench's. |

`memory` and `log` are game-agnostic and behave identically on the Skyrim build.

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
- **`record` / `replay` / `scenario` / `capture` / `game` / `papyrus` / `menu`** are
  Skyrim-only so far. They are mostly pure logic around a few engine calls
  (`Recording.cpp` is 1014 lines with 11 game references), so porting them is bounded
  work, not a rewrite.
- **`game::CurrentFrame()` on VR** reads `BSGraphics::State::frameCount` at a measured
  image offset, since the VR address library does not cover that id. It is
  plausibility-gated and returns `-1` rather than a wrong number; `-1` costs
  `inspect kind='health'` its hung-vs-busy discrimination and nothing else.
- **The cross-plugin C-ABI (`DevBenchAPI.h`) is Skyrim-typed.** A Fallout mod cannot
  register its own tools yet. The ABI is game-neutral; only the discovery handshake is not.

## Safety

Bound to `127.0.0.1`, always, and not configurable. There is no auth, and the bench can
run console commands and read process memory — correct for a local dev bench, unacceptable
on a reachable address. Memory *writes* need the two gates above.
