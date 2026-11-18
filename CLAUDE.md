# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

devbench is an in-process script-extender plugin that exposes a running game to AI agents (MCP) and scripts (REST) on one `127.0.0.1` port. One codebase serves Skyrim (SKSE) and Fallout 4 / Fallout 4 VR (F4SE, one DLL for both). This checkout is a fork (`origin` ArthurHub/devbench, `upstream` rollingrock/devbench). Day-to-day work here is the Fallout 4 VR side.

Read first: [docs/MULTIGAME.md](docs/MULTIGAME.md) (why the core/platform split, the seams), [docs/FALLOUT4.md](docs/FALLOUT4.md) (Fallout tools, main-thread facts, known gaps, C-ABI for mods). [README.md](README.md) describes the Skyrim plugin.

## Build and test

Two build systems, one per game. `src/core/` is buildsystem-neutral and compiled by both.

- **Fallout 4 / FO4VR**: CMake + vcpkg (`VCPKG_ROOT` must be set), needs the VR-capable rollingrock CommonLibF4 (`extern/CommonLibF4` submodule).
  ```
  cmake --preset fallout4-vs2026        # or fallout4-vs2022 / fallout4-ninja
  cmake --build --preset fallout4-vs2026 --target devbench
  ```
  Set `FalloutPluginTargets` (`;`-separated game `Data` dirs) to deploy on build.
- **Fallout release**: pushing a `fo4-vX.Y.Z` tag on `main` runs `release-fallout.yaml` (CI build with the
  `fallout4-vs2026` preset via `_build-fallout.yaml`, a draft GitHub Release with the F4SE archive). X.Y.Z must be
  `CMakeLists.txt`'s version: a Fallout release never bumps it (it is `GetBuildNumber()` too), the prefix only keeps
  the tag apart from Skyrim's `vX.Y.Z`. Details in `docs/FALLOUT4.md` "Releasing". Upstream's `release.yaml` /
  `_build.yaml` are left as is.
