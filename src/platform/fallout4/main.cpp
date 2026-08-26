#include "GameEvents_Fallout4.h"
#include "Tools_Fallout4.h"

#include "core/Config.h"
#include "core/GameState.h"
#include "core/HostApi.h"
#include "core/Server.h"
#include "core/tools/CommonTools.h"
#include "core/tools/GfxTools.h"

#include "Version.h"

#include <memory>
#include <set>
#include <spdlog/sinks/basic_file_sink.h>

// Load-order-proof interface entry (see HostApi::GetApiEntry). A consumer does
// GetModuleHandle("devbench.dll") + GetProcAddress("DevBench_GetApiFunction"),
// calls it, and receives the same GetApi the message handshake would deliver.
extern "C" __declspec(dllexport) void* DevBench_GetApiFunction()
{
	return dvb::HostApi::GetApiEntry();
}

// devbench, Fallout 4 / Fallout 4 VR entry point.
//
// One DLL serves both runtimes: CommonLibF4 (rollingrock fork) resolves per-runtime
// addresses at load, exactly as CommonLibSSE-NG does for Skyrim SE/AE/VR, so there is
// no separate VR build to keep in step.

namespace
{
	std::unique_ptr<dvb::Server> g_server;

	void InitLogging()
	{
		auto path = F4SE::log::log_directory();
		if (!path)
			return;
		// F4SEVR's log_directory can come back pointing at the wrong game folder; the
		// same correction every FO4VR plugin carries.
		const auto gamePath = REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
		if (!path->generic_string().ends_with(gamePath))
			path = path->parent_path().append(gamePath);

		*path /= "devbench.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%^%L%$] %v");
	}

	void ApplyLogLevel(const std::string& a_level)
	{
		const auto lvl = spdlog::level::from_str(a_level);  // unknown string → 'off'; guard that
		const auto resolved = (lvl == spdlog::level::off && a_level != "off") ? spdlog::level::info : lvl;
		if (resolved == spdlog::level::off && a_level != "off")
			logs::warn("devbench: unknown logLevel '{}' — keeping info", a_level);
		spdlog::set_level(resolved);
		spdlog::flush_on(resolved);
	}

	void StartServer()
	{
		if (g_server)  // one server per process, whichever message got here first
			return;

		const dvb::Config cfg = dvb::LoadConfig();
		ApplyLogLevel(cfg.logLevel);
		if (!cfg.enabled) {
			logs::info("devbench: server disabled via config; not starting");
			return;
		}

		dvb::tools::SetAllowMemoryWrites(cfg.allowMemoryWrites);

		g_server = std::make_unique<dvb::Server>("127.0.0.1", cfg.port);
		g_server->Events().SetFrameProvider(&dvb::game::CurrentFrame);
		// Tools are registered BEFORE Start() so they appear on both transports from
		// the very first request rather than racing a client that connects instantly.
		dvb::tools::RegisterCommonTools(g_server->Tools(), g_server->Events());
		// Fallout implements the graphics seam (Device_Fallout4.cpp), so the graphics
		// tools are advertised here. A platform without that seam must not register them.
		dvb::tools::RegisterGfxTools(g_server->Tools(), g_server->Events());
		dvb::RegisterGameTools(g_server->Tools(), g_server->Events());
		// Cross-plugin C-ABI: wire the provider to this host's registry + bus BEFORE
		// Start(), so a consumer that registers a tool the instant it gets the interface
		// finds a live registry rather than racing the server's first request.
		dvb::HostApi::Init(g_server->Tools(), g_server->Events(), DEVBENCH_BUILD_NUMBER);
		g_server->Start();
		// Try here so the "menu" event stream starts as early as it can, but do NOT
		// assume it succeeds: RE::UI is NOT up at kPostLoad on FO4VR. The first version
		// of this asserted it was, swallowed the null, and shipped a detector that
		// reported an empty menu set through a cell transition — see the retry at
		// kGameDataReady below. (Same shape as the Skyrim platform's kInputLoaded note:
		// BSInputDeviceManager is null at kPostLoad and registering then silently
		// no-ops.) The open-menu ANSWERS do not depend on this — they read
		// RE::UI::menuMap directly — so a false here costs events, not correctness.
		dvb::InstallGameEvents(g_server->Events());
	}

	// Listener for messages from ANY plugin. The default MessageHandler is registered
	// with sender "F4SE" and therefore only receives F4SE's own messages, so a consumer
	// mod's interface dispatch would never reach us without this second registration.
	//
	// A default-constructed zstring is a string_view with data() == nullptr, which is how
	// "any sender" is spelled at the F4SE plugin-manager boundary. It is NOT written as
	// zstring{nullptr}: constructing a string_view from a null pointer runs strlen on it.
	void OnInterfaceMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (a_msg)
			dvb::HostApi::OnInterfaceRequest(a_msg->type, a_msg->data, a_msg->sender);
	}

	// Register the any-sender listener. Called TWICE on purpose — see the block comment
	// on kPostLoad in MessageHandler. F4SE's plugin manager de-duplicates by listener
	// handle, so the second call adds only the lists the first one could not reach.
	void RegisterAnySenderListener(const char* a_when)
	{
		auto* messaging = F4SE::GetMessagingInterface();
		if (!messaging)
			return;
		if (!messaging->RegisterListener(OnInterfaceMessage, F4SE::stl::zstring{}))
			logs::warn("devbench: could not listen for cross-plugin interface requests at {} — "
					   "other plugins may not be able to register tools",
				a_when);
	}

	const char* MessageName(std::uint32_t a_type)
	{
		using M = F4SE::MessagingInterface;
		switch (a_type) {
		case M::kPostLoad:      return "kPostLoad";
		case M::kPostPostLoad:  return "kPostPostLoad";
		case M::kPreLoadGame:   return "kPreLoadGame";
		case M::kPostLoadGame:  return "kPostLoadGame";
		case M::kPreSaveGame:   return "kPreSaveGame";
		case M::kPostSaveGame:  return "kPostSaveGame";
		case M::kDeleteGame:    return "kDeleteGame";
		case M::kInputLoaded:   return "kInputLoaded";
		case M::kNewGame:       return "kNewGame";
		case M::kGameLoaded:    return "kGameLoaded";
		case M::kGameDataReady: return "kGameDataReady";
		default:                return "?";
		}
	}

	void MessageHandler(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg)
			return;

		// One line per lifecycle message, once each. F4SEVR delivers kPostLoad and
		// kPostPostLoad from the plugin manager itself, but everything from kInputLoaded
		// onwards depends on an engine hook landing on the VR binary — which is exactly
		// what "the server never started" turned out to hinge on. Recording what actually
		// arrives makes the next such question a log read instead of an investigation.
		static std::set<std::uint32_t> seen;
		if (seen.insert(a_msg->type).second)
			logs::info("devbench: F4SE message {} ({})", a_msg->type, MessageName(a_msg->type));

		// kPostLoad, matching the Skyrim platform — NOT kGameDataReady as before.
		//
		// Two reasons, both measured against F4SEVR 0.6.20's PluginManager rather than
		// assumed:
		//
		// 1. kGameDataReady reaches us only if F4SEVR's GameDataReady hook landed on the
		//    VR binary. On this install it never arrived: the server never started, in a
		//    session that loaded a save. kPostLoad and kPostPostLoad are dispatched
		//    directly by the plugin manager after the load loop, with no hook involved,
		//    so they cannot fail that way.
		//
		// 2. It is the only point at which the any-sender listener can reach every
		//    plugin. F4SEVR's RegisterListener(sender = nullptr) is a SNAPSHOT: it walks
		//    the listener table AS IT EXISTS AT THAT MOMENT and appends itself to each
		//    slot. A plugin loaded later gets a fresh, empty slot, and Dispatch only ever
		//    walks the SENDER's own slot — so that plugin can never reach us, addressed
		//    or broadcast. Registering from F4SEPlugin_Load, as devbench did, covered
		//    only the handles that existed while devbench was loading.
		//
		// The tools query game state lazily, so an open port before the data handler is
		// ready is not a correctness problem — the `health` endpoint reports the frame
		// signal, which is the honest answer to "can the game answer yet".
		if (a_msg->type == F4SE::MessagingInterface::kPostLoad) {
			RegisterAnySenderListener("kPostLoad");
			StartServer();
		}

		// Retry the menu-event sink once the engine is actually up. RE::UI is null at
		// kPostLoad, so the attempt in StartServer() is best-effort; kGameDataReady and
		// kGameLoaded both arrive on this install (~12 s later, per the message log
		// above) and RE::UI is live by then. InstallGameEvents is idempotent, so calling
		// on both is free — whichever lands first wins, and if neither does, the
		// menuMap-backed answers are still correct; only the "menu" event stream is lost.
		if (g_server &&
			(a_msg->type == F4SE::MessagingInterface::kGameDataReady ||
				a_msg->type == F4SE::MessagingInterface::kGameLoaded))
			dvb::InstallGameEvents(g_server->Events());
	}
}

