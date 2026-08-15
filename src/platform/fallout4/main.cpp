#include "Tools_Fallout4.h"

#include "core/Config.h"
#include "core/GameState.h"
#include "core/Server.h"
#include "core/tools/CommonTools.h"
#include "core/tools/GfxTools.h"

#include "Version.h"

#include <memory>
#include <spdlog/sinks/basic_file_sink.h>

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
		g_server->Start();
	}

	void MessageHandler(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg)
			return;
		// kGameDataReady, not kPostLoad: F4SE's kPostLoad fires before the data handler
		// exists, and the tools query forms lazily anyway. Starting the listener here
		// keeps "the port is open" honest about "the game can answer".
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady)
			StartServer();
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

	if (auto* messaging = F4SE::GetMessagingInterface())
		messaging->RegisterListener(MessageHandler);

	return true;
}
