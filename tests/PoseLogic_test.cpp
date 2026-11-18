#include "test_framework.h"

#include "core/PoseLogic.h"

#include <cmath>
#include <optional>
#include <string>

using dvb::Hand;
using dvb::json;
using namespace dvb::PoseLogic;

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	bool Near(double a_a, double a_b, double a_eps = 1e-9)
	{
		return std::abs(a_a - a_b) <= a_eps;
	}

	bool NearVec(const Vec3& a_a, const Vec3& a_b, double a_eps = 1e-9)
	{
		return Near(a_a.x, a_b.x, a_eps) && Near(a_a.y, a_b.y, a_eps) && Near(a_a.z, a_b.z, a_eps);
	}

	std::optional<PoseRequest> Parse(const std::string& a_json, std::string& a_error)
	{
		return ParsePose(json::parse(a_json), a_error);
	}

	Vec3 Translation(const json& a_frame, const char* a_device)
	{
		const json& m = a_frame[a_device]["matrix"];
		return Vec3{ m[3].get<double>(), m[7].get<double>(), m[11].get<double>() };
	}
}

TEST_CASE("ParsePose takes ready, node and world hands, head pitch and limits")
{
	std::string error;
	const auto  request = Parse(R"({"hands":{"left":"ready","right":{"node":"Chest","root":"player","front":5,"offset":[1,2,3]}},"head":{"pitch":-60},"moveMs":200,"corrections":1})", error);
	CHECK(request.has_value());
	CHECK(request->left->kind == HandPlacement::Kind::kReady);
	CHECK(request->right->kind == HandPlacement::Kind::kNode);
	CHECK(request->right->node == "Chest");
	CHECK(request->right->root == "player");
	CHECK(Near(request->right->front, 5.0));
	CHECK(NearVec(request->right->offset, Vec3{ 1, 2, 3 }));
	CHECK(Near(*request->headPitch, -60.0));
	CHECK(request->moveMs == 200);
	CHECK(request->corrections == 1);

	const auto world = Parse(R"({"hands":{"left":{"world":[-1700.5,-1300,90]}}})", error);
	CHECK(world.has_value());
	CHECK(world->left->kind == HandPlacement::Kind::kWorld);
	CHECK(!world->right.has_value());
	CHECK(!world->headPitch.has_value());
	CHECK(!world->eyeHeight.has_value());

	const auto heights = Parse(R"({"head":{"eyeHeight":1.2,"cameraHeight":120}})", error);
	CHECK(heights.has_value());
	CHECK(Near(*heights->eyeHeight, 1.2));
	CHECK(Near(*heights->cameraHeight, 120.0));
	CHECK(!heights->headPitch.has_value());

	CHECK(Parse(R"({})", error).has_value());
}

TEST_CASE("ParsePose takes a placed hand's controller pitch and yaw, the ready aim filling the other")
{
	std::string error;
	const auto  aimed = Parse(R"({"hands":{"left":{"world":[0,0,0],"pitch":123,"yaw":14},"right":{"node":"Chest","pitch":40}}})", error);
	CHECK(aimed.has_value());
	CHECK(Near(aimed->left->aim->pitchDeg, 123.0));
	CHECK(Near(aimed->left->aim->yawDeg, 14.0));
	CHECK(Near(aimed->right->aim->pitchDeg, 40.0));
	CHECK(Near(aimed->right->aim->yawDeg, kHandYawDeg));

	const auto plain = Parse(R"({"hands":{"left":{"world":[0,0,0]}}})", error);
	CHECK(!plain->left->aim.has_value());

	CHECK(!Parse(R"({"hands":{"left":{"world":[0,0,0],"pitch":200}}})", error).has_value());
	CHECK(error.find("hands.left.pitch") != std::string::npos);
	CHECK(!Parse(R"({"hands":{"left":{"world":[0,0,0],"yaw":"in"}}})", error).has_value());
}

