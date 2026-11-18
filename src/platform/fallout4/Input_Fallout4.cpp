#include "Tools_Fallout4.h"

#include "core/ToolRegistry.h"
#include "core/VRInput.h"
#include "core/VRInputState.h"

#include <format>

// The `input` tool on Fallout 4: only device='vrTrackedSet', with the same sequence and press
// contract as Skyrim's input tool (core/VRInput.cpp handles both). Fallout places the hands
// with pose; Skyrim keeps its calibrated reach.
namespace dvb
{
	namespace
	{
		json HandleInput(const json& a_args, const ToolContext& a_ctx)
		{
			if (!a_args.is_object())
				throw ToolError(400, "input arguments must be an object");
			if (a_args.contains("action") && !a_args["action"].is_string())
				throw ToolError(400, "'action' must be a string");
			const std::string action = a_args.value("action", std::string("capabilities"));
			if (action == "capabilities")
				return json{ { "capabilities", json{ { "vrTrackedSet", VRInputCapabilities() } } } };
			if (a_args.contains("device") && !a_args["device"].is_string())
				throw ToolError(400, "'device' must be a string");
			const std::string device = a_args.value("device", std::string("vrTrackedSet"));
			if (!IsVRInputDevice(device))
				throw ToolError(400, std::format("device '{}' is not supported here; Fallout 4 supports device='vrTrackedSet'", device));
			if (action == "reach" || action == "reachStatus" || action == "reachStop")
				throw ToolError(400, "reach is Skyrim-only on devbench; use pose");
			return HandleVRInput(a_args, a_ctx);
		}
	}

