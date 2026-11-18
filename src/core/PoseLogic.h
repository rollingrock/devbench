#pragma once

#include "core/Hand.h"
#include "core/Json.h"
#include "core/ReachLogic.h"

#include <optional>
#include <string>
#include <vector>

// Pose: places the HMD and both controllers without calibrating. The game puts a Wand
// straight at its controller's pose, so a world point maps to tracking space through the
// HMD alone: world = hmdWorld + turn(theta) * unitsPerMeter * game(tracking - hmdTracking),
// where game() swaps OpenVR's axes (x right, y up, -z forward) into the world's (x right,
// y forward, z up) and theta, the turn between tracking space and the world, is fitted from
// where the held Wands land. The head stays at one eye position and only turns (pitch and
// yaw), so the HMD node does not move between placements.
namespace dvb::PoseLogic
{
	using ReachLogic::Pose;
	using ReachLogic::Vec3;

	/// How a controller points, in tracking degrees: pitch about X after yaw about Y (positive
	/// turns left), facing -z at zero. Over 90 pitches past vertical, pointing back.
	struct HandAim
	{
		double pitchDeg;
		double yawDeg;
	};

	/// Where one hand goes: the ready pose, a player node (plus front / offset), or a world point,
	/// with the controller pointing the ready way unless aim says otherwise.
	struct HandPlacement
	{
		enum class Kind
		{
			kReady,
			kNode,
			kWorld
		};
		Kind        kind{ Kind::kReady };
		std::string node;
		/// Which skeleton to search for node: "player" (body) or "firstPerson"; empty = the
		/// platform's default order.
		std::string root;
		double      front{ 0 };
		Vec3        offset;
		Vec3        world;
		/// A world target's offset along the head's axes: outward (toward this hand's side),
		/// forward, up, in world units.
		std::optional<Vec3>    headOffset;
		std::optional<HandAim> aim;
	};

	/// The head's axes in the world (the HMD node's rotation rows), each a unit vector.
	struct HeadAxes
	{
		Vec3 right;
		Vec3 forward;
		Vec3 up;
	};

	Vec3   HeadOffsetWorld(const Vec3& a_world, const Vec3& a_headOffset, const HeadAxes& a_axes, Hand a_hand);
	double YawInToTracking(double a_yawInDeg, Hand a_hand);

	/// What head.lookAt aims at: a held or placed hand, the midpoint of both, a player node, or a
	/// world point. It resolves once into head angles; the hold keeps the angles, not the target.
	struct HeadLook
	{
		enum class Kind
		{
			kLeft,
			kRight,
			kHands,
			kNode,
			kWorld
		};
		Kind        kind{ Kind::kHands };
		std::string node;
		/// As HandPlacement::root, for a node target.
		std::string root;
		Vec3        world;
	};

	struct PoseRequest
	{
		/// Absent: the hand stays where the held pose has it.
		std::optional<HandPlacement> left;
		std::optional<HandPlacement> right;
		/// Degrees, negative looks down; absent keeps the held head (the ready pitch on a fresh hold).
		std::optional<double> headPitch;
		/// Degrees in tracking space, positive turns left; absent keeps the held head.
		std::optional<double> headYaw;
		/// Exclusive with headPitch / headYaw, which it computes.
		std::optional<HeadLook> lookAt;
		long                    moveMs{ 300 };
		long                    holdMs{ 60000 };
		double                  toleranceUnits{ 1.0 };
		int                     corrections{ 2 };
		/// head.eyeHeight, tracking meters; overrides the eye height derived from cameraHeight.
		std::optional<double> eyeHeight;
		/// head.cameraHeight, world units above the player's feet the camera should sit at;
		/// default the platform's.
		std::optional<double> cameraHeight;
	};

	/// Parses the `pose` tool arguments; on failure fills a_error and returns nullopt.
	std::optional<PoseRequest> ParsePose(const json& a_args, std::string& a_error);

	/// The ready pose, chosen in the headset: the head pitched down, the hands in front at chest
	/// height and swung left so the desktop mirror (the left eye) shows them. Tracking meters
	/// relative to the eyes; the controllers point forward and a little down.
	inline constexpr double kReadyHeadPitchDeg = -22.0;
	/// The head preset "down": looking at the body, low enough to show the belt.
	inline constexpr double  kDownHeadPitchDeg = -75.0;
	inline constexpr double  kReadyForward = 0.60;
	inline constexpr double  kReadySide = 0.19;
	inline constexpr double  kReadyBelow = 0.40;
	inline constexpr double  kHandPitchDeg = 10.0;
	inline constexpr double  kHandYawDeg = 10.0;
	inline constexpr HandAim kReadyAim{ kHandPitchDeg, kHandYawDeg };
	inline constexpr double  kHandAimLimitDeg = 180.0;