- **Skyrim**: xmake, `xmake` (default mode `releasedbg`). `SkyrimPluginTargets` deploys.
- **Unit tests** (host-independent, no game, no extender): `xmake build devbench-tests && xmake run devbench-tests`. Only xmake builds them; CMake has no test target. The runner has no filter, it runs every `TEST_CASE` (self-registering, `tests/test_framework.h`). New pure-logic TUs must be added by hand to the `devbench-tests` file list in `xmake.lua`.
- **HTTP tests** (Skyrim, against a running game; skipped when none is reachable): `pytest tests/http -v`, `DEVBENCH_URL` picks the port.
- **Bridge** (`bridge/`, TypeScript stdio MCP proxy that survives game restarts): `npm --prefix bridge run build`, `npm --prefix bridge run lint`.
- **Python client** (`clients/python/`, distribution `devbench-client`, import `devbench`, MIT): `pip install -e clients/python && pytest clients/python`. Unit tests run against the fake server in `clients/python/tests/fake_devbench.py`, no game. The `devbench` package must never import pytest (only the opt-in `devbench.testing` plugin may); a test checks it. The plugin (`src/devbench/testing/`, configured by a mod's `DEVBENCH = Config(...)`) is tested with pytester in-process sessions against that fake. Version comes from the `fo4-v*` release tags (setuptools-scm, `git describe --match fo4-v*`).
- **Formatting**: `pre-commit run --files <changed files>` (clang-format for C++, prettier for Markdown/JSON, eslint for the bridge, ruff for `clients/python/` only).

Default ports: Skyrim SE 8920, Skyrim VR 8921, Fallout 4 8930, Fallout 4 VR 8931. The `devbench-fo4vr` MCP server in this environment is a live FO4VR instance through the bridge; use it to check changes in game.

## Architecture

- **`src/core/` includes no script-extender header.** This is a hard invariant. The `devbench-tests` target links neither SKSE nor F4SE and compiles `HostApi.cpp` / `DevBenchAPI.h` on purpose so a regression breaks the build.
- **The core reaches the game only through free-function seams**, one definition per platform, resolved at link time (not an `IHost` vtable): `core/Host.h` (identity, data/log dirs, `RequestQuit`), `core/Log.h` (`dvb::dlog`, not `logs`, to avoid shadowing CommonLib's alias), `core/MainThread.h` (`RunAndWait`), `core/GameState.h` (`CurrentFrame`), plus the `core/gfx/Device.h` graphics seam (Fallout only so far) and the `core/VRHost.h` VR seam (OpenVR interfaces, controller indices, Reach's hand/node/yaw reads, the HMD node's position and axes for `pose`, the handedness and the raw wand slots).
- **VR input is shared core code.** `core/VRInput.cpp` (the `vrTrackedSet` Sequence manager, `press`, the OpenVR vtable hooks) serves both games through `core/VRHost.h`. Placing the hands is per game: `core/PoseControl.cpp` (`pose`, no calibration) answers on Fallout 4 VR and 501s on Skyrim; Skyrim's calibrated `reach` lives in `platform/skyrim/ReachControl.cpp`, routed by Skyrim's `input` handler, and answers 400 on Fallout. `handedness` (also in `PoseControl.cpp`) sets the game's handedness and confirms it by the wand slots swapping under a held pose; FO4VR sets it through the console's `setini`, since writing the setting object alone does not make the game follow. `press` and `pose` resolve `primary` / `offhand` to left / right first (`core/HandednessLogic.cpp`). `vrhost::PlatformActions()` adds each game's own actions to `capabilities`. A hand side is typed, never a bare bool: `core/Hand.h`'s `Hand` (a physical left / right controller) and `Role` (primary / offhand; the offhand drives the game's secondary wand slot), with `HandFor` / `RoleOf` between them; the handedness itself stays the `leftHanded` bool. The Python client's handedness is `vr.Handedness` (`"left"` / `"right"`); the `leftHanded` bool exists only on the wire. `openvr.h` is vendored in `extern/openvr/` (IVRSystem_019 / IVRCompositor_022) and excluded from clang-format. Terms are in `docs/GLOSSARY.md`.
- **`ToolRegistry` is the single source of truth.** `McpAdapter` and `RestAdapter` reflect it and contain no tool names. A tool registered once is on `/mcp` and `POST /api/tool/<name>`; its JSON Schema is both the MCP `inputSchema` and the REST docs. `EventBus` fans out notifications.
- **Tool registration** on Fallout (`src/platform/fallout4/main.cpp`): `RegisterCommonTools` (core: `ping`, `memory`, `log`, `files`, `quit`), `RegisterGfxTools` (core: `rendertarget`, `measure`; only for platforms with the gfx seam), then `RegisterGameTools` (`Tools_Fallout4.cpp`: `inspect`, `console`, `menu`; `Nodes_Fallout4.cpp`: `nodes`; `Input_Fallout4.cpp`: `input`, which only works on VR; `Player_Fallout4.cpp`: `player`, power armor).
- **Pure logic is split out for testing**: `src/core/*Logic.cpp` holds the testable part of a feature, the platform file makes the engine calls.
- **Handlers run on the listener thread.** Anything touching game state marshals with `MainThread::RunAndWait`, which returns the value synchronously. On FO4VR, F4SE's `AddTask`/`AddUITask` run tasks on worker threads, so devbench drains its own queue after `Main::Swap` (`src/platform/fallout4/MainThread.cpp`). Never marshal through F4SE's queues.
- **Cross-plugin C-ABI**: `include/DevBenchAPI.h` + `.cpp` (MIT, vendored into consumer mods; also packaged as the vcpkg port in `cmake/ports/devbench-api`). Mods register tools, inspect kinds and use devbench's main-thread service. FRIK, True Scopes, Immersive Flashlight and Mods Config register through it. Check `GetBuildNumber()` before using a late vtable slot.

## Conventions

- **Bridge tool fallback must stay in sync.** Changing a tool registration file (`CommonTools.cpp`, `HostApi.cpp`, `GfxTools.cpp`, `Tools_Fallout4.cpp`, `Nodes_Fallout4.cpp`, Skyrim `Tools.cpp`, ...) without updating `bridge/src/tools-fallback-fallout4.json` / `tools-fallback.json` fails CI (`bridge/scripts/check-tools-fallback-sync.mjs`). Regenerate against a live game with `node bridge/scripts/sync-tools-fallback.mjs http://127.0.0.1:8931`; a new core tool also goes in that script's `coreTools` allowlist.
- **VR struct offsets are not CommonLibF4's.** Fields can sit at a different offset on FO4VR (e.g. `MessageBoxMenu::currentMessage` is `+0xF8`, not `+0xE8`). Where an offset is uncertain, validate by content (vtable, readable text) and fail with a diagnostic rather than dereference blind; `core/gfx/Validate.h` and the `menu` tool show the pattern.
- **Docs record live verification.** `docs/FALLOUT4.md` states which tools and kinds were run in game, on which runtime and date, and what remains unverified. Keep it accurate when a change is checked live (or isn't).
- **Commit subjects** use a scoped prefix: `feat(api):`, `fix(fallout4):`, `docs(fallout4):`, `feat(gfx):`.
- Server stays bound to `127.0.0.1`; there is no auth.

## Agent skills

### Issue tracker

Issues are tracked as local markdown files under `docs/plans/<feature>/` (GitHub Issues is off on the fork and upstream). See `docs/agents/issue-tracker.md`.

### Triage labels

The five default roles, with label strings matching the role names (needs-triage, needs-info, ready-for-agent, ready-for-human, wontfix). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `docs/GLOSSARY.md` and `docs/adr/`. See `docs/agents/domain.md`.
