#include "core/Host.h"

namespace dvb::host
{
	namespace
	{
		// Written once at plugin load, read from the listener and main threads
		// thereafter. Not atomic on purpose: Install() runs before any thread that
		// reads it exists, and making it atomic would imply a mutability this value
		// does not have.
		Identity g_identity{ "unknown", "SKSE", {}, "0.0.0", false };
	}

	const Identity& Get()
	{
		return g_identity;
	}

	std::filesystem::path DataDir()
	{
		return std::filesystem::path{ "Data" } / g_identity.extender / "Plugins" / "devbench";
	}

	void Install(Identity a_identity)
	{
		g_identity = std::move(a_identity);
	}
}
