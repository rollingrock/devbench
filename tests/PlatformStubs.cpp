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
