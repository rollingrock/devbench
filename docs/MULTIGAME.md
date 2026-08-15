# One devbench, several games

_Status: branch `feat/multigame-core` on a fork of `alandtse/devbench`. The Fallout 4
target builds; the Skyrim target's build change is mechanical and unverified. Written
2026-08-15._

This is the working note behind the split, and the thing to read before reviewing the
branch. It answers three questions in order: **why a fork and not a port**, **where the
line between "core" and "game" actually falls once you measure it**, and **what each side
brings to the merge**.

---

## 1. Why this is a fork of devbench and not a new project

The obvious way to get a devbench for Fallout 4 is to write one. That is roughly what
happened already — an in-plugin query server grew inside a Fallout 4 VR research mod
because the debug loop (rebuild → relaunch → headset on → screenshot → guess) cost about
ten minutes per question. It worked. It also would have become the fifth thing in this
ecosystem that does 90% of what another thing does, differently.

> "I really want to avoid the mistake we all made with game specific ports that diverge.
> The whole crashlogger vs buffout crashlogger is a nightmare to maintain."
> — alandtse

So: a git fork with full upstream history, a branch, and a diff intended to be read as a
PR. Nothing here is a rewrite of anything that already worked. Every Skyrim file moved
without a functional edit; where the core needed something from the game, a seam was
added rather than the call site changed.

## 2. Where the line actually falls

Not a guess — measured across the 1.14.0 tree, counting references to `RE::` / `SKSE::`
per file:

| Already game-agnostic | Lines | Game-coupled | Lines |
| --- | ---: | --- | ---: |
| `ToolRegistry`, `EventBus`, `ToolExtensions` | ~420 | `Tools.cpp` (107 game refs) | 2418 |
| `McpAdapter`, `RestAdapter`, `McpContent`, `Server` | ~525 | `Papyrus.cpp` | 599 |
| `Config`, `Autorun`, `Json`, `Ssim`, `RecordingsView` | ~680 | `GameEvents`, `ConsoleHook`, `InputHotkeys`, `main` | ~520 |
| `Recording` (11 refs / 1014 lines), `Capture` (5 / 727) | mostly portable | | |

The infrastructure was already common. The **tool bodies** are the game-specific part,
and even inside them the split is uneven: `Recording` and `Capture` are 98% pure logic
around a handful of engine calls. `Ssim.cpp` never mentioned the game at all.

The conclusion that shaped the branch: **the boundary is not "framework vs tools", it is
"plumbing + pure logic" vs "the ~20 engine calls a tool actually makes"**. So the split is
a directory move plus three seams, not an abstraction layer.

### The three seams

`src/core/` includes **no script-extender header** — that is a hard invariant, and the
unit-test target (which links neither CommonLibSSE-NG nor SKSE) is what keeps it honest.
Everything the core needs from a game arrives through:

```
core/Host.h        Identity{ game, extender, exe, version, vr }, DataDir(), LogDir()
core/Log.h         dvb::dlog -> a sink the platform installs at load
core/MainThread.h  RunAndWait / LastCompletedFrame / PendingTasks
core/GameState.h   CurrentFrame()
```

Free functions, one definition per platform, resolved at link time. **Not** an `IHost`
vtable — that choice is load-bearing: an interface object would have meant touching every
one of the ~3,500 lines of moved Skyrim code to thread a host reference through. This way
the moved files compile unchanged, which is what makes the diff reviewable.

Two details worth flagging in review:

- `dvb::dlog`, not `dvb::logs`. A `dvb::logs` would silently shadow CommonLibSSE's global
  `logs` alias inside every `namespace dvb` translation unit. That compiles everywhere and
  surprises somebody in six months.
- `host::DataDir()` is defined in the core (`Data/<extender>/Plugins/devbench`);
  `host::LogDir()` is defined per platform, because SKSE and F4SE resolve it differently
  and F4SEVR gets it wrong in a way every FO4VR plugin corrects by hand.

## 3. Adding a game, concretely

Fallout 4 support is four files:

