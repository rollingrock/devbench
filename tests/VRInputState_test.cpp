#include "test_framework.h"

#include "core/VRInputState.h"

#include <cmath>

using dvb::json;
using dvb::ParseBoundedIntegerArgument;
using dvb::ParseVRTrackedInputFrames;
using dvb::VRSequenceFinishAction;
using dvb::VRSequenceStopAccess;
using dvb::VRSequenceTransaction;
using dvb::VRTrackedInputFrameJson;

namespace
{
	json Pose(int a_index)
	{
		return json{ { "available", true }, { "connected", true }, { "valid", true },
			{ "index", a_index }, { "trackingResult", 200 },
			{ "matrix", json::array({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 }) },
			{ "velocity", json::array({ 0, 0, 0 }) },
			{ "angularVelocity", json::array({ 0, 0, 0 }) } };
	}

	json Controller(int a_index)
	{
		json out = Pose(a_index);
		out["controller"] = json{ { "packetNumber", 4 }, { "pressed", 8 }, { "touched", 8 },
			{ "axes", json::array({ json::array({ 0.25, -0.5 }), json::array({ 0, 0 }),
						  json::array({ 0, 0 }), json::array({ 0, 0 }), json::array({ 0, 0 }) }) } };
		return out;
	}

	json Frame(std::int64_t a_tMs)
	{
		return json{ { "tMs", a_tMs }, { "originCode", 1 }, { "hmd", Pose(0) },
			{ "left", Controller(1) }, { "right", Controller(2) } };
	}
}

TEST_CASE("atomic VR tracked-set frames validate and round trip")
{
	const auto frames = ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(20) }));
	CHECK(frames.size() == 2);
	CHECK(frames[1].tMs == 20);
	CHECK(frames[0].right.controller.pressed == 8);
	const json encoded = VRTrackedInputFrameJson(frames[0]);
	CHECK(encoded["hmd"]["index"] == 0);
	CHECK(encoded["left"]["controller"]["axes"][0][0] == 0.25f);
}

TEST_CASE("bounded JSON integers reject overflow before narrowing")
{
	CHECK(ParseBoundedIntegerArgument(json::object(), "tailMs", 50, 10, 1000) == 50);
	CHECK(ParseBoundedIntegerArgument(json{ { "tailMs", 1000 } }, "tailMs", 50, 10, 1000) == 1000);
	CHECK_THROWS_AS(ParseBoundedIntegerArgument(
						json{ { "tailMs", 4294967306ULL } }, "tailMs", 50, 10, 1000),
		std::invalid_argument);
	CHECK_THROWS_AS(ParseBoundedIntegerArgument(
						json{ { "tailMs", -4294967306LL } }, "tailMs", 50, 10, 1000),
		std::invalid_argument);
	CHECK_THROWS_AS(ParseBoundedIntegerArgument(
						json::parse(R"({"tailMs":5})"), "tailMs", 50, 10, 1000),
		std::invalid_argument);
}

TEST_CASE("atomic VR tracked-set validation rejects incomplete or incoherent sequences")
{
	json missing = Frame(0);
	missing.erase("right");
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ missing })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(10) })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(20), Frame(10) })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(0) })));

	json duplicate = Frame(0);
	duplicate["right"]["index"] = 1;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ duplicate })));

	json badAxis = Frame(0);
	badAxis["left"]["controller"]["axes"][0][0] = 2.0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ badAxis })));

	json changedIdentity = Frame(20);
	changedIdentity["right"]["index"] = 3;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), changedIdentity })));

	json changedOrigin = Frame(20);
	changedOrigin["originCode"] = 0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), changedOrigin })));

	json unavailable = Frame(0);
	unavailable["right"] = json{ { "available", false }, { "connected", false }, { "valid", false } };
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ unavailable })));

	json hmdAlias = Frame(0);
	hmdAlias["left"]["index"] = 0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ hmdAlias })));

	json badBoolean = Frame(0);
	badBoolean["hmd"]["available"] = "yes";
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ badBoolean })));

	json badSeq = Frame(20);
	badSeq["seq"] = 1;
	json first = Frame(0);
	first["seq"] = 1;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ first, badSeq })));

	for (const auto& field : { "trackingResult", "packetNumber", "originCode" }) {
		json overflow = Frame(0);
		if (std::string_view(field) == "trackingResult")
			overflow["hmd"][field] = 4294967296ULL;
		else if (std::string_view(field) == "packetNumber")
			overflow["left"]["controller"][field] = 4294967296ULL;
		else
			overflow[field] = 4294967296ULL;
		CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ overflow })));
	}
}

