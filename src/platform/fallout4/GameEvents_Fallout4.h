#pragma once

#include <string>
#include <vector>

namespace dvb
{
	class EventBus;

	/// Names of currently-open menus. Read from RE::UI::menuMap under the engine's own
	/// BSReadWriteLock — no main-thread hop, which is the whole point: it is still
	/// correct while the game's state machine is stuck behind a modal (`frame` keeps
	/// advancing and `coc` silently no-ops, so every other signal reads healthy). See
	/// DEVBENCH_REQ_MENU_STATE.md in the fallout4-scope-in-scope-investigation repo for
	/// the observed failure this closes.
	///
	/// Reading the map rather than only tracking MenuOpenCloseEvent is deliberate: the
	/// map is correct for menus that opened before this plugin subscribed, and the first
	/// field test showed the sink alone reporting an empty set through a cell transition
	/// that certainly opened a LoadingMenu. Falls back to the sink-tracked set only when
	/// RE::UI is not up yet; MenuStateSource() says which answered.
	std::vector<std::string> GetOpenMenus();

	/// True iff RE::MessageBoxMenu is open — the engine's modal message box
	/// (confirmations, the "content no longer present" load warning, ...). Cheap,
	/// thread-safe, no main-thread hop.
	bool IsMessageBoxOpen();

	/// "menuMap" (ground truth), "events" (sink fallback, RE::UI not up), or "none".
	/// Reported to callers so a `false` can be told apart from a blind instrument.
	const char* MenuStateSource();

	/// Whether the MenuOpenCloseEvent sink is installed (affects the "menu" event
	/// stream only — the open-menu answers above do not depend on it).
	bool MenuEventsInstalled();

	/// Install game event sources that publish into `a_bus`:
	///  - a MenuOpenCloseEvent sink → "menu" events { name, opening }.
	/// Idempotent, and safe to call before RE::UI exists: returns false in that case so
	/// the caller can retry at a later lifecycle message. Do NOT assume RE::UI is up at
	/// kPostLoad — that assumption is what the first field test disproved.
	bool InstallGameEvents(EventBus& a_bus);
}
