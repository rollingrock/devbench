#include "core/VRHost.h"

#include "Tools_Fallout4.h"
#include "core/ToolRegistry.h"

#include "openvr.h"

#include <atomic>

// Fallout 4 VR's side of the VR input seam (core/VRHost.h).
namespace dvb::vrhost
{
	namespace
	{
		using GetGenericInterface_t = void* (*)(const char*, vr::EVRInitError*);

		/**
		 * Asks the loaded openvr_api.dll for an interface by version string. GetProcAddress
		 * returns the real export, so this gets the runtime's own object even when another mod
		 * (FO4VRTools) patched the game's import to hand the game a proxy; the proxy forwards to
		 * this object, so hooks on it still see the game's calls. Null until the game has
		 * initialized OpenVR.
		 */
		void* LookUp(const char* a_version)
		{
			const HMODULE module = GetModuleHandleW(L"openvr_api.dll");
			if (!module)
				return nullptr;
			const auto getInterface = reinterpret_cast<GetGenericInterface_t>(GetProcAddress(module, "VR_GetGenericInterface"));
			if (!getInterface)
				return nullptr;
			vr::EVRInitError error = vr::VRInitError_None;
			void*            object = getInterface(a_version, &error);
			return error == vr::VRInitError_None ? object : nullptr;
		}

		/**
		 * Caches an interface once found; the runtime returns the same object for the life of
		 * the process.
		 */
		template <class T>
		T* Cached(std::atomic<T*>& a_cache, const char* a_version)
		{
			if (auto* cached = a_cache.load(std::memory_order_acquire))
				return cached;
			auto* found = static_cast<T*>(LookUp(a_version));
			if (found)
				a_cache.store(found, std::memory_order_release);
			return found;
		}

		std::atomic<vr::IVRSystem*>     g_system{ nullptr };
		std::atomic<vr::IVRCompositor*> g_compositor{ nullptr };
	}

	vr::IVRSystem* System()
	{
		return Cached(g_system, vr::IVRSystem_Version);
	}

	vr::IVRCompositor* Compositor()
	{
		return Cached(g_compositor, vr::IVRCompositor_Version);
	}

	/**
	 * No-op: no Fallout 4 VR structure holding a per-hand OpenVR device index is known, so
	 * the core's role hooks (GetTrackedDeviceIndexForControllerRole and its inverse) are what
	 * map devices to hands during a Sequence. Whether the game also caches the indices
	 * somewhere is unverified. Returns the requested indices unchanged.
	 */
	std::pair<std::uint32_t, std::uint32_t> SetControllerIndices(std::uint32_t a_left, std::uint32_t a_right)
	{
		return { a_left, a_right };
	}

	std::vector<std::string> PlatformActions()
	{
		return { "pose", "handedness" };
	}

	/**
	 * bLeftHandedMode:VR, read live from the setting object; nullopt without it (flat Fallout 4).
	 */
	std::optional<bool> LeftHanded()
	{
		auto* setting = LeftHandedSetting();
		return setting ? std::optional<bool>(setting->GetBinary()) : std::nullopt;
	}

	/**
	 * Sets bLeftHandedMode:VR through the console's setini, which the game follows within a few
	 * frames. Writing the setting object alone changes the value but the wands never swap (both
	 * verified live on FO4VR): setini does more than the write.
	 */
	void SetLeftHanded(bool a_leftHanded)
	{
		if (!LeftHandedSetting())
			throw ToolError(501, "bLeftHandedMode:VR is not on this runtime");
		const json result = RunConsoleCapture(std::format("setini \"bLeftHandedMode:VR\" {}", a_leftHanded ? 1 : 0), 20);
		if (!result.value("compiled", false))
			throw ToolError(500, std::format("setini bLeftHandedMode:VR did not compile: {}", result.value("lines", json::array()).dump()));
	}

	HandednessLogic::WandSlots WandSlotsWorld()
	{
		const auto& primary = WandSlotNode(Role::Primary)->world.translate;
		const auto& secondary = WandSlotNode(Role::Offhand)->world.translate;
		return HandednessLogic::WandSlots{ { primary.x, primary.y, primary.z }, { secondary.x, secondary.y, secondary.z } };
	}

	/**
	 * Pose measures the Wand, which the game places straight from the controller pose with
	 * no body IK, so it exists with or without FRIK.
	 */
	ReachLogic::Vec3 ReachHandWorld(Hand a_hand)
	{
		const auto& t = WandNode(a_hand)->world.translate;
		return ReachLogic::Vec3{ t.x, t.y, t.z };
	}

	ReachLogic::Vec3 ReachNodeWorld(const std::string& a_name, const std::string& a_root)
	{
		const auto& t = FindPlayerNode(a_name, a_root)->world.translate;
		return ReachLogic::Vec3{ t.x, t.y, t.z };
	}

	double PlayerYaw()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			throw ToolError(503, "player is not available");
		return player->data.angle.z;
	}

	/**
	 * 70 world units per tracking meter, measured live (a controller 0.36 m from the head lands
	 * 25.6 units from it). The camera target, 115 units above the feet, is where it sits in a
	 * natural seated FRIK body: FRIK's 37.4-unit seated lift on the room node plus 1.11 m of head
	 * height. The room node carries that lift, so reading it keeps the body standing the same
	 * with FRIK seated, standing, or absent.
	 */
	PoseStance ReadPoseStance()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			throw ToolError(503, "player is not available");
		const auto* room = FindPlayerNode("roomNode");
		return PoseStance{ 70.0, room->world.translate.z - player->data.location.z, 115.0 };
	}

	ReachLogic::Vec3 HmdWorld()
	{
		const auto& t = FindPlayerNode("hmdNode")->world.translate;
		return ReachLogic::Vec3{ t.x, t.y, t.z };
	}

	/**
	 * The HMD node's world rotation rows as right, forward, up: each row is one of the node's
	 * axes in the world, the frame the nodes tool's rot rows show.
	 */
	PoseLogic::HeadAxes HmdAxes()
	{
		const auto& m = FindPlayerNode("hmdNode")->world.rotate;
		const auto  row = [&](int a_r) { return ReachLogic::Vec3{ m.entry[a_r][0], m.entry[a_r][1], m.entry[a_r][2] }; };
		return PoseLogic::HeadAxes{ row(0), row(1), row(2) };
	}
}
