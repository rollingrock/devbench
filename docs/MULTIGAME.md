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

### `rendertarget` — look inside the renderer

The capability with no equivalent upstream, and the one that is genuinely hard to
rebuild from scratch. A screenshot shows the final image; this shows the buffers that
*produced* it — the G-buffer, the light accumulation, the shadow map — which is where a
rendering bug actually lives.

```
rendertarget action='list'                          -> every live target: index, size, format, decodable
rendertarget action='stats' index=12                -> nonFinitePct, darkPct, meanLuma, maxChannel
rendertarget action='dump'  index=12 label='before' -> the same, plus a BMP on disk
```

The platform seam it sits on is three functions (`core/gfx/Device.h`): device, context,
and "enumerate the targets the renderer owns, by engine index". Everything else —
readback, decode, analysis, dumping — is game-agnostic. Skyrim can have this by writing
those three functions.

Four decisions in it are load-bearing, each paid for:

1. **`IsNonFinite` and `IsDark` are separate calls, and a test enforces it.** A NaN
   displays black but has the *largest possible* exponent, so an exponent-threshold
   "is it dark" test classifies NaN as **bright**. That is not a hypothetical: 12,000+
   readbacks across five sessions all reported "0 dark" against a visibly black screen,
   and nine innocent suspects were eliminated against that blind instrument.
   `tests/Format_test.cpp` asserts the trap — `IsDark` must *not* catch NaN — so the two
   can never be helpfully merged back together.
2. **Non-finite pixels are painted MAGENTA in dumps, not clamped to white.** Clamped, a
   NaN-filled buffer is indistinguishable from a legitimately overbright one; the first
   dump session read as "blown out" for exactly that reason.
3. **An unknown DXGI format is refused before anything is mapped.** The original had a
   catch-all that assumed 8 bytes per pixel, so a 4-byte surface was read at twice its
   row length and ran off the end of the mapped staging texture. That crash was blamed on
   an unrelated render step for a session.
4. **Downsampling is nearest-neighbour, never averaged.** Averaging hides the
   single-pixel NaN and the one-pixel seam that are the reason you are looking.

Also landed: `R16G16_UNORM` decode (the G-buffer normals format), which had no case in the
original table — so that buffer was silently skipped and had never once been looked at.
It renders as R=x, G=y, B=0 with no reconstructed Z, because guessing the encoding
produces a plausible image that lies.

### `measure` — frame-time percentiles, no hook

`{ fps, meanMs, minMs, p50Ms, p95Ms, p99Ms, maxMs, frames, missedTransitions }` over a
window. This is the ROADMAP's "**`measure` primitive** — sample frametime over a window →
min/avg/p95/p99 (the benchmark primitive)", and it needs **no engine hook**: it watches
the frame counter the platform already exposes and timestamps each change with QPC.

It reports its own limitations rather than hiding them — `missedTransitions` when the
counter jumped by more than one, the sampling method in every result, and a 503 (not a
silent zero) on a runtime with no frame counter. It busies one core for the window, so
there is deliberately no background mode.

**Together with what is already upstream, that closes the loop:** `replay` puts the camera
in the same place, `capture` + `Ssim` says the image changed, `measure` says what it cost,
and `rendertarget` says which buffer went wrong. No half of that is a regression harness
on its own.

### Still to bring over

- **GPU timestamp stage timers** — per-stage GPU *and* CPU ms with disjoint handling.
  Deferred deliberately: it needs instrumentation points, which means exposing it through
  the C-ABI so a mod can bracket its own passes. The C-ABI is no longer the blocker (§7 —
  it is game-neutral now); the instrumentation points still have to be designed, and a
  `gputimer` tool with nothing registered would be vaporware.
- **Depth/stencil and cube targets.** Only 2D colour targets are enumerated.
- **Named targets on Fallout.** The engine addresses targets by *logical* id through
  RenderTargetManager's remap table, a different index space from the physical slots.
  `TargetName()` returns "" rather than a name that might belong to a different buffer.

## 5. What is unverified

Stated plainly, because a branch that claims more than it has tested is worse than one
that claims less:

- ✅ **The Fallout 4 target configures, compiles and links** at `/W4` with zero warnings.
  `devbench.dll` exports `F4SEPlugin_Query` / `F4SEPlugin_Load`.
- ✅ **The format decoder is unit-tested** — 8 cases, all passing, including the NaN
  blind-spot regression.
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
- ⚠️ **The renderer struct offsets have not been confirmed against a running game.** They
  agree from two independent directions (CommonLibF4's flat-rim layout, and a live
  x64dbg measurement on VR — `0x10 + 0x0A58 + 0x10 == 0xA78` exactly), and
  `Device_Fallout4.cpp` validates the device, the context and a sample of the target
  array before publishing anything, returning null rather than a plausible wrong
  pointer. But agreement is not the same as a live `rendertarget action='list'`.

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
- ~~**`DevBenchAPI.h` is Skyrim-typed**~~ — **SETTLED, and it turned out not to need a
  design.** The option taken was neither per-game headers nor a shim: the header simply
  stopped including an extender. Every declaration in it (message id, function-pointer
  types, vtable) was already plain C++; `<RE/Skyrim.h>` + `<SKSE/SKSE.h>` were only there
  for `GetDevBenchInterface001`, which lives in the *consumer-only* `DevBenchAPI.cpp` —
  and SKSE and F4SE spell that one dispatch identically, so that file picks its extender
  with `__has_include` plus a `DEVBENCHAPI_GAME_*` override.

  The provider side moved with it: `HostApi.{h,cpp}` are now **core**, taking the three
  message fields they use (`type, data, sender`) instead of an
  `SKSE::MessagingInterface::Message*`. Each platform's listener does a three-line unpack.
  Two knock-on details worth knowing:
  - `GetBuildNumber()` is now passed in at `HostApi::Init` rather than read from the
    generated `Version.h`, so the core keeps compiling with no configured headers.
    `DEVBENCH_BUILD_NUMBER` is defined once in `Version.h.in` so the two platforms cannot
    drift.
  - **`src/core/HostApi.cpp` is compiled by the `devbench-tests` target purely to enforce
    the no-extender invariant.** It needed `tests/PlatformStubs.cpp` for
    `game::CurrentFrame`. Without that, a re-coupling would have been invisible: both real
    plugin targets have an extender on the include path via their PCH and would have kept
    building.
- **Should the core become its own repo / vcpkg port?** Argument for: Starfield next, and
  `devbench-api` is already a port. Argument against: one repo is exactly what stops the
  divergence this branch exists to prevent. Recommendation is to stay one repo until a
  third game actually lands.
- **Should the graphics tools be conditional?** They are in the core and registered only
  by a platform that implements `core/gfx/Device.h` — Fallout does, Skyrim does not yet,
  and an unimplemented platform simply does not advertise them (better than tools that
  always answer 503). Skyrim gets them by writing three functions. The alternative — an
  optional module — buys little, since D3D11 is a system library.
