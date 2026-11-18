#include "core/PoseLogic.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dvb::PoseLogic
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kEpsilon = 1e-12;

		double Radians(double a_degrees)
		{
			return a_degrees * kPi / 180.0;
		}

		double EaseInOut(double a_t)
		{
			return a_t * a_t * (3.0 - 2.0 * a_t);
		}

		bool ReadNumber(const json& a_value, const std::string& a_path, double a_min, double a_max, double& a_out, std::string& a_error)
		{
			const std::string range = std::format("{}..{}", a_min, a_max);
			if (!a_value.is_number() || !std::isfinite(a_value.get<double>()) || a_value.get<double>() < a_min || a_value.get<double>() > a_max) {
				a_error = std::format("'{}' must be a number {}", a_path, range);
				return false;
			}
			a_out = a_value.get<double>();
			return true;
		}

		bool ReadInteger(const json& a_value, const std::string& a_path, long a_min, long a_max, long& a_out, std::string& a_error)
		{
			if (!a_value.is_number_integer() || a_value.get<long long>() < a_min || a_value.get<long long>() > a_max) {
				a_error = std::format("'{}' must be an integer {}..{}", a_path, a_min, a_max);
				return false;
			}
			a_out = static_cast<long>(a_value.get<long long>());
			return true;
		}

		bool ReadVec3(const json& a_value, const std::string& a_path, double a_limit, Vec3& a_out, std::string& a_error)
		{
			if (!a_value.is_array() || a_value.size() != 3) {
				a_error = std::format("'{}' must be an array of three numbers within +-{}", a_path, a_limit);
				return false;
			}
			double components[3]{};
			for (std::size_t i = 0; i < 3; ++i) {
				if (!ReadNumber(a_value[i], std::format("{}[{}]", a_path, i), -a_limit, a_limit, components[i], a_error))
					return false;
			}
			a_out = Vec3{ components[0], components[1], components[2] };
			return true;
		}

		/// Reads a node's skeleton: "player" (body) or "firstPerson".
		bool ReadRoot(const json& a_value, const std::string& a_path, std::string& a_out, std::string& a_error)
		{
			if (!a_value.is_string() || (a_value.get<std::string>() != "player" && a_value.get<std::string>() != "firstPerson")) {
				a_error = std::format("'{}' must be \"player\" or \"firstPerson\"", a_path);
				return false;
			}
			a_out = a_value.get<std::string>();
			return true;
		}

		/**
		 * Reads one hand's placement. a_hand picks the side yawIn turns toward the body from, so
		 * the placement carries the tracking yaw.
		 */
		bool ReadHand(const json& a_value, const std::string& a_path, Hand a_hand, HandPlacement& a_out, std::string& a_error)
		{
			if (a_value.is_string()) {
				if (a_value.get<std::string>() != "ready") {
					a_error = std::format("'{}' must be \"ready\" or an object", a_path);
					return false;
				}
				a_out.kind = HandPlacement::Kind::kReady;
				return true;
			}
			if (!a_value.is_object()) {
				a_error = std::format("'{}' must be \"ready\" or an object", a_path);
				return false;
			}
			const bool hasNode = a_value.contains("node");
			const bool hasWorld = a_value.contains("world");
			if (hasNode == hasWorld) {
				a_error = std::format("'{}' needs exactly one of 'node' or 'world'", a_path);
				return false;
			}
			if (a_value.contains("yaw") && a_value.contains("yawIn")) {
				a_error = std::format("'{}' takes 'yaw' or 'yawIn', not both", a_path);
				return false;
			}
			if (a_value.contains("pitch") || a_value.contains("yaw") || a_value.contains("yawIn")) {
				HandAim aim = kReadyAim;
				if (a_value.contains("pitch") && !ReadNumber(a_value["pitch"], a_path + ".pitch", -kHandAimLimitDeg, kHandAimLimitDeg, aim.pitchDeg, a_error))
					return false;
				if (a_value.contains("yaw") && !ReadNumber(a_value["yaw"], a_path + ".yaw", -kHandAimLimitDeg, kHandAimLimitDeg, aim.yawDeg, a_error))
					return false;
				if (a_value.contains("yawIn")) {
					double yawIn = 0.0;
					if (!ReadNumber(a_value["yawIn"], a_path + ".yawIn", -kHandAimLimitDeg, kHandAimLimitDeg, yawIn, a_error))
						return false;
					aim.yawDeg = YawInToTracking(yawIn, a_hand);
				}
				a_out.aim = aim;
			}
			if (hasWorld) {
				a_out.kind = HandPlacement::Kind::kWorld;
				if (!ReadVec3(a_value["world"], a_path + ".world", kWorldLimit, a_out.world, a_error))
					return false;
				if (a_value.contains("headOffset")) {
					Vec3 offset;
					if (!ReadVec3(a_value["headOffset"], a_path + ".headOffset", ReachLogic::kOffsetLimit, offset, a_error))
						return false;
					a_out.headOffset = offset;
				}
				return true;
			}
			if (a_value.contains("headOffset")) {
				a_error = std::format("'{}.headOffset' goes with a world target, not a node", a_path);
				return false;
			}
			if (!a_value["node"].is_string() || a_value["node"].get<std::string>().empty() || a_value["node"].get<std::string>().size() > ReachLogic::kNodeMaxLen) {
				a_error = std::format("'{}.node' must be a non-empty string of at most {} characters", a_path, ReachLogic::kNodeMaxLen);
				return false;
			}
			a_out.kind = HandPlacement::Kind::kNode;
			a_out.node = a_value["node"].get<std::string>();
			if (a_value.contains("root") && !ReadRoot(a_value["root"], a_path + ".root", a_out.root, a_error))
				return false;
			if (a_value.contains("front") && !ReadNumber(a_value["front"], a_path + ".front", ReachLogic::kFrontMin, ReachLogic::kFrontMax, a_out.front, a_error))
				return false;
			if (a_value.contains("offset") && !ReadVec3(a_value["offset"], a_path + ".offset", ReachLogic::kOffsetLimit, a_out.offset, a_error))
				return false;
			return true;
		}

		/**
		 * Reads head.lookAt: "left", "right", "hands", a node name, or a world point [x,y,z].
		 */
		bool ReadLook(const json& a_value, HeadLook& a_out, std::string& a_error)
		{
			if (a_value.is_array()) {
				a_out.kind = HeadLook::Kind::kWorld;
				return ReadVec3(a_value, "head.lookAt", kWorldLimit, a_out.world, a_error);
			}
			if (!a_value.is_string() || a_value.get<std::string>().empty() || a_value.get<std::string>().size() > ReachLogic::kNodeMaxLen) {
				a_error = std::format("'head.lookAt' must be \"left\", \"right\", \"hands\", a node name of at most {} characters, or [x,y,z]", ReachLogic::kNodeMaxLen);
				return false;
			}
			const std::string target = a_value.get<std::string>();
			if (target == "left")
				a_out.kind = HeadLook::Kind::kLeft;
			else if (target == "right")
				a_out.kind = HeadLook::Kind::kRight;
			else if (target == "hands")
				a_out.kind = HeadLook::Kind::kHands;
			else {
				a_out.kind = HeadLook::Kind::kNode;
				a_out.node = target;
			}
			return true;
		}

		/**
		 * Reads 'head': the preset "ready" or "down", or {pitch?, yaw?, lookAt?, root?, eyeHeight?,
		 * cameraHeight?}. lookAt computes the angles, so it refuses pitch and yaw beside it.
		 */
		bool ReadHead(const json& a_head, PoseRequest& a_request, std::string& a_error)
		{
			if (a_head.is_string()) {
				const std::string preset = a_head.get<std::string>();
				if (preset != "ready" && preset != "down") {
					a_error = "'head' must be \"ready\", \"down\" or an object";
					return false;
				}
				a_request.headPitch = preset == "ready" ? kReadyHeadPitchDeg : kDownHeadPitchDeg;
				a_request.headYaw = 0.0;
				return true;
			}
			if (!a_head.is_object()) {
				a_error = "'head' must be \"ready\", \"down\" or an object {pitch?, yaw?, lookAt?, root?, eyeHeight?, cameraHeight?}";
				return false;
			}
			for (const auto& [key, value] : a_head.items()) {
				if (key != "pitch" && key != "yaw" && key != "lookAt" && key != "root" && key != "eyeHeight" && key != "cameraHeight") {
					a_error = std::format("'head.{}' is not a pose head field (pitch|yaw|lookAt|root|eyeHeight|cameraHeight)", key);
					return false;
				}
			}
			if (a_head.contains("lookAt") && (a_head.contains("pitch") || a_head.contains("yaw"))) {
				a_error = "'head.lookAt' computes the pitch and yaw; give it without 'pitch' or 'yaw'";
				return false;
			}
			if (a_head.contains("pitch")) {
				double pitch = 0.0;
				if (!ReadNumber(a_head["pitch"], "head.pitch", -kHeadPitchLimitDeg, kHeadPitchLimitDeg, pitch, a_error))
					return false;
				a_request.headPitch = pitch;
			}
			if (a_head.contains("yaw")) {
				double yaw = 0.0;
				if (!ReadNumber(a_head["yaw"], "head.yaw", -kHeadYawLimitDeg, kHeadYawLimitDeg, yaw, a_error))
					return false;
				a_request.headYaw = yaw;
			}
			if (a_head.contains("lookAt")) {
				HeadLook look;
				if (!ReadLook(a_head["lookAt"], look, a_error))
					return false;
				a_request.lookAt = look;
			}
			if (a_head.contains("root")) {
				if (!a_request.lookAt || a_request.lookAt->kind != HeadLook::Kind::kNode) {
					a_error = "'head.root' goes with a node 'head.lookAt'";
					return false;
				}
				if (!ReadRoot(a_head["root"], "head.root", a_request.lookAt->root, a_error))
					return false;
			}
			if (a_head.contains("eyeHeight")) {
				double eye = 0.0;
				if (!ReadNumber(a_head["eyeHeight"], "head.eyeHeight", kEyeHeightMin, kEyeHeightMax, eye, a_error))
					return false;
				a_request.eyeHeight = eye;
			}
			if (a_head.contains("cameraHeight")) {
				double camera = 0.0;
				if (!ReadNumber(a_head["cameraHeight"], "head.cameraHeight", 1.0, kCameraHeightMax, camera, a_error))
					return false;
				a_request.cameraHeight = camera;
			}
			return true;
		}

		Vec3 Lerp(const Vec3& a_a, const Vec3& a_b, double a_t)
		{
			return ReachLogic::Add(a_a, ReachLogic::Scale(ReachLogic::Sub(a_b, a_a), a_t));
		}

		json FrameAt(long a_tMs, int a_packet, const HeldPose& a_pose)
		{
			return ReachLogic::MakeFrame(a_tMs, a_packet,
				Oriented(Vec3{ 0.0, a_pose.eyeHeight, 0.0 }, a_pose.headPitchDeg, a_pose.headYawDeg),
				Oriented(a_pose.left, a_pose.leftAim.pitchDeg, a_pose.leftAim.yawDeg),
				Oriented(a_pose.right, a_pose.rightAim.pitchDeg, a_pose.rightAim.yawDeg));
		}

		HandAim LerpAim(const HandAim& a_a, const HandAim& a_b, double a_t)
		{
			return HandAim{ a_a.pitchDeg + (a_b.pitchDeg - a_a.pitchDeg) * a_t, a_a.yawDeg + (a_b.yawDeg - a_a.yawDeg) * a_t };
		}
	}

	std::optional<PoseRequest> ParsePose(const json& a_args, std::string& a_error)
	{
		if (!a_args.is_object()) {
			a_error = "arguments must be an object";
			return std::nullopt;
		}
		PoseRequest request;
		if (a_args.contains("hands") && !a_args["hands"].is_null()) {
			const json& hands = a_args["hands"];
			if (hands.is_string()) {
				if (hands.get<std::string>() != "ready") {
					a_error = "'hands' must be \"ready\" or an object {left?, right?}";
					return std::nullopt;
				}
				request.left = HandPlacement{};
				request.right = HandPlacement{};
			} else if (!hands.is_object()) {
				a_error = "'hands' must be \"ready\" or an object {left?, right?}";
				return std::nullopt;
			} else {
				for (const auto& [key, value] : hands.items()) {
					if (key != "left" && key != "right") {
						a_error = std::format("'hands.{}' is not a hand (left|right)", key);
						return std::nullopt;
					}
					HandPlacement placement;
					if (!ReadHand(value, "hands." + key, key == "right" ? Hand::Right : Hand::Left, placement, a_error))
						return std::nullopt;
					(key == "left" ? request.left : request.right) = placement;
				}
			}
		}
		if (a_args.contains("head") && !a_args["head"].is_null() && !ReadHead(a_args["head"], request, a_error))
			return std::nullopt;
		if (a_args.contains("moveMs") && !ReadInteger(a_args["moveMs"], "moveMs", 0, kMoveMsMax, request.moveMs, a_error))
			return std::nullopt;
		if (a_args.contains("holdMs") && !ReadInteger(a_args["holdMs"], "holdMs", ReachLogic::kHoldMsMin, ReachLogic::kHoldMsMax, request.holdMs, a_error))
			return std::nullopt;
		if (a_args.contains("toleranceUnits") && !ReadNumber(a_args["toleranceUnits"], "toleranceUnits", ReachLogic::kToleranceMin, ReachLogic::kToleranceMax, request.toleranceUnits, a_error))
			return std::nullopt;
		if (a_args.contains("corrections")) {
			long corrections = 0;
			if (!ReadInteger(a_args["corrections"], "corrections", 0, kCorrectionsMax, corrections, a_error))
				return std::nullopt;
			request.corrections = static_cast<int>(corrections);
		}
		return request;
	}

	/**
	 * A world target moved by a head offset (outward, forward, up) along the head's axes,
	 * outward being -right for the left hand and +right for the right one.
	 */
	Vec3 HeadOffsetWorld(const Vec3& a_world, const Vec3& a_headOffset, const HeadAxes& a_axes, Hand a_hand)
	{
		const double outward = Mirrored(a_hand, a_headOffset.x);
		return ReachLogic::Add(a_world, ReachLogic::Add(ReachLogic::Scale(a_axes.right, outward),
											ReachLogic::Add(ReachLogic::Scale(a_axes.forward, a_headOffset.y), ReachLogic::Scale(a_axes.up, a_headOffset.z))));
	}

	/**
	 * The tracking yaw that turns a controller a_yawInDeg toward the body: tracking yaw turns
	 * left when positive, so the left hand takes it negated.
	 */
	double YawInToTracking(double a_yawInDeg, Hand a_hand)
	{
		return Mirrored(a_hand, a_yawInDeg);
	}

	Vec3 ReadyHand(Hand a_hand, double a_eyeHeight)
	{
		const double w = Radians(kHandYawDeg);
		const double x = Mirrored(a_hand, kReadySide);
		const double z = -kReadyForward;
		return Vec3{ x * std::cos(w) + z * std::sin(w), a_eyeHeight - kReadyBelow, -x * std::sin(w) + z * std::cos(w) };
	}

	Pose Oriented(const Vec3& a_position, double a_pitchDeg, double a_yawDeg)
	{
		const double p = Radians(a_pitchDeg);
		const double w = Radians(a_yawDeg);
		const double cp = std::cos(p), sp = std::sin(p), cw = std::cos(w), sw = std::sin(w);
		// yaw(Y) * pitch(X)
		return Pose{
			cw, sw * sp, sw * cp, a_position.x,
			0.0, cp, -sp, a_position.y,
			-sw, cw * sp, cw * cp, a_position.z
		};
	}

	double EyeHeightFor(double a_cameraUnits, double a_roomLiftUnits, double a_unitsPerMeter)
	{
		if (!(a_unitsPerMeter > kEpsilon))
			return kEyeHeightMin;
		return std::clamp((a_cameraUnits - a_roomLiftUnits) / a_unitsPerMeter, kEyeHeightMin, kEyeHeightMax);
	}

	Vec3 GameAxes(const Vec3& a_tracking)
	{
		return Vec3{ a_tracking.x, -a_tracking.z, a_tracking.y };
	}

	Vec3 TrackingAxes(const Vec3& a_game)
	{
		return Vec3{ a_game.x, a_game.z, -a_game.y };
	}

	double FitTurn(const Vec3& a_hmdTracking, const Vec3& a_hmdWorld, const std::vector<HandSample>& a_hands)
	{
		double cross = 0.0;
		double dot = 0.0;
		for (const auto& hand : a_hands) {
			const Vec3 t = GameAxes(ReachLogic::Sub(hand.tracking, a_hmdTracking));
			const Vec3 w = ReachLogic::Sub(hand.wandWorld, a_hmdWorld);
			cross += t.x * w.y - t.y * w.x;
			dot += t.x * w.x + t.y * w.y;
		}
		if (std::abs(cross) < kEpsilon && std::abs(dot) < kEpsilon)
			return 0.0;
		return std::atan2(cross, dot);
	}

	Vec3 TrackingDelta(double a_theta, const Vec3& a_worldDelta, double a_unitsPerMeter)
	{
		const double c = std::cos(-a_theta), s = std::sin(-a_theta);
		const Vec3   d = ReachLogic::Scale(a_worldDelta, 1.0 / a_unitsPerMeter);
		return TrackingAxes(Vec3{ c * d.x - s * d.y, s * d.x + c * d.y, d.z });
	}

	Vec3 TrackingFor(double a_theta, const Vec3& a_hmdTracking, const Vec3& a_hmdWorld, const Vec3& a_world, double a_unitsPerMeter)
	{
		return ReachLogic::Add(a_hmdTracking, TrackingDelta(a_theta, ReachLogic::Sub(a_world, a_hmdWorld), a_unitsPerMeter));
	}

	Vec3 WorldFor(double a_theta, const Vec3& a_hmdTracking, const Vec3& a_hmdWorld, const Vec3& a_tracking, double a_unitsPerMeter)
	{
		const Vec3   g = ReachLogic::Scale(GameAxes(ReachLogic::Sub(a_tracking, a_hmdTracking)), a_unitsPerMeter);
		const double c = std::cos(a_theta), s = std::sin(a_theta);
		return ReachLogic::Add(a_hmdWorld, Vec3{ c * g.x - s * g.y, s * g.x + c * g.y, g.z });
	}

	/**
	 * Maps the eyes-to-target offset into tracking space, then reads the angles off Oriented's
	 * forward, (-sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch)).
	 */
	std::optional<HeadAngles> LookAngles(double a_theta, const Vec3& a_eyesWorld, const Vec3& a_target, double a_unitsPerMeter)
	{
		const Vec3 offset = ReachLogic::Sub(a_target, a_eyesWorld);
		if (ReachLogic::Distance(a_target, a_eyesWorld) < kLookAtMinUnits || !(a_unitsPerMeter > kEpsilon))
			return std::nullopt;
		const Vec3   d = TrackingDelta(a_theta, offset, a_unitsPerMeter);
		const double horizontal = std::sqrt(d.x * d.x + d.z * d.z);
		HeadAngles   angles;
		angles.pitchDeg = std::atan2(d.y, horizontal) * 180.0 / kPi;
		angles.yawDeg = horizontal > kEpsilon ? std::atan2(-d.x, -d.z) * 180.0 / kPi : 0.0;
		if (std::abs(angles.pitchDeg) > kHeadPitchLimitDeg) {
			angles.pitchDeg = std::clamp(angles.pitchDeg, -kHeadPitchLimitDeg, kHeadPitchLimitDeg);
			angles.clamped = true;
		}
		return angles;
	}

	json MoveFrames(const HeldPose& a_from, const HeldPose& a_to, long a_moveMs, long a_holdMs)
	{
		json frames = json::array();
		int  packet = 1;
		long tMs = 0;
		if (a_moveMs > 0) {
			const long steps = std::max(1L, a_moveMs / kMoveStepMs);
			for (long k = 0; k <= steps; ++k) {
				const double t = EaseInOut(static_cast<double>(k) / static_cast<double>(steps));
				HeldPose     pose = a_to;
				pose.headPitchDeg = a_from.headPitchDeg + (a_to.headPitchDeg - a_from.headPitchDeg) * t;
				pose.headYawDeg = a_from.headYawDeg + (a_to.headYawDeg - a_from.headYawDeg) * t;
				pose.left = Lerp(a_from.left, a_to.left, t);
				pose.right = Lerp(a_from.right, a_to.right, t);
				pose.leftAim = LerpAim(a_from.leftAim, a_to.leftAim, t);
				pose.rightAim = LerpAim(a_from.rightAim, a_to.rightAim, t);
				tMs = k * a_moveMs / steps;
				frames.push_back(FrameAt(tMs, packet++, pose));
			}
		} else {
			frames.push_back(FrameAt(0, packet++, a_to));
		}
		const long holdEnd = tMs + std::max(a_holdMs, kHoldStepMs);
		for (long t = tMs + kHoldStepMs; t <= holdEnd; t += kHoldStepMs)
			frames.push_back(FrameAt(t, packet++, a_to));
		return frames;
	}
}