TEST_CASE("VR sequence transaction cancels startup before mutation")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("owner", "secret", false));
	const auto generation = state.Generation();
	CHECK(state.Starting());

	const auto decision = state.CancelForLifecycle();
	CHECK(decision.present);
	CHECK(!decision.preserved);
	CHECK(decision.generation == generation);
	CHECK(!state.CanApplyIndices(generation));
	CHECK(state.Busy());
	CHECK(state.Restoring());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kPublish);
	CHECK(!state.Busy());
}

TEST_CASE("VR lifecycle cleanup reserves an applied controller transaction")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("owner", "secret", false));
	const auto generation = state.Generation();
	CHECK(state.MarkIndicesApplied(generation));
	CHECK(state.Commit(generation));

	const auto decision = state.CancelForLifecycle();
	CHECK(decision.present);
	CHECK(!decision.preserved);
	CHECK(state.Busy());
	CHECK(state.Restoring());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kRestore);
}

TEST_CASE("VR sequence restoration is retained and retryable")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("owner", "secret", false));
	const auto generation = state.Generation();
	CHECK(state.MarkIndicesApplied(generation));
	CHECK(state.Commit(generation));
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kRestore);
	CHECK(state.Restoring());
	CHECK(state.RestoreAttemptActive());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kNone);
	CHECK(state.RestoreAttemptActive());

	state.RestoreFailed(generation);
	CHECK(state.Restoring());
	CHECK(!state.RestoreAttemptActive());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kRestore);
	CHECK(state.RestoreSucceeded(generation));
	CHECK(!state.Busy());
	CHECK(!state.IndicesApplied());
	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kNone);
}

TEST_CASE("VR sequence stop authority is token and owner scoped")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("owner", "secret", false));
	CHECK(state.AuthorizeStop("owner", "wrong", false, false) ==
		  VRSequenceStopAccess::kControlTokenMismatch);
	CHECK(state.AuthorizeStop("other", "secret", false, false) ==
		  VRSequenceStopAccess::kOwnerMismatch);
	CHECK(state.AuthorizeStop("owner", "secret", true, false) ==
		  VRSequenceStopAccess::kForceRequiresInternal);
	CHECK(state.AuthorizeStop("owner", "secret", false, false) ==
		  VRSequenceStopAccess::kAllowed);
	CHECK(state.AuthorizeStop("other", "", true, true) ==
		  VRSequenceStopAccess::kAllowed);
}

TEST_CASE("VR sequence replace needs a running sequence, its owner and its token")
{
	VRSequenceTransaction state;
	CHECK(!state.CanReplace("owner", "secret"));
	CHECK(state.Begin("owner", "secret", false));
	const auto generation = state.Generation();
	CHECK(!state.CanReplace("owner", "secret"));
	CHECK(state.MarkIndicesApplied(generation));
	CHECK(state.Commit(generation));
	CHECK(state.CanReplace("owner", "secret"));
	CHECK(!state.CanReplace("owner", "wrong"));
	CHECK(!state.CanReplace("owner", ""));
	CHECK(!state.CanReplace("other", "secret"));
	CHECK(state.Generation() == generation);

	CHECK(state.ClaimFinish(generation) == VRSequenceFinishAction::kRestore);
	CHECK(!state.CanReplace("owner", "secret"));
}

TEST_CASE("VR sequence replace is refused once lifecycle cleanup cancels it")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("owner", "secret", false));
	const auto generation = state.Generation();
	CHECK(state.MarkIndicesApplied(generation));
	CHECK(state.Commit(generation));
	state.CancelForLifecycle();
	CHECK(!state.CanReplace("owner", "secret"));
}

TEST_CASE("only internal replay may survive lifecycle cleanup")
{
	VRSequenceTransaction state;
	CHECK(state.Begin("replay", "secret", true));
	const auto decision = state.CancelForLifecycle();
	CHECK(decision.present);
	CHECK(decision.preserved);
	CHECK(state.Starting());
}