namespace dvb::platform
{
	void InstallHost();  // Fallout4Host.cpp
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "devbench";
	a_info->version = DEVBENCH_VERSION_MAJOR;

	if (a_f4se->IsEditor())
		return false;

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < (REL::Module::IsF4() ? F4SE::RUNTIME_LATEST : F4SE::RUNTIME_LATEST_VR))
		return false;

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	InitLogging();
	F4SE::Init(a_f4se, false);

	// Before anything else: the core's config paths, default port, and log sink all
	// route through the host seam, so it has to exist before LoadConfig or a dlog call.
	dvb::platform::InstallHost();

	const auto runtime = REL::Module::get().version();
	logs::info("devbench {} loading — {} v{}.{}.{}", DEVBENCH_VERSION_STRING,
		REL::Module::IsVR() ? "Fallout 4 VR" : "Fallout 4", runtime[0], runtime[1], runtime[2]);

	if (auto* messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(MessageHandler);
		// Second listener, any sender, for cross-plugin interface requests. Registering
		// here reaches only the plugins already loaded (F4SEVR's null-sender registration
		// is a snapshot — see MessageHandler), so it is a partial measure kept because it
		// costs nothing: it lets an EARLY-loading consumer that asks during its own
		// kPostLoad, before ours runs, still find us. The registration that reaches
		// everyone happens at kPostLoad.
		RegisterAnySenderListener("plugin load");
	}

	return true;
}
