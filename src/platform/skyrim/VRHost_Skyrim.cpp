#include "core/VRHost.h"

#include "ReachControl.h"

#include "core/ToolRegistry.h"

#include <RE/B/BSFixedString.h>
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSOpenVR.h>
#include <RE/N/NiAVObject.h>
#include <RE/P/PlayerCharacter.h>

#include <format>

// Skyrim VR's side of the VR input seam (core/VRHost.h).
namespace dvb::vrhost
{
	namespace
	{
		constexpr const char* kRightHandNode = "NPC R Hand [RHnd]";
		constexpr const char* kLeftHandNode = "NPC L Hand [LHnd]";

		RE::BSTrackedControllerDevice* ControllerDevice(Hand a_hand)
		{
			auto* manager = RE::BSInputDeviceManager::GetSingleton();
			if (!manager)
				return nullptr;
			return a_hand == Hand::Right ? manager->GetVRControllerRight() : manager->GetVRControllerLeft();
		}
	}

	vr::IVRSystem* System()
	{
		if (!REL::Module::IsVR())
			return nullptr;
		auto* openvr = RE::BSOpenVR::GetSingleton();
		return openvr ? openvr->vrSystem : nullptr;
	}

	/**
	 * The game's compositor, falling back to the one in BSOpenVR's context when the
	 * global accessor is still null.
	 */
	vr::IVRCompositor* Compositor()
	{
		if (!REL::Module::IsVR())
			return nullptr;
		if (auto* compositor = RE::BSOpenVR::GetIVRCompositor())
			return compositor;
		auto* openvr = RE::BSOpenVR::GetSingleton();
		return openvr ? openvr->vrContext.vrCompositor : nullptr;
	}

	/**
	 * Rewrites the tracked-device index on BSInputDeviceManager's VR controller devices, which
	 * is how Skyrim decides which OpenVR device feeds each hand.
	 */
	std::pair<std::uint32_t, std::uint32_t> SetControllerIndices(std::uint32_t a_left, std::uint32_t a_right)
	{
		auto* left = ControllerDevice(Hand::Left);
		auto* right = ControllerDevice(Hand::Right);
		if (!left || !right)
			throw ToolError(503, "Skyrim VR controller devices are unavailable");
		const std::pair previous{ left->GetRuntimeData().trackedDeviceIndex, right->GetRuntimeData().trackedDeviceIndex };
		left->GetRuntimeData().trackedDeviceIndex = a_left;
		right->GetRuntimeData().trackedDeviceIndex = a_right;
		return previous;
	}

	std::vector<std::string> PlatformActions()
	{
		return ReachControl::Actions();
	}

	/**
	 * A node of the player's body (third-person) 3D by name, or of the first-person 3D when
	 * a_root is "firstPerson".
	 */
	ReachLogic::Vec3 ReachNodeWorld(const std::string& a_name, const std::string& a_root)
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		auto* root = pc ? pc->Get3D(a_root == "firstPerson") : nullptr;
		auto* node = root ? root->GetObjectByName(RE::BSFixedString(a_name.c_str())) : nullptr;
		if (!node)
			throw ToolError(404, std::format("node '{}' not found on the player", a_name));
		const auto& t = node->world.translate;
		return ReachLogic::Vec3{ t.x, t.y, t.z };
	}

	ReachLogic::Vec3 ReachHandWorld(Hand a_hand)
	{
		return ReachNodeWorld(a_hand == Hand::Right ? kRightHandNode : kLeftHandNode);
	}

	double PlayerYaw()
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc)
			throw ToolError(503, "player is not available");
		return pc->GetAngleZ();
	}

	/**
	 * Not measured on Skyrim VR: its hand reads are body nodes, not controller-placed Wands, so
	 * the HMD-anchored map Pose relies on is unverified there.
	 */
	PoseStance ReadPoseStance()
	{
		throw ToolError(501, "pose is not available on Skyrim VR yet; use reach");
	}

	ReachLogic::Vec3 HmdWorld()
	{
		throw ToolError(501, "pose is not available on Skyrim VR yet; use reach");
	}

	PoseLogic::HeadAxes HmdAxes()
	{
		throw ToolError(501, "pose is not available on Skyrim VR yet; use reach");
	}

	/**
	 * Not reported on Skyrim VR yet: the handedness action and the primary / offhand hand
	 * names answer 501 there.
	 */
	std::optional<bool> LeftHanded()
	{
		return std::nullopt;
	}

	void SetLeftHanded(bool)
	{
		throw ToolError(501, "handedness is not available on Skyrim VR yet");
	}

	HandednessLogic::WandSlots WandSlotsWorld()
	{
		throw ToolError(501, "handedness is not available on Skyrim VR yet");
	}
}
