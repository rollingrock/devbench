#pragma once

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	/// Register the Fallout 4 game tools (`inspect`, `console`, `menu`, `nodes`). The
	/// game-agnostic tools (`ping`, `memory`, `log`) come from
	/// dvb::tools::RegisterCommonTools.
	void RegisterGameTools(ToolRegistry& a_registry, EventBus& a_events);

	/// Register `nodes` (scene-graph traversal). Called by RegisterGameTools.
	void RegisterNodeTools(ToolRegistry& a_registry);
}