TEST_CASE("ParsePose takes a world hand's head offset and an inward yaw per side")
{
	std::string error;
	const auto  request = Parse(R"({"hands":{"left":{"world":[1,2,3],"headOffset":[4,5,6],"yawIn":20},"right":{"world":[1,2,3],"headOffset":[4,5,6],"yawIn":20}}})", error);
	CHECK(request.has_value());
	CHECK(NearVec(*request->left->headOffset, Vec3{ 4, 5, 6 }));
	CHECK(NearVec(*request->right->headOffset, Vec3{ 4, 5, 6 }));
	// toward the body: the left hand turns right (negative tracking yaw), the right one left
	CHECK(Near(request->left->aim->yawDeg, -20.0));
	CHECK(Near(request->right->aim->yawDeg, 20.0));
	CHECK(Near(request->left->aim->pitchDeg, kHandPitchDeg));

	const auto plain = Parse(R"({"hands":{"left":{"world":[0,0,0]}}})", error);
	CHECK(!plain->left->headOffset.has_value());

	CHECK(!Parse(R"({"hands":{"left":{"world":[0,0,0],"yaw":5,"yawIn":5}}})", error).has_value());
	CHECK(error.find("yawIn") != std::string::npos);
	CHECK(!Parse(R"({"hands":{"left":{"world":[0,0,0],"yawIn":181}}})", error).has_value());
	CHECK(error.find("hands.left.yawIn") != std::string::npos);
	CHECK(!Parse(R"({"hands":{"left":{"node":"Chest","headOffset":[0,0,0]}}})", error).has_value());
	CHECK(error.find("hands.left.headOffset") != std::string::npos);
	CHECK(!Parse(R"({"hands":{"left":{"world":[0,0,0],"headOffset":[0,0]}}})", error).has_value());
	CHECK(!Parse(R"({"hands":{"left":{"world":[0,0,0],"headOffset":[0,0,5000]}}})", error).has_value());
}

TEST_CASE("HeadOffsetWorld mirrors the outward axis per hand and follows a pitched head")
{
	const Vec3 center{ 100.0, 200.0, 50.0 };
	const Vec3 offset{ 3.0, 4.0, 5.0 };

	// level, facing +y
	const HeadAxes level{ Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, 1 } };
	CHECK(NearVec(HeadOffsetWorld(center, offset, level, Hand::Right), Vec3{ 103.0, 204.0, 55.0 }));
	CHECK(NearVec(HeadOffsetWorld(center, offset, level, Hand::Left), Vec3{ 97.0, 204.0, 55.0 }));

	// pitched 30 degrees down: forward dips, up leans forward
	const double   p = 30.0 * kPi / 180.0;
	const HeadAxes pitched{ Vec3{ 1, 0, 0 }, Vec3{ 0, std::cos(p), -std::sin(p) }, Vec3{ 0, std::sin(p), std::cos(p) } };
	const Vec3     forwardOnly = HeadOffsetWorld(center, Vec3{ 0, 10, 0 }, pitched, Hand::Right);
	CHECK(NearVec(forwardOnly, Vec3{ 100.0, 200.0 + 10.0 * std::cos(p), 50.0 - 10.0 * std::sin(p) }));
	const Vec3 left = HeadOffsetWorld(center, offset, pitched, Hand::Left);
	const Vec3 right = HeadOffsetWorld(center, offset, pitched, Hand::Right);
	// the two hands differ only along the head's right axis, mirrored around its center
	CHECK(Near(left.y, right.y));
	CHECK(Near(left.z, right.z));
	CHECK(Near(left.x + right.x, 2.0 * center.x));
}

