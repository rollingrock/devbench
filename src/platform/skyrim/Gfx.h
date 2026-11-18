#pragma once

#include "core/Json.h"
#include "core/ToolRegistry.h"  // ToolContext, ToolDescriptor

namespace dvb::Gfx
{
	/// The `gfx` tool: read, set or call anything in an open Scaleform menu.
	ToolDescriptor BuildDescriptor();
	json           Handle(const json& a_args, const ToolContext& a_ctx);
}
