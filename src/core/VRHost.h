#pragma once

#include "core/HandednessLogic.h"
#include "core/PoseLogic.h"
#include "core/ReachLogic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vr
{
	class IVRSystem;
	class IVRCompositor;
}

// The VR input seam.
//
// Everything the core's VR input (core/VRInput.cpp) and Pose (core/PoseControl.cpp) need from
// the game; Skyrim's Reach (platform/skyrim/ReachControl.cpp) reads the hand and node through
// it too. Each platform links exactly one definition of these functions; a platform without VR
// input returns nullptr from the interface getters, which keeps the input API reporting "not
// ready" instead of failing to link.
//
// Threading: the interface getters, PlatformActions and LeftHanded are callable from any thread. Everything else is
// called on the game's main thread (the core marshals with MainThread::RunAndWait first).
namespace dvb::vrhost
{
	/// The game's own OpenVR system interface (IVRSystem_019 ABI), or nullptr while unavailable.
	vr::IVRSystem* System();
	/// The game's own OpenVR compositor interface (IVRCompositor_022 ABI), or nullptr while unavailable.
	vr::IVRCompositor* Compositor();

	/// Points the game's left/right controller devices at the given OpenVR device indices and
	/// returns the indices they had, so a Sequence can restore them. Throws ToolError(503) while
	/// the game's controller devices are unavailable.
	std::pair<std::uint32_t, std::uint32_t> SetControllerIndices(std::uint32_t a_left, std::uint32_t a_right);

	/// The vrTrackedSet actions this platform answers beside the shared ones, for capabilities:
	/// Fallout's pose, Skyrim's reach actions. Callable from any thread.
	std::vector<std::string> PlatformActions();

	/// World position of the in-game node Pose and Reach measure for one Controller.
	ReachLogic::Vec3 ReachHandWorld(Hand a_hand);
	/// World position of a Reach target node, looked up by name in a_root ("player" = body,
	/// "firstPerson", empty = the platform's default order). Throws ToolError(404) when absent.
	ReachLogic::Vec3 ReachNodeWorld(const std::string& a_name, const std::string& a_root = {});
	/// The player's heading in radians (game yaw, 0 = +Y, increasing clockwise seen from above).
	double PlayerYaw();

	/// What Pose needs to stand the injected body naturally and map tracking space to the world.
	struct PoseStance
	{
		double unitsPerMeter;  ///< world units per tracking meter
		double roomLiftUnits;  ///< how far the room node sits above the player's feet (a body mod's seated / standing lift)
		double cameraUnits;    ///< where the camera should sit above the feet by default
	};
	/// Throws ToolError(501) on a platform Pose has not been measured on.
	PoseStance ReadPoseStance();
	/// World position of the in-game HMD node, the anchor Pose maps tracking space through.
	ReachLogic::Vec3 HmdWorld();
	/// The in-game HMD node's world axes (its rotation rows: right, forward, up), the frame a
	/// Pose head offset is given in. Throws ToolError(501) where Pose is not available.
	PoseLogic::HeadAxes HmdAxes();

	/// The game's handedness: true while the left controller is the primary hand; nullopt on a
	/// platform that does not report it (the handedness action and logical hand names 501 there).
	std::optional<bool> LeftHanded();
	/// Sets the game's handedness. Throws ToolError(501) where LeftHanded is nullopt.
	void SetLeftHanded(bool a_leftHanded);
	/// The two wand slots' world positions, read from the slots (not mapped through the
	/// handedness), so a handedness change shows as a swap. Throws ToolError(501) where absent.
	HandednessLogic::WandSlots WandSlotsWorld();
}
