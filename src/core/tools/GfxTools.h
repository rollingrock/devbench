#pragma once

namespace dvb
{
	class ToolRegistry;
	class EventBus;
}

namespace dvb::tools
{
	/// Register the graphics tools: `rendertarget` (list / stats / dump the
	/// renderer's own buffers) and `measure` (frame-time percentiles, no hook).
	///
	/// Both are game-agnostic and sit on the graphics half of the platform seam
	/// (core/gfx/Device.h). A platform that does not implement that seam should
	/// simply not call this — the alternative, tools that always answer 503, is
	/// worse than tools that are not advertised.
	void RegisterGfxTools(ToolRegistry& a_registry, EventBus& a_events);
}
