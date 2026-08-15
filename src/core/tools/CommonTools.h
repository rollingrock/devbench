#pragma once

#include "core/ToolRegistry.h"

namespace dvb
{
	class EventBus;
}

namespace dvb::tools
{
	/// Register the tools that need nothing from the game beyond "it is a Windows
	/// process with an executable image and a log file": `ping`, `memory`, `log`.
	///
	/// Every platform calls this; game-specific tools are registered separately by
	/// src/platform/<game>. Anything that ends up here must compile with no script
	/// extender in the include path — that constraint is the whole point of the split,
	/// and tests/ builds this file to keep it honest.
	void RegisterCommonTools(ToolRegistry& a_registry, EventBus& a_events);

	/// Gate for `memory action='write'`. Off unless config.json sets allowMemoryWrites.
	/// A write ALSO needs confirm=true on the request itself — two gates because a
	/// mistyped read that pokes the engine is a mistake someone will make exactly once
	/// and then spend an hour misdiagnosing.
	void SetAllowMemoryWrites(bool a_allow);
}
