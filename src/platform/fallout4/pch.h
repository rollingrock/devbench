#pragma once

#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used stuff from Windows headers
#define NOMINMAX

// cpp-mcp's vendored cpp-httplib pulls in <winsock2.h>. CommonLibF4's transitive
// <Windows.h> would otherwise include the legacy <winsock.h>, which conflicts
// (sockaddr / WSAData redefinitions). Defining this first makes winsock2 the only
// one in the build. Same reason as the Skyrim PCH.
#define _WINSOCKAPI_

#include <RE/Fallout.h>
#include <REL/Relocation.h>
#include <REX/REX.h>
#include <F4SE/F4SE.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// CommonLibF4 names its spdlog alias `logger`, CommonLibSSE-NG names it `logs`.
// Alias both here so platform code reads the same either side of the fence and a
// file moved between platforms does not need its log calls rewritten. Core code
// uses neither — it uses dvb::dlog (src/core/Log.h).
namespace logs = F4SE::log;

using namespace std::literals;

// CommonLibF4 does not define this (CommonLibSSE-NG hides it behind SKSEPluginLoad).
// The F4SE entry points are exported by hand, so the macro has to come from us.
#define DLLEXPORT __declspec(dllexport)