TEST_CASE("ParsePose refuses malformed requests")
{
	std::string error;
	CHECK(!Parse(R"({"hands":{"middle":"ready"}})", error).has_value());
	CHECK(!Parse(R"({"hands":{"left":"wave"}})", error).has_value());
	CHECK(!Parse(R"({"hands":{"left":{"node":"Chest","world":[0,0,0]}}})", error).has_value());
	CHECK(!Parse(R"({"hands":{"left":{}}})", error).has_value());
	CHECK(!Parse(R"({"hands":{"left":{"node":"Chest","root":"head"}}})", error).has_value());
	CHECK(!Parse(R"({"head":{"pitch":-95}})", error).has_value());
	CHECK(!Parse(R"({"head":{"back":0.5}})", error).has_value());
	CHECK(!Parse(R"({"moveMs":-1})", error).has_value());
	CHECK(!Parse(R"({"corrections":9})", error).has_value());
	CHECK(!Parse(R"({"head":{"eyeHeight":4}})", error).has_value());
	CHECK(error.find("head.eyeHeight") != std::string::npos);
	CHECK(!Parse(R"({"head":{"cameraHeight":0}})", error).has_value());
	CHECK(error.find("head.cameraHeight") != std::string::npos);
}

TEST_CASE("ReadyHand mirrors the hands and swings them left")
{
	const Vec3 left = ReadyHand(Hand::Left, 1.11);
	const Vec3 right = ReadyHand(Hand::Right, 1.11);
	CHECK(Near(left.y, 1.11 - kReadyBelow));
	CHECK(Near(right.y, left.y));
	// both in front, the swing puts the pair's middle left of center
	CHECK(left.z < 0.0);
	CHECK(right.z < 0.0);
	CHECK((left.x + right.x) / 2.0 < 0.0);
	CHECK(Near(std::hypot(right.x - left.x, right.z - left.z), 2.0 * kReadySide));
	CHECK(Near(std::hypot(left.x, left.z), std::hypot(kReadySide, kReadyForward)));
}

TEST_CASE("Oriented pitches the forward axis down for a negative pitch")
{
	const Pose head = Oriented(Vec3{ 0.0, 1.5, 0.0 }, -30.0);
	// forward is -z: the third column, negated
	const Vec3 forward{ -head[2], -head[6], -head[10] };
	CHECK(Near(forward.y, -std::sin(30.0 * kPi / 180.0)));
	CHECK(Near(forward.x, 0.0));
	CHECK(Near(head[7], 1.5));

	const Pose turned = Oriented(Vec3{}, 0.0, 90.0);
	const Vec3 turnedForward{ -turned[2], -turned[6], -turned[10] };
	CHECK(NearVec(turnedForward, Vec3{ -1.0, 0.0, 0.0 }));
}

TEST_CASE("EyeHeightFor lands the camera at its target over the room lift")
{
	// seated FRIK: a 37.4-unit lift and a 115-unit camera give the 1.11 m eye height measured live
	CHECK(Near(EyeHeightFor(115.0, 37.3882, 70.0), 1.10874, 1e-4));
	// no lift: the whole camera height is the head's
	CHECK(Near(EyeHeightFor(115.0, 0.0, 70.0), 115.0 / 70.0));
	CHECK(Near(EyeHeightFor(10.0, 200.0, 70.0), kEyeHeightMin));
	CHECK(Near(EyeHeightFor(115.0, 0.0, 0.0), kEyeHeightMin));
}

TEST_CASE("FitTurn recovers the turn and TrackingFor inverts WorldFor")
{
	const Vec3   hmdTracking{ 0.0, 1.11, 0.0 };
	const Vec3   hmdWorld{ -1759.0, -1369.4, 115.1 };
	const double theta = 66.71 * kPi / 180.0;
	const Vec3   left = ReadyHand(Hand::Left, 1.11);
	const Vec3   right = ReadyHand(Hand::Right, 1.11);
	const double fitted = FitTurn(hmdTracking, hmdWorld,
		{ HandSample{ left, WorldFor(theta, hmdTracking, hmdWorld, left, 70.0) }, HandSample{ right, WorldFor(theta, hmdTracking, hmdWorld, right, 70.0) } });
	CHECK(Near(fitted, theta, 1e-9));

	const Vec3 belt{ -1765.0, -1372.0, 75.0 };
	const Vec3 tracking = TrackingFor(fitted, hmdTracking, hmdWorld, belt, 70.0);
	CHECK(NearVec(WorldFor(fitted, hmdTracking, hmdWorld, tracking, 70.0), belt, 1e-6));

	// a world miss turns into the tracking move that removes it
	const Vec3 miss{ 3.0, -2.0, 1.0 };
	const Vec3 moved = WorldFor(fitted, hmdTracking, hmdWorld, dvb::ReachLogic::Add(tracking, TrackingDelta(fitted, miss, 70.0)), 70.0);
	CHECK(NearVec(moved, dvb::ReachLogic::Add(belt, miss), 1e-6));

	CHECK(Near(FitTurn(hmdTracking, hmdWorld, {}), 0.0));
}

