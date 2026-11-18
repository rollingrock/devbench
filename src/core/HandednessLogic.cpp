#include "core/HandednessLogic.h"

#include <format>

namespace dvb::HandednessLogic
{
	namespace
	{
		bool IsLogical(std::string_view a_name)
		{
			return a_name == "primary" || a_name == "offhand";
		}

		/**
		 * A copy of VR input arguments with each hand name passed through a_map, at every place a
		 * hand is named: press's 'hand', the keys of pose's 'hands' object, and head.lookAt (string
		 * values only). The one walk over those places; fails (nullopt, a_duplicate set to the
		 * name) when the mapped 'hands' keys name one controller twice.
		 */
		template <class Map>
		std::optional<json> MapHandNames(const json& a_args, Map&& a_map, std::string& a_duplicate)
		{
			json out = a_args;
			if (!out.is_object())
				return out;
			if (out.contains("hand") && out["hand"].is_string())
				out["hand"] = a_map(out["hand"].get<std::string>());
			if (out.contains("hands") && out["hands"].is_object()) {
				json hands = json::object();
				for (const auto& [key, value] : out["hands"].items()) {
					const std::string mapped = a_map(key);
					if (hands.contains(mapped)) {
						a_duplicate = mapped;
						return std::nullopt;
					}
					hands[mapped] = value;
				}
				out["hands"] = std::move(hands);
			}
			if (out.contains("head") && out["head"].is_object() && out["head"].contains("lookAt") && out["head"]["lookAt"].is_string())
				out["head"]["lookAt"] = a_map(out["head"]["lookAt"].get<std::string>());
			return out;
		}
	}

	/**
	 * The physical controller ("left" / "right") a hand name means: a physical name as is, a
	 * logical one for the handedness (the primary hand is the right controller unless
	 * left-handed). nullopt for any other name.
	 */
	std::optional<std::string> PhysicalHand(std::string_view a_name, bool a_leftHanded)
	{
		if (a_name == "left" || a_name == "right")
			return std::string(a_name);
		if (a_name == "primary")
			return std::string(HandName(HandFor(Role::Primary, a_leftHanded)));
		if (a_name == "offhand")
			return std::string(HandName(HandFor(Role::Offhand, a_leftHanded)));
		return std::nullopt;
	}

	/**
	 * Whether VR input arguments name a logical hand where a hand is named: press's 'hand',
	 * a key of pose's 'hands' object, or head.lookAt.
	 */
	bool UsesLogicalHands(const json& a_args)
	{
		bool        found = false;
		std::string duplicate;
		MapHandNames(
			a_args, [&](const std::string& a_name) {
				found = found || IsLogical(a_name);
				return a_name;
			},
			duplicate);
		return found;
	}

	/**
	 * A copy of VR input arguments with every logical hand name replaced by its physical
	 * controller for the handedness. Fails (nullopt, a_error filled) when pose's 'hands' names
	 * one controller twice, e.g. left and primary while left-handed. Other values pass through
	 * for the action's own validation.
	 */
	std::optional<json> ResolveHands(const json& a_args, bool a_leftHanded, std::string& a_error)
	{
		std::string duplicate;
		auto        out = MapHandNames(
			a_args, [&](const std::string& a_name) {
				return IsLogical(a_name) ? *PhysicalHand(a_name, a_leftHanded) : a_name;
			},
			duplicate);
		if (!out)
			a_error = std::format("'hands' names the {} controller twice ({}handed)", duplicate, a_leftHanded ? "left-" : "right-");
		return out;
	}

	/**
	 * Whether the wand slots sit too close together for a swap to show.
	 */
	bool TooClose(const WandSlots& a_slots)
	{
		return ReachLogic::Distance(a_slots.primary, a_slots.secondary) < kMinApartUnits;
	}

	/**
	 * Whether each wand slot now sits where the other one was, within kSwapToleranceUnits.
	 */
	bool Swapped(const WandSlots& a_before, const WandSlots& a_now)
	{
		return ReachLogic::Distance(a_now.primary, a_before.secondary) <= kSwapToleranceUnits &&
		       ReachLogic::Distance(a_now.secondary, a_before.primary) <= kSwapToleranceUnits;
	}
}
