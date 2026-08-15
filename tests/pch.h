#pragma once

// Test-only precompiled header. The Skyrim PCH (src/platform/skyrim/pch.h) pulls
// in RE/Skyrim.h + SKSE and aliases `namespace logs = SKSE::log`. The unit tests
// link NONE of CommonLibSSE-NG/SKSE — they compile only the game-agnostic core
// TUs (src/core/**) — so we supply a no-op `logs` stub with the same call surface
// for any platform code that leaks in.
//
// Core code does not need the stub: it logs through dvb::dlog, whose sink is
// simply never installed in a test binary, so every core log line is discarded by
// construction. That the tests build at all is the standing check that src/core
// really is free of the script extender.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_

#include <nlohmann/json.hpp>

#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace logs
{
	template <class... Args>
	void trace(Args&&...) noexcept
	{}
	template <class... Args>
	void debug(Args&&...) noexcept
	{}
	template <class... Args>
	void info(Args&&...) noexcept
	{}
	template <class... Args>
	void warn(Args&&...) noexcept
	{}
	template <class... Args>
	void error(Args&&...) noexcept
	{}
	template <class... Args>
	void critical(Args&&...) noexcept
	{}
}

using namespace std::literals;
