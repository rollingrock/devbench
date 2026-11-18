#pragma once

#include "core/Json.h"
#include "core/ToolRegistry.h"

#include <string>

namespace dvb::PoseControl
{
	/// Places the HMD and controllers as the `pose` arguments ask and holds them; answers once
	/// the hands are placed. Replaces this owner's held pose in place while its sequence runs.
	json Place(const json& a_args, const std::string& a_owner, const ToolContext& a_ctx);

	/// Reads the game's handedness, or sets it and confirms the wands swapped (`handedness`).
	json Handedness(const json& a_args, const std::string& a_owner, const ToolContext& a_ctx);
}
