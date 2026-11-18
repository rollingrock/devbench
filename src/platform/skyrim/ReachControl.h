#pragma once

#include "core/Json.h"
#include "core/ToolRegistry.h"

#include <string>
#include <vector>

// Reach, Skyrim's way to place the VR hands: it calibrates how a tracking-space move shows up
// at the hand bones, then corrects until each hand sits at its node. Fallout uses Pose
// (core/PoseControl.cpp) instead, since its Wands sit straight at the controllers.
namespace dvb::ReachControl
{
	/// The input actions Reach answers (reach, reachStatus, reachStop), for capabilities.
	std::vector<std::string> Actions();
	/// Whether a_action is one of Actions().
	bool IsAction(const std::string& a_action);
	/// Answers one Reach input action with the vrTrackedSet owner the arguments resolve to.
	json Handle(const std::string& a_action, const json& a_args, const ToolContext& a_ctx);
	/// Stops any active run and drops the cached calibrations; called on game load and new game.
	void EndSession();
}
