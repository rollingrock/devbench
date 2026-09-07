// The Fallout 4 / Fallout 4 VR side of the core's platform seam (src/core/Host.h,
// src/core/Log.h, src/core/GameState.h). One translation unit, three small
// definitions — if this file grows, whatever was added probably belongs in a tool.

#include "core/GameState.h"
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
	// Called first thing in F4SEPlugin_Load, before LoadConfig (which needs DataDir
	// and DefaultPort) and before anything logs through the core.
	void InstallHost()
	{
		dlog::SetSink(&SpdlogSink);
		host::Install(host::Identity{
			.game = "fallout4",
			.extender = "F4SE",
			.exe = ExeBasename(),
			.version = DEVBENCH_VERSION_STRING,
			.vr = REL::Module::IsVR(),
		});
	}
}

namespace dvb::game
{
	// BSGraphics::State::frameCount — the engine's own per-frame counter, read
	// hook-free, mirroring what the Skyrim platform does with its equivalent global.
	// devbench takes no per-frame hook by design; this number is what lets
	// `GET /api/health` tell "main thread busy" from "main thread hung" without one.
	//
	// Resolution differs per runtime, and only the flat-rim path is address-library
	// backed:
	//   * Fallout 4      REL::ID(600795) — CommonLibF4's BSGraphics::State singleton.
	//   * Fallout 4 VR   the VR address library does not cover that id, so the state
	//                    block is taken from its measured image offset. That offset
	//                    was established in the FO4VR scope investigation and is
	//                    plausibility-gated below rather than trusted outright.
	//
	// Returns -1 when the address cannot be resolved or the value fails the gate. The
	// core treats -1 as "no frame signal" and degrades gracefully (health reports the
	// pending/last-completed markers without the hung-vs-busy discrimination).
	int CurrentFrame()
	{
		// Resolve once; the address is stable for the process lifetime.
		static std::uint32_t* counter = []() -> std::uint32_t* {
			constexpr std::uintptr_t kStateOffsetVR = 0x65A2AB0;  // measured, see above
			constexpr std::uintptr_t kFrameCountField = 0x98;     // BSGraphics::State::frameCount

			std::uintptr_t state = 0;
			if (REL::Module::IsVR()) {
				state = REL::Module::get().base() + kStateOffsetVR;
			} else {
				try {
					state = REL::ID(600795).address();
				} catch (...) {
					return nullptr;
				}
			}
			if (!state)
				return nullptr;
			return reinterpret_cast<std::uint32_t*>(state + kFrameCountField);
		}();

		if (!counter)
			return -1;

		// The main loop writes this while we read it off the listener thread;
		// atomic_ref makes that read well-defined (a raw load is a formal data race
		// the compiler is free to hoist).
		const std::uint32_t raw = std::atomic_ref<std::uint32_t>(*counter).load(std::memory_order_relaxed);

		// Plausibility gate. A frame counter is a small monotonically-rising integer;
		// anything above ~2 years of continuous play at 144 Hz means the offset is
		// wrong on this build, and reporting a garbage number as a frame is worse than
		// reporting none (the stall watchdog would act on it).
		constexpr std::uint32_t kImplausible = 0x40000000;
		if (raw >= kImplausible)
			return -1;
		return static_cast<int>(raw);
	}
}

namespace dvb::host
{
	std::filesystem::path LogDir()
	{
		auto dir = F4SE::log::log_directory();
		if (!dir)
			return {};
		// F4SEVR's resolver can land on the flat-rim folder; correct it the same way
		// the plugin's own log setup does, so the `log` tool and the log file agree.
		const auto gamePath = REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
		if (!dir->generic_string().ends_with(gamePath))
			return dir->parent_path().append(gamePath);
		return *dir;
	}
}
