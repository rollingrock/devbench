#pragma once

#include <filesystem>
#include <string>

// The platform seam.
//
// Everything the game-agnostic core needs to know about the game it happens to be
// running inside. Each platform (src/platform/<game>) links exactly one definition
// of these functions; the core only ever calls them. Deliberately a handful of free
// functions rather than an interface object: the core is a static library linked
// into exactly one plugin, so a link-time seam costs nothing and keeps the moved
// Skyrim sources compiling unchanged.
//
// Keep this SMALL. Anything a game can express as a *tool* belongs in
// src/platform/<game>, registered through ToolRegistry — not here. This header is
// only for things the core's own plumbing (config paths, identity, main-thread
// marshalling) cannot do without.

namespace dvb::host
{
	/// Which game answered. Reported by `inspect kind='state'`, `GET /api/health`, and
	/// the MCP server info, so a client talking to several running games can tell them
	/// apart (devbench#16) — and now also tell Skyrim from Fallout.
	struct Identity
	{
		std::string game;      ///< "skyrim" | "fallout4" | "starfield"
		std::string extender;  ///< "SKSE" | "F4SE" | "SFSE" — also the Data/<x>/Plugins folder
		std::string exe;       ///< basename of the running executable, e.g. "Fallout4VR.exe"
		std::string version;   ///< devbench build version string
		bool        vr = false;
	};

	/// Process-constant; safe to call from any thread once the platform has installed it.
	const Identity& Get();

	/// Where devbench keeps config.json, runtime.json, recordings, and captures —
	/// `Data/<extender>/Plugins/devbench`. Relative to the game's working directory,
	/// matching how every other plugin resolves its data.
	std::filesystem::path DataDir();

	/// Called once by the platform at plugin load, before Config::Load or Server::Start.
	void Install(Identity a_identity);

	/// Where the script extender writes its logs — `Documents/My Games/<game>/<extender>`.
	/// DEFINED BY THE PLATFORM (the extenders each have their own resolver, and they do
	/// not agree on the layout), unlike the three above which the core defines itself.
	/// Empty when the extender cannot resolve it. Used by the `log` tool.
	std::filesystem::path LogDir();
}