| File | Lines | What it is |
| --- | ---: | --- |
| `platform/fallout4/Fallout4Host.cpp` | ~125 | the three seams |
| `platform/fallout4/MainThread.cpp` | ~105 | F4SE task marshalling (same failure taxonomy as Skyrim's) |
| `platform/fallout4/Tools_Fallout4.cpp` | ~190 | `inspect`, `console` |
| `platform/fallout4/main.cpp` | ~120 | F4SE entry point |

One DLL covers Fallout 4 **and** Fallout 4 VR, the way CommonLibSSE-NG covers SE/AE/VR:
CommonLibF4 (the [rollingrock fork](https://github.com/rollingrock/CommonLibF4), which is
the VR-capable one) resolves per-runtime addresses at load.

Default ports became per game **and** runtime, since two games can now be up at once:

| | flat | VR |
| --- | --- | --- |
| skyrim | 8920 | 8921 |
| fallout4 | 8930 | 8931 |

`InstanceIdentity` (shared by `GET /api/health` and `inspect kind='state'`) gained `game`
and `extender`. A port alone no longer identifies who replied.

### Tool shape is deliberately preserved

The Fallout `inspect` and `console` use the same names, the same action dispatch, and the
same result keys as the Skyrim ones wherever the concept exists in both games. An agent
script written against Skyrim should mostly work against Fallout. Where a concept does
**not** transfer, the shape differs rather than being faked — Fallout's `console` returns
`{ executed, command, note }` and says plainly that output capture is not wired yet,
instead of returning an empty `lines: []` that reads like "the command printed nothing".

## 4. What the Fallout side brings back

The point of merging rather than porting is that the traffic goes both ways. From the
Fallout 4 VR scope investigation's in-plugin bench, generalised and moved into
`src/core/tools/`:

### `memory` — read/resolve/write process memory by address expression

```
expr := term (('+' | '-') term)*
term := '[' expr ']' | 'base' | 0xHEX | DEC
```

`base` is the executable's load base; `[x]` dereferences a qword. An RVA out of a Ghidra
session pastes in verbatim:

```
memory action='resolve' addr='base+0x1d98ff0'
memory addr='[base+0x6239340]+4' type='u8' count=8
```

This is the half of a debugging loop that a gameplay-oriented bench cannot reach, and it
is what turns "attach x64dbg, find the struct, read eight bytes, detach" into one call an
agent can make unattended. Four decisions in it are worth keeping in any review:

1. **Every access is SEH-guarded.** A bad address is a `400` that *names the faulting
   address*, never a CTD. A probe whose only outcomes are "the answer" and "failed" cannot
   tell a wrong pointer from an absent one.
2. **RVAs are reported only for addresses genuinely inside the image.** A heap pointer
   gets `"heap": true` instead. An RVA for a heap object pastes into Ghidra as a
   valid-looking lie — this was a real bug, caught live, where a camera object at
   `0x1ec2ea480` was being labelled `rva=0xac2ea480`.
3. **NaN and Inf are emitted as the strings `"NaN"`/`"Inf"`, never JSON `null`.** A NaN in
   a light-accumulation buffer survived five debugging sessions and eliminated nine
   innocent suspects partly because the instruments reported it as nothing to see.
4. **Writes echo `{ before, after, held }`.** "The write succeeded" and "the value
   changed" are different claims: a write to a page the engine rewrites every frame looks
   identical to a no-op unless both are printed. `before` is also the caller's undo.
   Gated twice — `allowMemoryWrites` in config **and** `confirm=true` per request.

### `log` — tail/grep any plugin's log

Not just devbench's. The mod being debugged is usually not devbench.

### Still to bring over (not in this branch)

The Fallout bench also carries a **D3D11 graphics-debug** toolkit that has no equivalent
upstream and is, on inspection, almost entirely game-agnostic:

- GPU timestamp-query **stage timers** (per-stage GPU *and* CPU ms, disjoint-discard
  handling) — this is what a shader A/B actually needs, and it complements record/replay
  exactly: replay gives you the same scene twice, stage timers tell you what changed.
- **Render-target readback and full-surface dumps** with a multi-format decoder
  (`R16G16_UNORM`, `R11G11B10`, F16 — the formats a G-buffer actually uses), plus
  per-buffer NaN percentage and NaN-highlighting output.
- `PixelNonFinite` / `PixelDark` classification. The lesson embedded in that pair is the
  one worth transplanting: a "dark pixel" test that checks `exponent < 12` classifies NaN
  as *bright*, which is why 12,000 readbacks across five sessions all reported "0 dark"
  while the screen was visibly black.

Given `Ssim.cpp` and the golden-image replay work already upstream, the natural join is:
**replay puts the camera in the same place, capture+SSIM says the image changed, stage
timers and buffer dumps say why.** That is a full shader-regression loop and neither half
has it alone.

## 5. What is unverified

Stated plainly, because a branch that claims more than it has tested is worse than one
that claims less:

- ✅ **The Fallout 4 target configures, compiles and links.** `devbench.dll` exports
  `F4SEPlugin_Query` / `F4SEPlugin_Load`.
- ❌ **The Fallout plugin has not been loaded in the game.** No endpoint has answered a
  live request.
- ❌ **The Skyrim xmake target has not been built since the move.** The change is
  mechanical — recursive glob (unchanged), two `add_includedirs`, the PCH path, the
  `RecordingsMenu` paths, and `remove_files("src/platform/fallout4/**.cpp")` — but
  mechanical is not the same as verified, and CI is the right place to find out.
- ⚠️ **`game::CurrentFrame()` on Fallout 4 VR** reads `BSGraphics::State::frameCount` at a
  measured image offset, because the VR address library does not cover that id. It is
  plausibility-gated and returns `-1` rather than a garbage number when the gate fails;
  `-1` degrades `/api/health` to "no frame signal" instead of misreporting one. The
  flat-rim path is address-library backed and fine.
- ⚠️ **Fallout `console` output capture** is not implemented (Skyrim's fencing trick has no
  wired-up equivalent yet). The tool says so in its own description.

## 6. Suggested review order

1. This document.
2. `src/core/Host.h`, `src/core/Log.h` — the whole seam, ~110 lines.
3. `git log -p --follow` on any one moved file, to confirm the moves are functionally
   empty.
4. `src/platform/fallout4/` — what a new game actually costs.
5. `src/core/tools/` — the new capability, and the part most worth arguing about.

## 7. Open questions for the merge

- **Naming.** `RegisterCoreTools` in `platform/skyrim/Tools.cpp` now registers *game*
  tools while `tools::RegisterCommonTools` registers the core ones. That is backwards and
  should be renamed; it was left alone here to keep the diff about the split.
- **`DevBenchAPI.h` is Skyrim-typed** (`#include <RE/Skyrim.h>`, `SKSE::` messaging). The
  C-ABI itself is game-neutral — only the discovery handshake is not. Options: per-game
  headers sharing one ABI, or a small game-neutral header plus a per-game shim. Worth
  settling before Fallout consumers exist, not after.
- **Should the core become its own repo / vcpkg port?** Argument for: Starfield next, and
  `devbench-api` is already a port. Argument against: one repo is exactly what stops the
  divergence this branch exists to prevent. Recommendation is to stay one repo until a
  third game actually lands.
- **Does the graphics toolkit (§4) belong in the core, or in an optional module?** It
  drags in D3D11. Leaning: core, behind a capability flag — the Skyrim shader work wants
  it too.
