#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// A hand side. Hand is a physical controller; Role is what the game's handedness makes of one:
// the primary hand, or the offhand, which drives the game's secondary wand slot.
namespace dvb
{
	/// A physical controller.
	enum class Hand : std::uint8_t
	{
		Left,
		Right,
	};

	/// A hand's role under the game's handedness.
	enum class Role : std::uint8_t
	{
		Primary,
		Offhand,
	};

	/// The hand's name on the wire: "left" or "right".
	constexpr std::string_view HandName(Hand a_hand)
	{
		return a_hand == Hand::Right ? "right" : "left";
	}

	/// The hand's slot in a per-hand array: left 0, right 1.
	constexpr std::size_t HandIndex(Hand a_hand)
	{
		return a_hand == Hand::Right ? 1 : 0;
	}

	/**
	 * The physical controller that plays a_role: the primary hand is the right controller unless
	 * left-handed.
	 */
	constexpr Hand HandFor(Role a_role, bool a_leftHanded)
	{
		return (a_role == Role::Primary) != a_leftHanded ? Hand::Right : Hand::Left;
	}

	/**
	 * The role a physical controller plays under the handedness; the inverse of HandFor.
	 */
	constexpr Role RoleOf(Hand a_hand, bool a_leftHanded)
	{
		return (a_hand == Hand::Right) != a_leftHanded ? Role::Primary : Role::Offhand;
	}

	/**
	 * a_x for the right hand, -a_x for the left: the left hand's side of a symmetric pose is the
	 * right one mirrored across the body's center line.
	 */
	constexpr double Mirrored(Hand a_hand, double a_x)
	{
		return a_hand == Hand::Right ? a_x : -a_x;
	}
}
