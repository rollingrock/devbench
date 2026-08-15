#pragma once

#include <string>
#include <vector>

namespace dvb
{
	// Headless startup config (devbench has no in-game menu yet). Read once at
	// kPostLoad, before the server starts. Bind address is intentionally fixed to
	// 127.0.0.1 and not configurable.
	struct Config
	{
		bool        enabled = true;     ///< start the MCP/REST server at all
		int         port = 8920;        ///< localhost port for /mcp and /api (default SE/AE 8920, VR 8921 — LoadConfig)
		std::string logLevel = "info";  ///< trace|debug|info|warn|error (spdlog level)

		// In-game hotkeys (DXScanCode; 0 = disabled). Let recording/replay run with no
		// MCP/REST client connected — the standalone-benchmark path. Ignored while the
		// console is open so they don't fire on keystrokes typed into it.
		int         recordHotkey = 0;           ///< toggle record start/stop (DXScanCode)
		int         replayHotkey = 0;           ///< replay a recording (see replayPath)
		bool        recordHotkeyShift = false;  ///< require Shift held with recordHotkey
		bool        replayHotkeyShift = false;  ///< require Shift held with replayHotkey
		std::string replayPath = "";            ///< replay target; empty = most recent recording
		bool        replayRestoreScene = true;  ///< replay hotkey re-establishes the recorded scene
		int         recordIntervalMs = 10;      ///< default record sample interval (ms; min 10); per-call intervalMs overrides

		// Autorun: replay a recording once on the first load of the session — a fully
		// unattended benchmark with no client and no keypress. Empty = off.
		std::string autoRunPath = "";            ///< recording to replay on first postLoadGame
		bool        autoRunRestoreScene = true;  ///< autorun loads the recording's entry save first

		// Settle delay (ms) inserted after a restore-load before the replayed trajectory runs.
		// Teleporting the player the instant a load finishes (cells/physics/AI not settled) is
		// crash-prone. This is LOCAL/per-machine (settle time is hardware-dependent), not baked
		// into the portable recording. A replay call may override it with a settleMs arg.
		int loadSettleMs = 3000;

		// Scene coupling: how strictly a replay must reproduce the recorded entry, chosen by
		// how long before record-start devbench brokered the save/coc (stored per recipe as
		// entryPoint.ageMs). A save staged seconds before recording was deliberate; one from
		// minutes ago was incidental. Tiers (a recipe may override in its meta.coupling block):
		//   age <= anchorMs : "anchored" — restore the entry + re-apply recorded time/weather.
		//   age <= cellMs    : "cell"     — restore the entry.
		//   else             : "worldspace" — skip the entry restore; only assert the worldspace.
		int couplingAnchorMs = 10000;
		int couplingCellMs = 60000;

		// A raw coc/cow can stream between scenes without the full loading-screen teardown some
		// mods rely on to free resources, which can CTD. When true, a coc/cow restore first
		// bounces through couplingTransitionCell (a known-present interior) to force a clean
		// loading screen. Save-loads already tear down, so they skip the bounce.
		bool        cleanTransition = true;
		std::string cleanTransitionCell = "QASmoke";

		// `capture` tool: where captured images are written, where the native (vanilla) fallback
		// looks for the screenshot it just triggered, and default timeouts. captureScanDirs are
		// relative to the game install root (GetModuleFileNameW's parent dir — vanilla screenshots
		// and Open Shaders' own default capture path both resolve there, NOT Documents/My Games,
		// which is a different, SKSE-log-specific convention); "" means the root itself.
		/// Empty = `host::DataDir()/captures` (i.e. Data/<extender>/Plugins/devbench/captures),
		/// resolved at load so the default follows the game rather than hardcoding SKSE.
		std::string              captureDir = "";
		std::vector<std::string> captureScanDirs = { "", "Screenshots" };
		int                      captureTimeoutMs = 8000;
		int                      captureSettleMs = 500;  // default settle before a checkpoint capture

		// Background watchdog: if the engine frame counter hasn't advanced for this long,
		// publish a "health.stalled" EventBus event (and "health.resumed" once it recovers) —
		// covers a frozen main thread, which a `menu`/`lifecycle` event never can (those are
		// published BY the main thread). 0 disables the watchdog.
		int stallWatchdogMs = 5000;

		// `memory action='write'` gate. Reading process memory is inherent to a dev
		// bench; writing it is a different risk class (a typo'd address pokes the live
		// engine), so it is opt-in per install AND needs confirm=true per request.
		bool allowMemoryWrites = false;
	};

	// The port used when config.json does not pin one. Deterministic per runtime so a
	// fixed MCP client URL never moves, and distinct per game so two games running at
	// once do not collide:
	//   skyrim   SE/AE 8920, VR 8921
	//   fallout4 F4    8930, VR 8931
	//   (a game the core does not know about falls back to 8920)
	// Register one client entry per runtime; the game that is off just shows disconnected.
	int DefaultPort();

	// Load <DataDir>/config.json. If the file is missing it is auto-created with the
	// current defaults (so users/agents discover the keys); a parse error → defaults
	// (logged). Never throws.
	Config LoadConfig();

	// Persist just the four hotkey keys into config.json, preserving every other key. Written from
	// the render thread (in-menu rebind), so it writes a temp file then renames it over the target
	// (atomic replace) — a concurrent reader never sees a half-written file. Never throws.
	void SaveHotkeys(int a_recordKey, bool a_recordShift, int a_replayKey, bool a_replayShift);
}