	void RegisterInputTool(ToolRegistry& a_registry)
	{
		ToolDescriptor input;
		input.name = "input";
		input.description =
			"Synthetic VR input. action='capabilities' (default) returns the contract version, readiness, limits and "
			"supported actions; clients should check it rather than assume this tool exists. device='vrTrackedSet' "
			"(the only device on Fallout 4) replaces the HMD and both controllers at the OpenVR boundary. 'status' "
			"reports readiness, the active sequence and, per hooked OpenVR call, whether devbench sits closest to the "
			"runtime (when false, another mod hooked that call first and injected input bypasses it). 'observe' returns "
			"the current physical HMD and controllers as one frame. 'sequence' plays a prevalidated timestamped list of "
			"frames, each holding the HMD, left and right poses plus both controllers' OpenVR button/touch/axis state; it "
			"is asynchronous, has one owner, and returns a controlToken that 'stop' requires. A 'sequence' carrying the "
			"running sequence's controlToken replaces its frames in place, without handing the devices back to the "
			"physical controllers in between (same controller indices required). 'releaseAll' stops this "
			"owner's sequence. The real devices pass through whenever no sequence is active, and a sequence stops on "
			"load and new game. Emits input.vrTrackedSet events. "
			"Hands are named physically (left / right) or logically (primary / offhand, resolved live from the game's "
			"handedness) in press.hand, the pose hands keys and head.lookAt; answers stay keyed left / right and carry "
			"leftHanded, and two names for one controller answer 400. "
			"action='handedness' answers { leftHanded } (bLeftHandedMode:VR); with leftHanded it sets it and waits until the "
			"wands swap controllers, answering { leftHanded, was, changed, elapsedMs }. The swap only shows with the "
			"controllers held still and apart, so the check runs under this owner's pose hold, or a short ready-pose hold "
			"of its own when nothing holds the devices; another sequence answers 409, as do held hands under 10 units "
			"apart. A set that does not take within 2 s puts the old value back and answers 504 with the wand positions. "
			"action='press' (hand, buttons and / or stick, durationMs default 100, delayMs) holds named buttons on one controller "
			"for durationMs on top of the active sequence or pose hold, as its owner (409 otherwise); trigger and grip also "
			"drive their analog axis to 1.0, and the controller packet number moves on at press start and end. stick [x, y] "
			"(-1..1, y up) pushes that controller's thumbstick and lets it back to center when the press ends; the poses do not "
			"move. Each hand has its own press, so presses on both hands overlap; a new press on a hand replaces its last. It ends with "
			"the sequence. capabilities.vrTrackedSet.pressButtons lists the names. "
			"action='pose' places the HMD and controllers without calibrating and holds them: the game puts each wand "
			"(primaryWandNode / secondaryWandNode, mapped to physical left/right) straight at its controller, so a world "
			"point maps to tracking space through the HMD node and a turn fitted from where the held wands land. Skyrim's "
			"calibrated 'reach' is not on Fallout (400; use pose). The first pose takes over in the ready pose (head pitched -22 deg, hands 0.6 m "
			"in front at chest height, swung 10 deg left so the desktop mirror shows them), at the eye height that puts "
			"the camera head.cameraHeight units above the feet over the room node's lift (a body mod's seated / standing "
			"offset), so the body stands naturally. 'head' is 'ready' (pitch -22, yaw 0), 'down' (pitch -75, looking at "
			"the body) or {pitch?, yaw?, lookAt?, root?, eyeHeight?, cameraHeight?}: pitch (negative looks down) and yaw "
			"(positive turns left) in tracking degrees turn the head about the eyes; lookAt ('left' | 'right' | 'hands' | "
			"<node> with root? | [x,y,z]) computes them instead, aimed once, before anything moves, at a hand's target in "
			"this call (or its wand when the call leaves it), the hands' midpoint, a node or a world point (a hand target is "
			"aimed at again once the wands land, since the turn moves the body), and the hold keeps the angles; either height restarts the hold in the ready pose at that height. 'hands' is 'ready' (both) "
			"or {left?, right?} each 'ready', {node, root?, front?, offset?} (a node of the player: a VR node slot as the "
			"nodes tool names it, else that name in the first-person skeleton, else in the body; root 'player' or "
			"'firstPerson' searches one; front world units ahead along the facing, plus a world offset) or "
			"{world:[x,y,z]}, the last two with an optional "
			"controller pitch / yaw in degrees (the ready 10 / 10 otherwise; over 90 tilts it back past vertical); an absent hand stays. It "
			"moves over moveMs, corrects up to 'corrections' times while a wand is further than toleranceUnits from its "
			"target, holds holdMs (default 60 s), and answers once placed (about 0.5-1 s) with { controlToken, fresh, "
			"eyeHeight, headPitch, headYaw, lookAt? { target, clamped, reaimed? }, turnDeg (with a node or world target), "
			"corrections, hands { left, right: { tracking, pitch, yaw, wand, target?, error? } }, elapsedMs }. A later pose "
			"by the same owner moves from the held one in place (fresh: false); 'press' works over it; 'stop' with its "
			"controlToken hands the devices back. A pose while another sequence holds the devices, this owner's own "
			"included, answers 409. Fingers are not driven.";
		input.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "capabilities", "status", "observe", "sequence", "stop", "releaseAll", "press", "pose", "handedness" }) } } },
								{ "device", json{ { "type", "string" }, { "enum", json::array({ "vrTrackedSet" }) }, { "description", "the only device on Fallout 4 (default)" } } },
								{ "owner", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 }, { "description", "stable task/session owner; defaults to MCP session id or rest:anonymous" } } },
								{ "frames", json{ { "type", "array" }, { "minItems", 1 }, { "maxItems", kMaximumVRTrackedFrames }, { "description", "sequence: monotonic atomic frames; each requires tMs, originCode, hmd, left, right; controller objects carry packetNumber/pressed/touched/five axes" }, { "items", json{ { "type", "object" } } } } },
								{ "tailMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 1000 }, { "description", "sequence: final-state visibility before automatic release (default 50ms)" } } },
								{ "controlToken", json{ { "type", "string" }, { "description", "stop/releaseAll: opaque token returned by sequence start. sequence: the running sequence's token replaces its frames in place" } } },
								{ "hand", json{ { "type", "string" }, { "enum", json::array({ "left", "right", "primary", "offhand" }) }, { "description", "press: the controller, physical (left / right) or logical (primary / offhand, for the game's handedness)" } } },
								{ "leftHanded", json{ { "type", "boolean" }, { "description", "handedness: set the game's handedness (true: the left controller is the primary hand); absent reads it" } } },
								{ "buttons", json{ { "type", "array" }, { "minItems", 1 }, { "items", json{ { "type", "string" }, { "enum", json::array({ "trigger", "grip", "a", "b", "thumbstick" }) } } }, { "description", "press: buttons held together (trigger, grip, a = A/X, b = B/Y, thumbstick = click); optional with stick" } } },
								{ "stick", json{ { "type", "array" }, { "minItems", 2 }, { "maxItems", 2 }, { "items", json{ { "type", "number" }, { "minimum", -1 }, { "maximum", 1 } } }, { "description", "press: thumbstick deflection [x, y] held for durationMs (x right, y up), then back to center; touches the stick, does not click it" } } },
								{ "delayMs", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", 30000 }, { "description", "press: wait this long before pressing (default 0)" } } },
								{ "durationMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 10000 }, { "description", "press: how long the buttons / stick stay held (default 100)" } } },
								{ "hands", json{ { "oneOf", json::array({ json{ { "type", "string" }, { "enum", json::array({ "ready" }) } }, json{ { "type", "object" } } }) }, { "description", "pose: 'ready' for both hands, or {left?, right?} (or primary? / offhand?) each 'ready', {node, root?, front?, offset?:[x,y,z]} (that controller's wand at a node of the player, front world units in front of it along the facing, plus a world offset; root 'player' (body) or 'firstPerson' picks the skeleton the node is searched in) or {world:[x,y,z], headOffset?:[outward,forward,up]} (headOffset: world units along the HMD's axes as posed, outward toward that hand's own side, so one offset mirrors around the head); a node or world hand takes pitch / yaw (degrees, -180..180) for how the controller points, or yawIn instead of yaw (degrees, positive turns the controller toward the body for either hand); an absent hand stays where it is held" } } },
								{ "head", json{ { "oneOf", json::array({ json{ { "type", "string" }, { "enum", json::array({ "ready", "down" }) } }, json{ { "type", "object" } } }) }, { "description", "pose: 'ready' (pitch -22, yaw 0), 'down' (pitch -75, yaw 0) or {pitch?, yaw?, lookAt?, root?, eyeHeight?, cameraHeight?} - pitch in degrees (-85..85, negative looks down) and yaw (-180..180, positive turns left), absent keeps the held head; lookAt 'left' | 'right' | 'primary' | 'offhand' | 'hands' | <node> | [x,y,z] computes both instead (pitch clamped to 85), root 'player' or 'firstPerson' for a node; eyeHeight (0.3..2.5) the injected eye height in tracking meters, instead of the one cameraHeight gives; cameraHeight (1..1000) world units above the player's feet the camera should sit at (default 115, a natural FRIK body); either height starts a fresh hold" } } },
								{ "moveMs", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", 5000 }, { "description", "pose: how long a move takes, so it is seen (default 300)" } } },
								{ "corrections", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", 5 }, { "description", "pose: correction moves while a wand misses its node or world target (default 2)" } } },
								{ "holdMs", json{ { "type", "integer" }, { "minimum", 100 }, { "maximum", 1798000 }, { "description", "pose: how long to hold after the move (default 60000)" } } },
								{ "toleranceUnits", json{ { "type", "number" }, { "minimum", 0.05 }, { "maximum", 50 }, { "description", "pose: a wand this close to its target needs no correction (default 1)" } } },
							} },
		};
		a_registry.Register(std::move(input), &HandleInput);
	}
}