TEST_CASE("GameAxes and TrackingAxes swap OpenVR and world axes")
{
	const Vec3 tracking{ 0.2, 1.0, -0.5 };
	CHECK(NearVec(GameAxes(tracking), Vec3{ 0.2, 0.5, 1.0 }));
	CHECK(NearVec(TrackingAxes(GameAxes(tracking)), tracking));
}

TEST_CASE("MoveFrames eases from one held pose to another, then holds")
{
	HeldPose from{ 1.11, kReadyHeadPitchDeg, ReadyHand(Hand::Left, 1.11), ReadyHand(Hand::Right, 1.11) };
	HeldPose to = from;
	to.headPitchDeg = -60.0;
	to.left = Vec3{ -0.1, 0.55, -0.05 };

	const json frames = MoveFrames(from, to, 300, 3000);
	const long steps = 300 / kMoveStepMs;
	CHECK(frames.size() == static_cast<std::size_t>(steps + 1 + 3));
	CHECK(frames[0]["tMs"].get<long>() == 0);
	CHECK(NearVec(Translation(frames[0], "left"), from.left));
	CHECK(NearVec(Translation(frames[static_cast<std::size_t>(steps)], "left"), to.left));
	CHECK(frames[static_cast<std::size_t>(steps)]["tMs"].get<long>() == 300);
	CHECK(frames.back()["tMs"].get<long>() == 3300);
	CHECK(NearVec(Translation(frames.back(), "right"), from.right));
	CHECK(NearVec(Translation(frames.back(), "hmd"), Vec3{ 0.0, 1.11, 0.0 }));
	for (std::size_t i = 1; i < frames.size(); ++i)
		CHECK(frames[i]["tMs"].get<long>() > frames[i - 1]["tMs"].get<long>());

	// no move: one frame at the target, then the hold
	const json instant = MoveFrames(from, to, 0, 1000);
	CHECK(instant.size() == 2);
	CHECK(NearVec(Translation(instant[0], "left"), to.left));
}

TEST_CASE("MoveFrames turns a controller to its aim with the move")
{
	HeldPose from{ 1.11, kReadyHeadPitchDeg, ReadyHand(Hand::Left, 1.11), ReadyHand(Hand::Right, 1.11) };
	HeldPose to = from;
	to.leftAim = HandAim{ 123.0, 14.0 };

	const json frames = MoveFrames(from, to, 100, 1000);
	const auto forward = [](const json& a_frame, const char* a_device) {
		const json& m = a_frame[a_device]["matrix"];
		return Vec3{ -m[2].get<double>(), -m[6].get<double>(), -m[10].get<double>() };
	};
	const Pose ready = Oriented(Vec3{}, kHandPitchDeg, kHandYawDeg);
	const Pose aimed = Oriented(Vec3{}, 123.0, 14.0);
	CHECK(NearVec(forward(frames.front(), "left"), Vec3{ -ready[2], -ready[6], -ready[10] }));
	CHECK(NearVec(forward(frames.back(), "left"), Vec3{ -aimed[2], -aimed[6], -aimed[10] }));
	// tilted back past vertical: pointing up and behind
	CHECK(forward(frames.back(), "left").y > 0.8);
	CHECK(forward(frames.back(), "left").z > 0.5);
	// the other controller keeps the ready aim
	CHECK(NearVec(forward(frames.back(), "right"), Vec3{ -ready[2], -ready[6], -ready[10] }));
}