TEST_CASE("press maps button names to OpenVR bits, touch and analog axes")
{
	const auto press = dvb::ParseVRPress(json{ { "hand", "left" }, { "buttons", json::array({ "trigger", "a" }) } });
	CHECK(!press.right);
	CHECK(press.pressed == ((1ull << 33) | (1ull << 7)));
	CHECK(press.touched == press.pressed);
	CHECK(press.axes[2].has_value());
	CHECK(!press.axes[0].has_value());
	CHECK(press.durationMs == dvb::kDefaultPressMs);
	CHECK(press.delayMs == 0);
}

TEST_CASE("press rejects a missing hand, unknown buttons and out-of-range timing")
{
	CHECK_THROWS(dvb::ParseVRPress(json{ { "buttons", json::array({ "trigger" }) } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "middle" }, { "buttons", json::array({ "trigger" }) } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "right" }, { "buttons", json::array() } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "right" }, { "buttons", json::array({ "jump" }) } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "right" }, { "buttons", json::array({ "grip" }) }, { "durationMs", 5 } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "right" }, { "buttons", json::array({ "grip" }) }, { "delayMs", -1 } }));
}

TEST_CASE("press takes a stick alone or with buttons, on axis 0 with the stick touched")
{
	const auto stick = dvb::ParseVRPress(json{ { "hand", "right" }, { "stick", json::array({ 0.0, -1.0 }) } });
	CHECK(stick.right);
	CHECK(stick.stick.has_value());
	CHECK(stick.pressed == 0);
	CHECK(stick.touched == (1ull << 32));
	CHECK(std::abs(*stick.axes[0]) <= 1e-6f);
	CHECK(std::abs(*stick.axes[1] + 1.0f) <= 1e-6f);
	CHECK(stick.buttons.empty());

	const auto both = dvb::ParseVRPress(json{ { "hand", "left" }, { "buttons", json::array({ "trigger" }) }, { "stick", json::array({ 0.5, 0 }) } });
	CHECK(both.pressed == (1ull << 33));
	CHECK(both.touched == ((1ull << 33) | (1ull << 32)));
	CHECK(std::abs(*both.axes[0] - 0.5f) <= 1e-6f);
	CHECK(std::abs(*both.axes[2] - 1.0f) <= 1e-6f);
}

TEST_CASE("press rejects a stick outside [-1, 1], a malformed stick and neither buttons nor stick")
{
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "left" } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "left" }, { "stick", json::array({ 1.5, 0 }) } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "left" }, { "stick", json::array({ 0 }) } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "left" }, { "stick", json::array({ "up", 0 }) } }));
	CHECK_THROWS(dvb::ParseVRPress(json{ { "hand", "left" }, { "stick", json::array({ 0, 1 }) }, { "buttons", json::array() } }));
}

TEST_CASE("a stick press replaces the frame's stick and leaves its other axes")
{
	dvb::VRControllerState state;
	state.axes[0] = 0.3f;
	state.axes[2] = 0.7f;
	const auto out = dvb::ApplyVRPress(state, dvb::ParseVRPress(json{ { "hand", "left" }, { "stick", json::array({ -1, 0.25 }) } }));
	CHECK(std::abs(out.axes[0] + 1.0f) <= 1e-6f);
	CHECK(std::abs(out.axes[1] - 0.25f) <= 1e-6f);
	CHECK(std::abs(out.axes[2] - 0.7f) <= 1e-6f);
	CHECK(out.pressed == 0);
}

TEST_CASE("press lays its state over the frame's controller state")
{
	dvb::VRControllerState state;
	state.packetNumber = 41;
	state.pressed = 1ull << 1;
	state.axes[0] = 0.5f;
	const auto press = dvb::ParseVRPress(json{ { "hand", "right" }, { "buttons", json::array({ "grip" }) } });
	const auto out = dvb::ApplyVRPress(state, press);
	CHECK(out.pressed == ((1ull << 1) | (1ull << 2)));
	CHECK(out.touched == (1ull << 2));
	CHECK(std::abs(out.axes[0] - 0.5f) <= 1e-6f);
	CHECK(std::abs(out.axes[4] - 1.0f) <= 1e-6f);
	CHECK(out.packetNumber == 41);
}
