#include "test_framework.h"

#include "core/HandednessLogic.h"

#include <string>

using dvb::json;
using namespace dvb::HandednessLogic;

TEST_CASE("PhysicalHand maps logical hands through the handedness")
{
	CHECK(*PhysicalHand("left", false) == "left");
	CHECK(*PhysicalHand("right", true) == "right");
	CHECK(*PhysicalHand("primary", false) == "right");
	CHECK(*PhysicalHand("offhand", false) == "left");
	CHECK(*PhysicalHand("primary", true) == "left");
	CHECK(*PhysicalHand("offhand", true) == "right");
	CHECK(!PhysicalHand("primaryHand", false));
	CHECK(!PhysicalHand("", true));
}

TEST_CASE("RoleOf inverts HandFor under either handedness")
{
	using dvb::Hand;
	using dvb::Role;
	for (const bool leftHanded : { false, true })
		for (const Role role : { Role::Primary, Role::Offhand })
			CHECK(dvb::RoleOf(dvb::HandFor(role, leftHanded), leftHanded) == role);
	CHECK(dvb::RoleOf(Hand::Left, true) == Role::Primary);
	CHECK(dvb::RoleOf(Hand::Left, false) == Role::Offhand);
}

TEST_CASE("UsesLogicalHands finds press hand, pose hands keys and head.lookAt")
{
	CHECK(!UsesLogicalHands(json::parse(R"({"action":"press","hand":"left"})")));
	CHECK(UsesLogicalHands(json::parse(R"({"action":"press","hand":"offhand"})")));
	CHECK(UsesLogicalHands(json::parse(R"({"hands":{"primary":"ready"}})")));
	CHECK(!UsesLogicalHands(json::parse(R"({"hands":"ready","head":"ready"})")));
	CHECK(UsesLogicalHands(json::parse(R"({"head":{"lookAt":"primary"}})")));
	CHECK(!UsesLogicalHands(json::parse(R"({"head":{"lookAt":"hands"}})")));
	CHECK(!UsesLogicalHands(json::parse(R"({"head":{"lookAt":[1,2,3]}})")));
	CHECK(!UsesLogicalHands(json::parse("[]")));
}

TEST_CASE("ResolveHands rewrites logical names to the physical controllers")
{
	std::string error;
	auto        out = ResolveHands(json::parse(R"({"action":"press","hand":"offhand","buttons":["a"]})"), true, error);
	CHECK(out.has_value());
	if (!out)
		return;
	CHECK((*out)["hand"] == "right");
	CHECK((*out)["buttons"] == json::array({ "a" }));

	out = ResolveHands(json::parse(R"({"hands":{"primary":{"world":[1,2,3]},"left":"ready"},"head":{"lookAt":"offhand","root":"player"}})"), false, error);
	CHECK(out.has_value());
	if (!out)
		return;
	CHECK((*out)["hands"]["right"]["world"] == json::array({ 1, 2, 3 }));
	CHECK((*out)["hands"]["left"] == "ready");
	CHECK((*out)["hands"].size() == 2);
	CHECK((*out)["head"]["lookAt"] == "left");
	CHECK((*out)["head"]["root"] == "player");

	// unknown names pass through for pose's own validation
	out = ResolveHands(json::parse(R"({"hands":{"middle":"ready"}})"), false, error);
	CHECK(out.has_value());
	if (!out)
		return;
	CHECK((*out)["hands"].contains("middle"));
}

TEST_CASE("ResolveHands refuses two names for one controller")
{
	std::string error;
	CHECK(!ResolveHands(json::parse(R"({"hands":{"left":"ready","primary":"ready"}})"), true, error));
	CHECK(error.find("left controller twice") != std::string::npos);
	CHECK(ResolveHands(json::parse(R"({"hands":{"left":"ready","primary":"ready"}})"), false, error));
}

TEST_CASE("Swapped and TooClose read the wand slots")
{
	const WandSlots before{ { 0, 0, 0 }, { 26, 0, 0 } };
	CHECK(!TooClose(before));
	CHECK(TooClose(WandSlots{ { 0, 0, 0 }, { 9.9, 0, 0 } }));
	CHECK(Swapped(before, WandSlots{ { 25, 1, 0 }, { 0.5, 0, 1 } }));
	CHECK(!Swapped(before, before));
	CHECK(!Swapped(before, WandSlots{ { 26, 0, 0 }, { 3, 0, 0 } }));
}
