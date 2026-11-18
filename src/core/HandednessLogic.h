#pragma once

#include "core/Hand.h"
#include "core/Json.h"
#include "core/ReachLogic.h"

#include <optional>
#include <string>
#include <string_view>

// Handedness: which physical controller is the player's primary hand. VR input names a hand
// physically (left / right) or logically (primary / offhand); the logical names resolve
// against the game's handedness at request time. A handedness change is confirmed by the
// game's wand slots swapping controllers, which only shows while the controllers are held
// still and apart.
namespace dvb::HandednessLogic
{
	using ReachLogic::Vec3;

	/// World positions of the game's two wand slots, read from the slots themselves (not
	/// mapped to physical hands through the handedness).
	struct WandSlots
	{
		Vec3 primary;
		Vec3 secondary;
	};

	/// A wand slot within this many world units of where the other one was counts as swapped.
	inline constexpr double kSwapToleranceUnits = 2.0;
	/// Held wands closer than this (world units) cannot show a swap.
	inline constexpr double kMinApartUnits = 10.0;
	/// How long a set waits for the wands to swap; the game follows a few frames later.
	inline constexpr long kSwapTimeoutMs = 2000;

	std::optional<std::string> PhysicalHand(std::string_view a_name, bool a_leftHanded);
	bool                       UsesLogicalHands(const json& a_args);
	std::optional<json>        ResolveHands(const json& a_args, bool a_leftHanded, std::string& a_error);
	bool                       TooClose(const WandSlots& a_slots);
	bool                       Swapped(const WandSlots& a_before, const WandSlots& a_now);
}