TEST_CASE("MoveFrames turns the head's yaw with the move, at the eyes")
{
	HeldPose from{ 1.11, kReadyHeadPitchDeg, ReadyHand(Hand::Left, 1.11), ReadyHand(Hand::Right, 1.11) };
	HeldPose to = from;
	to.headYawDeg = 40.0;

	const json frames = MoveFrames(from, to, 100, 1000);
	const auto matrixOf = [](const json& a_frame) {
		const json& m = a_frame["hmd"]["matrix"];
		Pose        pose{};
		for (std::size_t i = 0; i < 12; ++i)
			pose[i] = m[i].get<double>();
		return pose;
	};
	const Pose start = Oriented(Vec3{ 0.0, 1.11, 0.0 }, kReadyHeadPitchDeg, 0.0);
	const Pose end = Oriented(Vec3{ 0.0, 1.11, 0.0 }, kReadyHeadPitchDeg, 40.0);
	for (std::size_t i = 0; i < 12; ++i) {
		CHECK(Near(matrixOf(frames.front())[i], start[i]));
		CHECK(Near(matrixOf(frames.back())[i], end[i]));
	}
	CHECK(NearVec(Translation(frames.back(), "hmd"), Vec3{ 0.0, 1.11, 0.0 }));
}

TEST_CASE("ParsePose takes head presets, yaw and lookAt targets")
{
	std::string error;
	const auto  ready = Parse(R"({"head":"ready"})", error);
	CHECK(ready.has_value());
	CHECK(Near(*ready->headPitch, kReadyHeadPitchDeg));
	CHECK(Near(*ready->headYaw, 0.0));

	const auto down = Parse(R"({"head":"down"})", error);
	CHECK(Near(*down->headPitch, kDownHeadPitchDeg));
	CHECK(Near(*down->headYaw, 0.0));

	const auto turned = Parse(R"({"head":{"pitch":-10,"yaw":35}})", error);
	CHECK(Near(*turned->headPitch, -10.0));
	CHECK(Near(*turned->headYaw, 35.0));
	CHECK(!turned->lookAt.has_value());

	const auto left = Parse(R"({"head":{"lookAt":"left"}})", error);
	CHECK(left->lookAt->kind == HeadLook::Kind::kLeft);
	CHECK(!left->headPitch.has_value());
	CHECK(Parse(R"({"head":{"lookAt":"right"}})", error)->lookAt->kind == HeadLook::Kind::kRight);
	CHECK(Parse(R"({"head":{"lookAt":"hands"}})", error)->lookAt->kind == HeadLook::Kind::kHands);

	const auto node = Parse(R"({"head":{"lookAt":"Chest","root":"player"}})", error);
	CHECK(node->lookAt->kind == HeadLook::Kind::kNode);
	CHECK(node->lookAt->node == "Chest");
	CHECK(node->lookAt->root == "player");

	const auto world = Parse(R"({"head":{"lookAt":[-1765,-1372,75]}})", error);
	CHECK(world->lookAt->kind == HeadLook::Kind::kWorld);
	CHECK(NearVec(world->lookAt->world, Vec3{ -1765, -1372, 75 }));

	CHECK(Parse(R"({})", error)->holdMs == 60000);
}

