#pragma once

#include <cstddef>
#include <functional>
#include <vector>

// Controls for the platform test doubles in PlatformStubs.cpp.
namespace dvb::test
{
	/// What MainThread::ConfinedToMainThread() answers.
	extern bool g_confined;

	/// Functions MainThread::Post() has queued and not yet run.
	extern std::vector<std::function<void()>> g_posted;

	/// Run every queued function, as the next frame would; returns how many ran.
	std::size_t DrainPosted();
}
