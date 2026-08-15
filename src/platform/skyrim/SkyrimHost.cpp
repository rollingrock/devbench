// The Skyrim side of the core's platform seam (src/core/Host.h, src/core/Log.h).
// The Fallout twin is src/platform/fallout4/Fallout4Host.cpp — keeping the two files
// the same shape is deliberate: adding a game should be a matter of writing one of
// these plus a tools file, and a diff between them shows exactly what a game owes
// the core.

#include "core/Host.h"
#include "core/Log.h"

#include "Version.h"

#include <filesystem>

namespace
{
	void SpdlogSink(dvb::dlog::Level a_level, std::string_view a_message)
	{
		switch (a_level) {
		case dvb::dlog::Level::trace:
			logs::trace("{}", a_message);
			break;
		case dvb::dlog::Level::debug:
			logs::debug("{}", a_message);
			break;
		case dvb::dlog::Level::info:
			logs::info("{}", a_message);
			break;
		case dvb::dlog::Level::warn:
			logs::warn("{}", a_message);
			break;
		case dvb::dlog::Level::error:
			logs::error("{}", a_message);
			break;
		case dvb::dlog::Level::critical:
			logs::critical("{}", a_message);
			break;
		}
	}

	std::string ExeBasename()
	{
		char        path[MAX_PATH]{};
		const DWORD len = ::GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (len == 0 || len == MAX_PATH)
			return {};
		return std::filesystem::path(path).filename().string();
	}
}

namespace dvb::platform
{
	void InstallHost()
	{
		dlog::SetSink(&SpdlogSink);
		host::Install(host::Identity{
			.game = "skyrim",
			.extender = "SKSE",
			.exe = ExeBasename(),
			.version = DEVBENCH_VERSION_STRING,
			.vr = REL::Module::IsVR(),
		});
	}
}

namespace dvb::host
{
	std::filesystem::path LogDir()
	{
		const auto dir = SKSE::log::log_directory();
		return dir ? *dir : std::filesystem::path{};
	}
}
