#include "GameEvents_Fallout4.h"

#include "core/EventBus.h"
#include "core/Json.h"

#include <mutex>
#include <unordered_set>

// Mirrors src/platform/skyrim/GameEvents.cpp's menu tracking. Written for
// DEVBENCH_REQ_MENU_STATE.md (fallout4-scope-in-scope-investigation): an in-game
// modal (e.g. the "this save relies on content that is no longer present" load
// warning) is otherwise indistinguishable from a healthy game over every existing
// signal (frame count, pendingTasks, playerLoaded all read fine while it blocks
// `coc` and every other state-machine command).
//
// TWO sources, deliberately, after the first field test:
//
//   * RE::UI::menuMap, read under the engine's own BSReadWriteLock — GROUND TRUTH.
//     It reports what is open RIGHT NOW regardless of when we subscribed, needs no
//     main-thread hop (the RW lock is what makes concurrent reads legal), and is
//     therefore correct even for a menu that opened before this plugin existed.
//   * the MenuOpenCloseEvent sink — kept for the "menu" event stream, and as a
//     fallback for the window where RE::UI is not up yet.
//
// The first version had ONLY the sink, installed at kPostLoad, and the field test
// showed why that is not enough: a full cell transition (which opens a LoadingMenu)
// produced no event at all, so `blocking:false` was not a measurement — it was a
// blind instrument answering the same way whether or not anything was open. Which
// source answered is now reported to the caller rather than left to be assumed.

namespace dvb
{
	namespace
	{
		EventBus* g_bus = nullptr;

		std::mutex                      g_menuMutex;
		std::unordered_set<std::string> g_openMenus;  // sink-tracked fallback set
		bool                            g_sinkInstalled = false;

		// CommonLibF4's BSTEventSink takes the event by const reference, not pointer
		// (CommonLibSSE-NG's Skyrim template takes a pointer) — the one API shape
		// difference from the Skyrim sink this mirrors.
		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				const std::string name = a_event.menuName.c_str() ? a_event.menuName.c_str() : std::string{};
				if (!name.empty()) {
					std::lock_guard lock(g_menuMutex);
					if (a_event.opening)
						g_openMenus.insert(name);
					else
						g_openMenus.erase(name);
				}
				if (g_bus)
					g_bus->Publish("menu", json{ { "name", name }, { "opening", a_event.opening } });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;
	}

	bool InstallGameEvents(EventBus& a_bus)
	{
		g_bus = &a_bus;
		if (g_sinkInstalled)
			return true;
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			// Not fatal, and no longer silent: menuMap reads are the primary source, so
			// this costs the event stream, not the `blocking` answer.
			logs::info("devbench: RE::UI not up yet — MenuOpenCloseEvent sink not installed (will retry)");
			return false;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuSink);
		g_sinkInstalled = true;
		logs::info("devbench: MenuOpenCloseEvent sink installed");
		return true;
	}

	bool MenuEventsInstalled()
	{
		return g_sinkInstalled;
	}

	const char* MenuStateSource()
	{
		return RE::UI::GetSingleton() ? "menuMap" : (g_sinkInstalled ? "events" : "none");
	}

	std::vector<std::string> GetOpenMenus()
	{
		// Ground truth: the engine's own menu map, under the engine's own read lock.
		// An entry is reported only if it has a live menu that is OnStack() — the same
		// test UI::GetMenuOpen uses — so a registered-but-closed menu is not counted.
		if (auto* ui = RE::UI::GetSingleton()) {
			std::vector<std::string> out;
			RE::BSAutoReadLock       l{ RE::UI::GetMenuMapRWLock() };
			for (const auto& entry : ui->menuMap) {
				const auto* name = entry.first.c_str();
				if (!name || !*name)
					continue;
				const auto& menu = entry.second.menu;
				if (menu && menu->OnStack())
					out.emplace_back(name);
			}
			return out;
		}
		std::lock_guard<std::mutex> lock(g_menuMutex);
		return { g_openMenus.begin(), g_openMenus.end() };
	}

	bool IsMessageBoxOpen()
	{
		if (auto* ui = RE::UI::GetSingleton())
			return ui->GetMenuOpen(RE::MessageBoxMenu::MENU_NAME);
		std::lock_guard<std::mutex> lock(g_menuMutex);
		return g_openMenus.contains(std::string(RE::MessageBoxMenu::MENU_NAME));
	}
}