	inline constexpr double kEyeHeightMin = 0.3;
	inline constexpr double kEyeHeightMax = 2.5;
	inline constexpr double kCameraHeightMax = 1000.0;
	/// The steepest head pitch, explicit or from lookAt: chosen in the headset, where the view
	/// and a body mod's neck look wrong closer to vertical (a held pose stays stable either way).
	inline constexpr double kHeadPitchLimitDeg = 85.0;
	inline constexpr double kHeadYawLimitDeg = 180.0;
	/// A lookAt target closer to the eyes than this (world units) has no direction to aim at.
	inline constexpr double kLookAtMinUnits = 1.0;
	/// A hand lookAt turns the head again once the hands land when its aim is off by more than
	/// this (degrees): the first turn moves a body mod's body, and a node-placed hand with it.
	inline constexpr double kLookAtReaimDeg = 0.5;
	inline constexpr double kWorldLimit = 1.0e7;
	inline constexpr long   kMoveMsMax = 5000;
	inline constexpr int    kCorrectionsMax = 5;
	/// Frame step of a move, and of the hold frames after it.
	inline constexpr long kMoveStepMs = 10;
	inline constexpr long kHoldStepMs = 1000;

	/// A hand's place in the ready pose, in tracking meters, for an eye height.
	Vec3 ReadyHand(Hand a_hand, double a_eyeHeight);

	/// A pose at a_position, pitched about X (negative looks down) after a yaw about Y (positive
	/// turns left); facing -z at zero.
	Pose Oriented(const Vec3& a_position, double a_pitchDeg, double a_yawDeg = 0.0);

	/// The eye height that puts the camera a_cameraUnits above the feet over a room node lifted
	/// a_roomLiftUnits above them (a body mod's seated / standing offset), clamped to a sane range.
	double EyeHeightFor(double a_cameraUnits, double a_roomLiftUnits, double a_unitsPerMeter);

	/// OpenVR tracking axes to the world's (x right, y forward, z up), and back.
	Vec3 GameAxes(const Vec3& a_tracking);
	Vec3 TrackingAxes(const Vec3& a_game);

	/// One held hand: where it was put in tracking space and where its Wand is in the world.
	struct HandSample
	{
		Vec3 tracking;
		Vec3 wandWorld;
	};

	/// The turn (radians) between tracking space and the world, fitted over the held hands'
	/// horizontal offsets from the head; 0 when no hand is offset from it.
	double FitTurn(const Vec3& a_hmdTracking, const Vec3& a_hmdWorld, const std::vector<HandSample>& a_hands);

	/// A world offset as a tracking-space offset, for a turn theta.
	Vec3 TrackingDelta(double a_theta, const Vec3& a_worldDelta, double a_unitsPerMeter);
	/// A world point as a tracking-space position, anchored on the head.
	Vec3 TrackingFor(double a_theta, const Vec3& a_hmdTracking, const Vec3& a_hmdWorld, const Vec3& a_world, double a_unitsPerMeter);
	/// A tracking-space position as a world point, anchored on the head.
	Vec3 WorldFor(double a_theta, const Vec3& a_hmdTracking, const Vec3& a_hmdWorld, const Vec3& a_tracking, double a_unitsPerMeter);

	/// Head angles aimed at a point; clamped is set when the pitch was clamped to the limit.
	struct HeadAngles
	{
		double pitchDeg{ 0 };
		double yawDeg{ 0 };
		bool   clamped{ false };
	};

	/// The head pitch and yaw (tracking space, as Oriented takes them) that aim the eyes at
	/// a_target, for a turn theta; nullopt when a_target is within kLookAtMinUnits of the eyes.
	/// The pitch is clamped to +-kHeadPitchLimitDeg.
	std::optional<HeadAngles> LookAngles(double a_theta, const Vec3& a_eyesWorld, const Vec3& a_target, double a_unitsPerMeter);

	/// The held state a move goes from and to.
	struct HeldPose
	{
		double  eyeHeight{ 1.65 };
		double  headPitchDeg{ kReadyHeadPitchDeg };
		Vec3    left;
		Vec3    right;
		HandAim leftAim{ kReadyAim };
		HandAim rightAim{ kReadyAim };
		double  headYawDeg{ 0 };
	};

	/// Frames easing from a_from to a_to over a_moveMs (none when a_moveMs is 0), then hold
	/// frames at a_to every kHoldStepMs for a_holdMs. The head stays at the eyes and only turns;
	/// the controllers turn with the move.
	json MoveFrames(const HeldPose& a_from, const HeldPose& a_to, long a_moveMs, long a_holdMs);
}