TEST_CASE("ParsePose refuses a lookAt beside angles and malformed heads")
{
	std::string error;
	CHECK(!Parse(R"({"head":{"lookAt":"hands","pitch":-20}})", error).has_value());
	CHECK(error.find("lookAt") != std::string::npos);
	CHECK(!Parse(R"({"head":{"lookAt":"left","yaw":10}})", error).has_value());
	CHECK(!Parse(R"({"head":"up"})", error).has_value());
	CHECK(!Parse(R"({"head":{"pitch":-87}})", error).has_value());
	CHECK(Parse(R"({"head":{"pitch":-85}})", error).has_value());
	CHECK(!Parse(R"({"head":{"yaw":200}})", error).has_value());
	CHECK(error.find("head.yaw") != std::string::npos);
	CHECK(!Parse(R"({"head":{"root":"player"}})", error).has_value());
	CHECK(!Parse(R"({"head":{"lookAt":"hands","root":"player"}})", error).has_value());
	CHECK(!Parse(R"({"head":{"lookAt":"Chest","root":"head"}})", error).has_value());
	CHECK(!Parse(R"({"head":{"lookAt":5}})", error).has_value());
	CHECK(!Parse(R"({"head":{"lookAt":""}})", error).has_value());
	CHECK(!Parse(R"({"head":{"lookAt":[1,2]}})", error).has_value());
	CHECK(!Parse(R"({"head":{"pos":[0,0,0]}})", error).has_value());
}

TEST_CASE("ParsePose takes hands 'ready' as both hands ready")
{
	std::string error;
	const auto  both = Parse(R"({"hands":"ready","head":"ready"})", error);
	CHECK(both.has_value());
	CHECK(both->left->kind == HandPlacement::Kind::kReady);
	CHECK(both->right->kind == HandPlacement::Kind::kReady);
	CHECK(!Parse(R"({"hands":"wave"})", error).has_value());
	CHECK(!Parse(R"({"hands":5})", error).has_value());
}

TEST_CASE("LookAngles aims the head's forward at the target through the turn")
{
	const Vec3 eyes{ 100.0, 200.0, 120.0 };
	// no turn: the world's +y is tracking forward
	const auto ahead = LookAngles(0.0, eyes, dvb::ReachLogic::Add(eyes, Vec3{ 0.0, 50.0, 0.0 }), 70.0);
	CHECK(ahead.has_value());
	CHECK(Near(ahead->pitchDeg, 0.0));
	CHECK(Near(ahead->yawDeg, 0.0));
	CHECK(!ahead->clamped);

	const auto leftward = LookAngles(0.0, eyes, dvb::ReachLogic::Add(eyes, Vec3{ -50.0, 0.0, 0.0 }), 70.0);
	CHECK(Near(leftward->yawDeg, 90.0));
	const auto below = LookAngles(0.0, eyes, dvb::ReachLogic::Add(eyes, Vec3{ 0.0, 40.0, -40.0 }), 70.0);
	CHECK(Near(below->pitchDeg, -45.0));
	CHECK(Near(below->yawDeg, 0.0));

	// straight down clamps to the pitch limit
	const auto feet = LookAngles(0.0, eyes, dvb::ReachLogic::Add(eyes, Vec3{ 0.0, 0.0, -100.0 }), 70.0);
	CHECK(Near(feet->pitchDeg, -kHeadPitchLimitDeg));
	CHECK(feet->clamped);

	// at the eyes there is no direction
	CHECK(!LookAngles(0.0, eyes, dvb::ReachLogic::Add(eyes, Vec3{ 0.0, 0.5, 0.0 }), 70.0).has_value());

	// with a turn, Oriented's forward at the angles points along the target's tracking offset
	const double theta = 66.71 * kPi / 180.0;
	const Vec3   target = dvb::ReachLogic::Add(eyes, Vec3{ 30.0, -20.0, -25.0 });
	const auto   aimed = LookAngles(theta, eyes, target, 70.0);
	const Pose   head = Oriented(Vec3{}, aimed->pitchDeg, aimed->yawDeg);
	const Vec3   forward{ -head[2], -head[6], -head[10] };
	const Vec3   offset = TrackingDelta(theta, dvb::ReachLogic::Sub(target, eyes), 70.0);
	const double length = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
	CHECK(NearVec(forward, dvb::ReachLogic::Scale(offset, 1.0 / length), 1e-9));
}
