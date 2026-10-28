// Test doubles for the platform seams that a *linked-in* core TU calls.
//
// The suite links no script extender and no game, so any core file it compiles must
// still resolve its seam symbols. Host.cpp and Log.cpp are self-contained (Log's sink
// is simply never installed); GameState is not — it is declared in the core and
// implemented per platform, so it needs a double here.
//
// This exists so src/core/HostApi.cpp can be compiled by this target as a standing
// check that the cross-plugin C-ABI provider stays free of SKSE/F4SE. See the comment
// on that add_files() line in xmake.lua.

#include "core/GameState.h"
#include "core/MainThread.h"
#include "test_platform.h"

#include <utility>

// MainThread's queue-without-waiting half, which HostApi's main-thread service calls.
// Posted functions are held until a test drains them, the way a frame would; the
// confinement answer is whatever the test sets.
namespace dvb::test
{
	bool                               g_confined = false;
	std::vector<std::function<void()>> g_posted;

	std::size_t DrainPosted()
	{
		auto batch = std::exchange(g_posted, {});
		for (auto& fn : batch)
			fn();
		return batch.size();
	}
}

namespace dvb::MainThread
{
	bool Post(std::function<void()> a_fn)
	{
		test::g_posted.push_back(std::move(a_fn));
		return true;
	}

	bool ConfinedToMainThread() { return test::g_confined; }
}

namespace dvb::game
{
	// -1 is the core's own documented "could not resolve" value, and it is the honest
	// answer here: there is no game, so there is no frame counter. Returning 0 would
	// claim a real frame number and could make a future test assert against a fiction.
	int CurrentFrame()
	{
		return -1;
	}
}
