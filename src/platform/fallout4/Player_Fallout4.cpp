#include "Tools_Fallout4.h"

#include "core/Json.h"
#include "core/MainThread.h"
#include "core/ToolRegistry.h"
#include "core/VRInput.h"
#include "core/tools/Memory.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <thread>
#include <vector>

// The `player` tool on Fallout 4: put the player in and out of a power armor frame and
// say whether they are in one, so a test can set up power armor without a headset.
//
// The state is the engine's own answer, PowerArmor::ActorInPowerArmor (REL::ID 1176757;
// the VR address library maps it to 0x9BF5D0, the RVA F4VR-CommonFramework calls too:
// Modding-Reference F4VR/Analysis/gold/F4VR-CommonFramework_RE_REFERENCE.md, Power Armor).
// The switch is the console's `player.cf "Actor.SwitchToPowerArmor" <frame>` / `none`,
// the path Immersive Flashlight's release check drives on FO4VR. Activating a frame from
// the console does not get the player in; stepIn / stepOut (below) use the controllers. The switch lands on a later frame, and the skeleton
// swap can hold the main thread, so the wait polls from the listener thread with short
// main-thread reads instead of blocking the main thread.
namespace dvb
{
	namespace
	{
		// the keyword on the furniture base of every power armor frame standing in the world
		constexpr std::string_view kFrameKeyword = "FurnitureTypePowerArmor";

		constexpr std::chrono::milliseconds kPollInterval{ 100 };
		constexpr std::chrono::milliseconds kPollTaskTimeout{ 2000 };
		constexpr std::int64_t              kDefaultTimeoutMs = 10000;
		constexpr std::int64_t              kMaxTimeoutMs = 60000;
		constexpr double                    kDefaultRadius = 500.0;
		constexpr double                    kMaxRadius = 20000.0;
		constexpr std::int64_t              kDefaultLimit = 5;
		constexpr std::int64_t              kMaxLimit = 50;
		constexpr std::size_t               kCommandMaxLines = 50;

		/**
		 * Main thread. The player, or a 409 when no save is loaded.
		 */
		RE::PlayerCharacter& RequirePlayer(std::string_view a_action)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetParentCell())
				throw ToolError(409, std::format("player {}: no player in the world (no save loaded yet)", a_action));
			return *player;
		}

		/**
		 * Main thread. The engine's own "is this actor in power armor".
		 */
		bool InPowerArmor(RE::PlayerCharacter& a_player)
		{
			return RE::PowerArmor::ActorInPowerArmor(a_player);
		}

		/**
		 * The furniture base of a reference when it is a power armor frame, else nullptr.
		 */
		RE::TESFurniture* FrameBase(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || !a_ref->data.objectReference)
				return nullptr;
			auto* furniture = a_ref->data.objectReference->As<RE::TESFurniture>();
			return furniture && furniture->HasKeywordString(kFrameKeyword) ? furniture : nullptr;
		}

		float DistanceTo(const RE::TESObjectREFR& a_from, const RE::TESObjectREFR& a_to)
		{
			const auto a = a_from.GetPosition();
			const auto b = a_to.GetPosition();
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}

		json FrameToJson(RE::TESObjectREFR& a_ref, RE::TESFurniture& a_base, float a_distance)
		{
			json out{
				{ "formId", std::format("0x{:08X}", a_ref.GetFormID()) },
				{ "baseFormId", std::format("0x{:08X}", a_base.GetFormID()) },
				{ "distance", std::round(static_cast<double>(a_distance) * 10.0) / 10.0 },
			};
			if (const auto name = RE::TESFullName::GetFullName(a_base); !name.empty())
				out["name"] = std::string{ name };
			return out;
		}

		struct Frame
		{
			RE::TESObjectREFR* ref;
			RE::TESFurniture*  base;
			float              distance;
		};

		/**
		 * Main thread. The power armor frames in the player's cell within a_radius game units,
		 * nearest first. Only the player's own cell: a frame just over an exterior cell border
		 * is not listed.
		 */
		std::vector<Frame> FindFrames(RE::PlayerCharacter& a_player, float a_radius)
		{
			std::vector<Frame> found;
			auto*              cell = a_player.GetParentCell();
			if (!cell)
				return found;
			for (const auto& refPtr : cell->references) {
				auto* ref = refPtr.get();
				if (!ref || ref->IsDeleted() || ref->IsDisabled())
					continue;
				auto* base = FrameBase(ref);
				if (!base)
					continue;
				const float distance = DistanceTo(a_player, *ref);
				if (distance <= a_radius)
					found.push_back({ ref, base, distance });
			}
			std::ranges::sort(found, {}, &Frame::distance);
			return found;
		}

		/**
		 * A form ID from a JSON string ("0x0001F2A3" or "1F2A3", hex) or integer.
		 */
		std::uint32_t ParseFormId(const json& a_value)
		{
			if (a_value.is_number_unsigned())
				return a_value.get<std::uint32_t>();
			if (!a_value.is_string())
				throw ToolError(400, "player: 'formId' must be a hex string such as \"0x0001F2A3\"");
			std::string text = a_value.get<std::string>();
			if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
				text.erase(0, 2);
			std::uint32_t id = 0;
			const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id, 16);
			if (text.empty() || ec != std::errc{} || end != text.data() + text.size())
				throw ToolError(400, std::format("player: 'formId' '{}' is not a hex form ID", a_value.get<std::string>()));
			return id;
		}

		float RadiusArg(const json& a_args)
		{
			return static_cast<float>(std::clamp(a_args.value("radius", kDefaultRadius), 1.0, kMaxRadius));
		}

		std::chrono::milliseconds TimeoutArg(const json& a_args)
		{
			return std::chrono::milliseconds(std::clamp(a_args.value("timeoutMs", kDefaultTimeoutMs), std::int64_t{ 0 }, kMaxTimeoutMs));
		}

		/**
		 * Main thread. The frame a_formId names (404 / 400 when it is no frame), else the
		 * nearest frame within a_radius of the player (404 when there is none).
		 */
		Frame ResolveFrame(RE::PlayerCharacter& a_player, std::optional<std::uint32_t> a_formId, float a_radius, std::string_view a_action)
		{
			if (a_formId) {
				auto* ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(*a_formId);
				if (!ref)
					throw ToolError(404, std::format("player {}: no reference 0x{:08X}", a_action, *a_formId));
				auto* base = FrameBase(ref);
				if (!base)
					throw ToolError(400, std::format("player {}: 0x{:08X} is not a power armor frame (its base has no {} keyword)", a_action, *a_formId, kFrameKeyword));
				return { ref, base, DistanceTo(a_player, *ref) };
			}
			const auto found = FindFrames(a_player, a_radius);
			if (found.empty())
				throw ToolError(404, std::format("player {}: no power armor frame within {} units in the player's cell; pass formId or a larger radius", a_action, a_radius));
			return found.front();
		}

		/**
		 * Polls the player's power armor state from the listener thread until it equals
		 * a_target or a_timeout passes. A poll the main thread does not serve in time (a
		 * skeleton swap or a load) counts as a stall and the wait goes on.
		 */
		json WaitForPowerArmor(bool a_target, std::chrono::milliseconds a_timeout)
		{
			const auto start = std::chrono::steady_clock::now();
			const auto deadline = start + a_timeout;
			bool       in = !a_target;
			int        stalls = 0;
			for (;;) {
				try {
					in = MainThread::RunAndWait([]() -> json { return InPowerArmor(RequirePlayer("wait")); }, kPollTaskTimeout).get<bool>();
				} catch (const MainThread::TaskTimeout&) {
					++stalls;
				}
				const auto now = std::chrono::steady_clock::now();
				if (in == a_target || now >= deadline)
					break;
				std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(kPollInterval, deadline - now));
			}
			const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
			json       out{ { "inPowerArmor", in }, { "waitedMs", waited.count() } };
			if (stalls)
				out["mainThreadStalls"] = stalls;
			return out;
		}

		json HandleStatus()
		{
			return MainThread::RunAndWait([]() -> json {
				return json{ { "inPowerArmor", InPowerArmor(RequirePlayer("status")) } };
			});
		}

		json HandleFind(const json& a_args)
		{
			const float radius = RadiusArg(a_args);
			const auto  limit = static_cast<std::size_t>(std::clamp(a_args.value("limit", kDefaultLimit), std::int64_t{ 1 }, kMaxLimit));
			return MainThread::RunAndWait([radius, limit]() -> json {
				auto&      player = RequirePlayer("findPowerArmor");
				const auto found = FindFrames(player, radius);
				json       frames = json::array();
				for (std::size_t i = 0; i < found.size() && i < limit; ++i)
					frames.push_back(FrameToJson(*found[i].ref, *found[i].base, found[i].distance));
				return json{ { "inPowerArmor", InPowerArmor(player) }, { "radius", radius }, { "frames", std::move(frames) } };
			});
		}

		/**
		 * enterPowerArmor / exitPowerArmor: switch on the main thread, then wait for the
		 * engine state from here. `outcome` is switched, already, refused (the state did not
		 * change within timeoutMs) or notWaited (wait=false).
		 */
		json HandleSwitch(const std::string& a_action, const json& a_args)
		{
			const bool                   enter = a_action == "enterPowerArmor";
			const bool                   wait = a_args.value("wait", true);
			const auto                   timeout = TimeoutArg(a_args);
			std::optional<std::uint32_t> formId;
			if (enter && a_args.contains("formId"))
				formId = ParseFormId(a_args["formId"]);
			const float radius = RadiusArg(a_args);

			json out = MainThread::RunAndWait([a_action, enter, formId, radius]() -> json {
				auto& player = RequirePlayer(a_action);
				if (InPowerArmor(player) == enter)
					return json{ { "inPowerArmor", enter }, { "outcome", "already" } };

				json        result = json::object();
				std::string target = "none";
				if (enter) {
					const Frame frame = ResolveFrame(player, formId, radius, a_action);
					result["frame"] = FrameToJson(*frame.ref, *frame.base, frame.distance);
					target = std::format("{:08X}", frame.ref->GetFormID());
				}
				const auto command = std::format("player.cf \"Actor.SwitchToPowerArmor\" {}", target);
				const auto console = RunConsoleCapture(command, kCommandMaxLines);
				result["command"] = command;
				for (const char* key : { "compiled", "lines" }) {
					if (console.contains(key))
						result[key] = console[key];
				}
				result["inPowerArmor"] = InPowerArmor(player);
				return result;
			});

			out["action"] = a_action;
			if (out.contains("outcome"))
				return out;
			if (!wait) {
				out["outcome"] = "notWaited";
				out["note"] = "inPowerArmor was read right after the command; the switch can land on a later frame, so read action='status' to see it";
				return out;
			}
			const json waited = WaitForPowerArmor(enter, timeout);
			out.update(waited);
			const bool switched = waited["inPowerArmor"].get<bool>() == enter;
			out["outcome"] = switched ? "switched" : "refused";
			if (!switched) {
				out["note"] = std::format(
					"the player is still {} power armor after {} ms. The switch was refused or has not finished: a mod can "
					"block it, `lines` holds what the command printed, and a Papyrus error goes to the Papyrus log",
					enter ? "out of" : "in", timeout.count());
			}
			return out;
		}

		// ---- stepIn / stepOut: the transition a player makes, through the controllers --------
		//
		// stepIn stands the player behind the frame, points the primary wand at it and taps A,
		// so the game runs its own activation, walk-in and animation. stepOut holds the
		// secondary hand's A/X button, the exit gesture measured in a setup with KabutoVR
		// loaded (a held A on the primary hand did nothing there). Both drive the VR input
		// tool's tracked-set contract as the owner kInputOwner and release it when done.
		//
		// The aim is checked, not assumed: the primary wand's ViewCaster keeps the reference its
		// activation ray is on. ViewCasterBase::QActivatePickRef (0x9D0CE0; Modding-Reference
		// F4VR/Analysis/silver/F4VRControlsConfig_RE_REFERENCE.md, with the caster pointer at
		// 0x5AC72B0) returns it through vtable slot 2, which, disassembled on 1.2.72, answers
		// the record at +0x90 while the byte at +0xD0 is set; the record starts with the
		// ObjectRefHandle. Measured live (2026-11-10): the handle at +0x90 turned non-zero once
		// the right wand pointed at a frame, and one A tap then took the player in.

		constexpr std::uintptr_t kPrimaryViewCasterHolder = 0x5AC72B0;
		constexpr std::uintptr_t kPrimaryPickSlotRVA = 0x9D5780;  // the primary caster's vtable slot 2
		constexpr std::uintptr_t kPickHandleOffset = 0x90;
		constexpr std::uintptr_t kPickValidOffset = 0xD0;

		constexpr const char*               kInputOwner = "devbench.player";
		constexpr double                    kBehindFrameUnits = 70.0;
		constexpr std::chrono::milliseconds kSettle{ 300 };
		constexpr std::int64_t              kRigHoldMs = 120000;
		constexpr std::int64_t              kActivateTapMs = 150;
		constexpr std::chrono::milliseconds kTapRetry{ 1500 };
		constexpr std::chrono::milliseconds kSettlePoll{ 100 };
		constexpr std::chrono::milliseconds kSettleLimit{ 2000 };
		constexpr std::int64_t              kExitHoldMs = 2000;
		constexpr double                    kPi = 3.14159265358979323846;

		double WrapAngle(double a_radians)
		{
			while (a_radians > kPi)
				a_radians -= 2.0 * kPi;
			while (a_radians < -kPi)
				a_radians += 2.0 * kPi;
			return a_radians;
		}

		/**
		 * Game yaw of the direction (a_dx, a_dy): 0 = +Y, increasing clockwise seen from above.
		 */
		double GameYaw(double a_dx, double a_dy)
		{
			return std::atan2(a_dx, a_dy);
		}

		double Degrees(double a_radians)
		{
			return std::round(a_radians * 1800.0 / kPi) / 10.0;
		}

		/**
		 * Main thread. The ObjectRefHandle value the primary wand's activation ray is on, 0 for
		 * nothing. The ViewCaster is identified by its vtable slot 2 before anything else is read,
		 * so a different runtime fails with a diagnostic instead of reading a wrong layout.
		 */
		std::uint32_t ReadPrimaryWandPick()
		{
			const auto     base = REL::Module::get().base();
			std::uintptr_t caster = 0;
			std::uintptr_t vtable = 0;
			std::uintptr_t slot2 = 0;
			if (!mem::SafeRead(reinterpret_cast<const void*>(base + kPrimaryViewCasterHolder), &caster, sizeof(caster)) || caster < 0x10000 ||
				!mem::SafeRead(reinterpret_cast<const void*>(caster), &vtable, sizeof(vtable)) ||
				!mem::SafeRead(reinterpret_cast<const void*>(vtable + 0x10), &slot2, sizeof(slot2)) || slot2 != base + kPrimaryPickSlotRVA)
				throw ToolError(500, std::format("player stepIn: the primary wand's ViewCaster at [base+0x{:X}] did not validate (caster 0x{:X}, "
												 "vtable slot 2 0x{:X}, expected base+0x{:X}); this runtime's layout is not the measured one",
										 kPrimaryViewCasterHolder, caster, slot2, kPrimaryPickSlotRVA));
			std::uint8_t  valid = 0;
			std::uint32_t handle = 0;
			if (!mem::SafeRead(reinterpret_cast<const void*>(caster + kPickValidOffset), &valid, sizeof(valid)) ||
				!mem::SafeRead(reinterpret_cast<const void*>(caster + kPickHandleOffset), &handle, sizeof(handle)))
				throw ToolError(500, "player stepIn: the primary wand's pick record could not be read");
			return valid ? handle : 0;
		}

		/**
		 * Main thread. The reference an ObjectRefHandle value names, as "0x<formId>", or null.
		 */
		json HandleToJson(std::uint32_t a_handle)
		{
			if (!a_handle)
				return nullptr;
			RE::ObjectRefHandle handle;
			static_assert(sizeof(handle) == sizeof(a_handle));
			std::memcpy(&handle, &a_handle, sizeof(a_handle));
			const auto ref = handle.get();
			return ref ? json(std::format("0x{:08X}", ref->GetFormID())) : json(std::format("handle 0x{:X}", a_handle));
		}

		struct DeviceState
		{
			std::uint32_t index;
			std::uint64_t packet;
		};

		/**
		 * One tracked device's pose: rotated by a_yaw about tracking +Y, at a_pos rotated the
		 * same way, so the whole rig turns about the tracking origin.
		 */
		json RigPose(std::uint32_t a_index, double a_yaw, double a_x, double a_y, double a_z)
		{
			const double c = std::cos(a_yaw);
			const double s = std::sin(a_yaw);
			return json{
				{ "index", a_index },
				{ "available", true },
				{ "connected", true },
				{ "valid", true },
				{ "trackingResult", 200 },
				{ "matrix", json::array({ c, 0.0, s, c * a_x + s * a_z, 0.0, 1.0, 0.0, a_y, -s, 0.0, c, -s * a_x + c * a_z }) },
				{ "velocity", json::array({ 0.0, 0.0, 0.0 }) },
				{ "angularVelocity", json::array({ 0.0, 0.0, 0.0 }) },
			};
		}

		json RigController(std::uint64_t a_packet)
		{
			const json zero = json::array({ 0.0, 0.0 });
			return json{ { "packetNumber", a_packet }, { "pressed", 0 }, { "touched", 0 }, { "axes", json::array({ zero, zero, zero, zero, zero }) } };
		}

		/**
		 * The injected standing pose for stepIn / stepOut, owned for the lifetime of this object:
		 * the HMD at eye height and both controllers held in front, the right one straight ahead
		 * of the head so its ray passes over the tracking origin. Turn() swaps the whole rig's
		 * yaw in place; the destructor hands the devices back.
		 */
		class ControllerRig
		{
		public:
			/**
			 * Takes the device indices and packet numbers from the physical tracked set. A device
			 * that reports no index (a controller switched off or asleep) gets the lowest index
			 * nobody else uses: the game's controllers are pointed at whatever index is injected.
			 */
			explicit ControllerRig(const ToolContext& a_ctx) :
				_ctx(a_ctx)
			{
				const json  observed = HandleVRInput(json{ { "action", "observe" }, { "device", "vrTrackedSet" } }, _ctx);
				const auto& frame = observed.at("frame");
				auto        indexOf = [&](const char* a_device) -> std::optional<std::uint32_t> {
					const auto& device = frame.at(a_device);
					if (device.contains("index") && device["index"].is_number_unsigned())
						return device["index"].get<std::uint32_t>();
					return std::nullopt;
				};
				const auto                 hmd = indexOf("hmd");
				const auto                 left = indexOf("left");
				const auto                 right = indexOf("right");
				std::vector<std::uint32_t> used;
				for (const auto& index : { hmd, left, right }) {
					if (index)
						used.push_back(*index);
				}
				auto pick = [&](std::optional<std::uint32_t> a_index) {
					if (a_index)
						return *a_index;
					std::uint32_t free = 0;
					while (std::ranges::find(used, free) != used.end())
						++free;
					used.push_back(free);
					return free;
				};
				auto packetOf = [&](const char* a_device) {
					return frame.at(a_device).at("controller").value("packetNumber", std::uint64_t{ 0 });
				};
				_hmd = pick(hmd);
				_right = { pick(right), packetOf("right") };
				_left = { pick(left), packetOf("left") };
			}

			~ControllerRig()
			{
				if (_token.empty())
					return;
				try {
					HandleVRInput(json{ { "action", "stop" }, { "device", "vrTrackedSet" }, { "owner", kInputOwner }, { "controlToken", _token } }, _ctx);
				} catch (...) {
					// the sequence may already have ended; nothing is left to hand back
				}
			}

			ControllerRig(const ControllerRig&) = delete;
			ControllerRig& operator=(const ControllerRig&) = delete;

			/**
			 * Holds the rig at a_yaw (radians about tracking +Y) and lets it settle.
			 */
			void Turn(double a_yaw)
			{
				json frames = json::array({ PoseFrame(0, a_yaw), PoseFrame(kRigHoldMs, a_yaw) });
				json args{ { "action", "sequence" }, { "device", "vrTrackedSet" }, { "owner", kInputOwner }, { "frames", std::move(frames) } };
				if (!_token.empty())
					args["controlToken"] = _token;
				_token = HandleVRInput(args, _ctx).at("controlToken").get<std::string>();
				std::this_thread::sleep_for(kSettle);
			}

			/**
			 * Holds the A/X button of one physical controller for a_ms, over the held pose.
			 */
			void PressA(Hand a_hand, std::int64_t a_ms)
			{
				HandleVRInput(json{ { "action", "press" }, { "device", "vrTrackedSet" }, { "owner", kInputOwner }, { "hand", HandName(a_hand) },
								  { "buttons", json::array({ "a" }) }, { "durationMs", a_ms } },
					_ctx);
			}

		private:
			json PoseFrame(std::int64_t a_tMs, double a_yaw)
			{
				json left = RigPose(_left.index, a_yaw, -0.25, 1.0, -0.15);
				left["controller"] = RigController(++_left.packet);
				json right = RigPose(_right.index, a_yaw, 0.0, 1.3, -0.35);
				right["controller"] = RigController(++_right.packet);
				return json{ { "tMs", a_tMs }, { "originCode", 1 }, { "hmd", RigPose(_hmd, a_yaw, 0.0, 1.65, 0.0) }, { "left", std::move(left) }, { "right", std::move(right) } };
			}

			const ToolContext& _ctx;
			std::uint32_t      _hmd = 0;
			DeviceState        _left{};
			DeviceState        _right{};
			std::string        _token;
		};

		/**
		 * What one aim check saw: the primary wand's world yaw and position, and its pick.
		 */
		struct AimRead
		{
			double        wandYaw;
			double        wandX;
			double        wandY;
			std::uint32_t pick;
		};

		AimRead ReadAim(Hand a_primary)
		{
			const json r = MainThread::RunAndWait([a_primary]() -> json {
				auto* wand = WandNode(a_primary);
				// The activation ray runs along the wand's local +Y, which in the world is the
				// rotation's second ROW (entry[1][0], entry[1][1]). The second column is the same
				// direction mirrored across world Y: it agreed only for frames lying along +Y and
				// sent the aim the wrong way for a frame at 231 degrees (measured live, 2026-11-10).
				const auto& m = wand->world.rotate;
				return json{ { "yaw", GameYaw(m.entry[1][0], m.entry[1][1]) }, { "x", wand->world.translate.x }, { "y", wand->world.translate.y },
					{ "pick", ReadPrimaryWandPick() } };
			});
			return { r["yaw"].get<double>(), r["x"].get<double>(), r["y"].get<double>(), r["pick"].get<std::uint32_t>() };
		}

		/**
		 * Reads the aim until two reads kSettlePoll apart agree, up to kSettleLimit. The first
		 * injection after a load takes longer than kSettle to reach the wand (measured live: one
		 * read after it came back 38 degrees short of the turn).
		 */
		AimRead ReadSettledAim(Hand a_primary)
		{
			AimRead    last = ReadAim(a_primary);
			const auto until = std::chrono::steady_clock::now() + kSettleLimit;
			while (std::chrono::steady_clock::now() < until) {
				std::this_thread::sleep_for(kSettlePoll);
				const AimRead next = ReadAim(a_primary);
				const bool    still = std::abs(WrapAngle(next.wandYaw - last.wandYaw)) < 0.01 &&
				                      std::abs(next.wandX - last.wandX) < 0.5 && std::abs(next.wandY - last.wandY) < 0.5;
				last = next;
				if (still)
					break;
			}
			return last;
		}

		/**
		 * Turns the rig until the primary wand's activation ray is on the frame. It learns which
		 * way a tracking-space turn moves the wand in the world from one 90-degree turn, steers
		 * the wand at the frame for up to three passes, then sweeps up to 30 degrees either side.
		 * Returns { matched, yawDeg, passes:[{ yawDeg, wandYawDeg, wandErrorDeg, pick }] }.
		 */
		json AimAtFrame(ControllerRig& a_rig, Hand a_primary, std::uint32_t a_frameHandle, double a_frameX, double a_frameY)
		{
			json passes = json::array();
			auto record = [&](double a_yaw, const AimRead& a_read) {
				const double error = WrapAngle(GameYaw(a_frameX - a_read.wandX, a_frameY - a_read.wandY) - a_read.wandYaw);
				json         pick = MainThread::RunAndWait([handle = a_read.pick]() -> json { return HandleToJson(handle); });
				passes.push_back(json{ { "yawDeg", Degrees(a_yaw) }, { "wandYawDeg", Degrees(a_read.wandYaw) }, { "wandErrorDeg", Degrees(error) }, { "pick", std::move(pick) } });
				return error;
			};

			a_rig.Turn(0.0);
			const AimRead start = ReadSettledAim(a_primary);
			record(0.0, start);
			if (start.pick == a_frameHandle)
				return json{ { "matched", true }, { "yawDeg", 0.0 }, { "passes", std::move(passes) } };
			a_rig.Turn(kPi / 2.0);
			AimRead      read = ReadSettledAim(a_primary);
			const double turned = WrapAngle(read.wandYaw - start.wandYaw);
			if (std::abs(std::abs(turned) - kPi / 2.0) > 0.5)
				throw ToolError(500, std::format("player stepIn: a 90-degree turn of the injected controllers turned the wand {} degrees; the wand "
												 "is not following the injected pose (is another mod moving it?)",
										 Degrees(turned)));
			// +1 when a positive tracking yaw turns the wand clockwise in the world
			const double sign = turned > 0.0 ? 1.0 : -1.0;
			double       yaw = kPi / 2.0;
			double       error = record(yaw, read);

			for (int pass = 0; pass < 3 && read.pick != a_frameHandle; ++pass) {
				yaw = WrapAngle(yaw + sign * error);
				a_rig.Turn(yaw);
				read = ReadSettledAim(a_primary);
				error = record(yaw, read);
			}
			const double aimed = yaw;
			for (const double offsetDeg : { 10.0, -10.0, 20.0, -20.0, 30.0, -30.0 }) {
				if (read.pick == a_frameHandle)
					break;
				yaw = WrapAngle(aimed + offsetDeg * kPi / 180.0);
				a_rig.Turn(yaw);
				read = ReadSettledAim(a_primary);
				record(yaw, read);
			}
			return json{ { "matched", read.pick == a_frameHandle }, { "yawDeg", Degrees(yaw) }, { "passes", std::move(passes) } };
		}

		/**
		 * stepIn: stand behind the frame, aim the primary wand at it, tap A, wait for the state.
		 */
		json HandleStepIn(const json& a_args, const ToolContext& a_ctx)
		{
			if (!REL::Module::IsVR())
				throw ToolError(501, "player stepIn: drives the VR controllers, so it runs on Fallout 4 VR only; use enterPowerArmor");
			std::optional<std::uint32_t> formId;
			if (a_args.contains("formId"))
				formId = ParseFormId(a_args["formId"]);
			const float radius = RadiusArg(a_args);
			const auto  timeout = TimeoutArg(a_args);
			// read the tracked set before anything moves, so a failure here leaves the player be
			ControllerRig rig(a_ctx);

			// Stand the player kBehindFrameUnits behind the frame, facing the way it faces: the
			// frame opens at the back, which is where a player walks in from.
			json placed = MainThread::RunAndWait([formId, radius]() -> json {
				auto& player = RequirePlayer("stepIn");
				if (InPowerArmor(player))
					return json{ { "outcome", "already" }, { "inPowerArmor", true } };
				const Frame  frame = ResolveFrame(player, formId, radius, "stepIn");
				const auto   pos = frame.ref->GetPosition();
				const double facing = frame.ref->data.angle.z;
				const double x = pos.x - std::sin(facing) * kBehindFrameUnits;
				const double y = pos.y - std::cos(facing) * kBehindFrameUnits;
				const auto   command = std::format("player.setpos x {:.2f}; player.setpos y {:.2f}; player.setpos z {:.2f}; player.setangle z {:.2f}",
					x, y, pos.z, Degrees(facing));
				const json   console = RunConsoleCapture(command, kCommandMaxLines);
				if (!console.value("compiled", false))
					throw ToolError(500, std::format("player stepIn: placing the player did not compile: {}", console.value("lines", json::array()).dump()));
				return json{
					{ "frame", FrameToJson(*frame.ref, *frame.base, frame.distance) },
					{ "frameHandle", frame.ref->GetHandle().native_handle() },
					{ "frameX", pos.x },
					{ "frameY", pos.y },
					{ "leftHanded", LeftHandedMode() },
				};
			});
			json out{ { "action", "stepIn" } };
			if (placed.contains("outcome")) {
				out.update(placed);
				return out;
			}
			out["frame"] = placed["frame"];

			const Hand primary = HandFor(Role::Primary, placed["leftHanded"].get<bool>());
			json       aim = AimAtFrame(rig, primary, placed["frameHandle"].get<std::uint32_t>(), placed["frameX"].get<double>(), placed["frameY"].get<double>());
			const bool aimed = aim["matched"].get<bool>();
			out["aim"] = std::move(aim);
			if (!aimed) {
				out["outcome"] = "notAimed";
				out["inPowerArmor"] = false;
				out["note"] = "the primary wand's activation ray never landed on the frame, so A was not pressed; aim.passes shows where it pointed and what it picked";
				return out;
			}
			// A tap with the wand on the frame was lost twice in live runs (no walk-in, no state
			// change), both right after a stepOut; later immediate stepOut / stepIn cycles worked
			// on the first tap, so the cause is unknown. A tap that works flips the state within
			// about 250 ms, so one with no flip after kTapRetry is tapped again while the wand is
			// still on the frame.
			const auto frameHandle = placed["frameHandle"].get<std::uint32_t>();
			const auto start = std::chrono::steady_clock::now();
			const auto deadline = start + timeout;
			int        taps = 0;
			int        stalls = 0;
			bool       in = false;
			for (;;) {
				rig.PressA(primary, kActivateTapMs);
				++taps;
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
				const json waited = WaitForPowerArmor(true, std::clamp(remaining, std::chrono::milliseconds(0), kTapRetry));
				in = waited["inPowerArmor"].get<bool>();
				stalls += waited.value("mainThreadStalls", 0);
				if (in || std::chrono::steady_clock::now() >= deadline)
					break;
				if (ReadAim(primary).pick != frameHandle) {
					out["note"] = "the wand left the frame between taps (the player moved?), so it was not tapped again";
					break;
				}
			}
			out["inPowerArmor"] = in;
			out["taps"] = taps;
			out["waitedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			if (stalls)
				out["mainThreadStalls"] = stalls;
			out["outcome"] = in ? "switched" : "refused";
			if (!in && !out.contains("note"))
				out["note"] = std::format("the wand was on the frame and A was tapped {} times, but the player was not in power armor after {} ms", taps, timeout.count());
			return out;
		}

		/**
		 * stepOut: hold the secondary hand's A/X button and wait for the state.
		 */
		json HandleStepOut(const json& a_args, const ToolContext& a_ctx)
		{
			if (!REL::Module::IsVR())
				throw ToolError(501, "player stepOut: drives the VR controllers, so it runs on Fallout 4 VR only; use exitPowerArmor");
			const auto timeout = TimeoutArg(a_args);
			const json state = MainThread::RunAndWait([]() -> json {
				auto& player = RequirePlayer("stepOut");
				return json{ { "inPowerArmor", InPowerArmor(player) }, { "leftHanded", LeftHandedMode() } };
			});
			json       out{ { "action", "stepOut" } };
			if (!state["inPowerArmor"].get<bool>()) {
				out["outcome"] = "already";
				out["inPowerArmor"] = false;
				return out;
			}
			ControllerRig rig(a_ctx);
			rig.Turn(0.0);
			rig.PressA(HandFor(Role::Offhand, state["leftHanded"].get<bool>()), kExitHoldMs);
			out.update(WaitForPowerArmor(false, std::chrono::milliseconds(kExitHoldMs) + timeout));
			const bool in = out["inPowerArmor"].get<bool>();
			out["outcome"] = in ? "refused" : "switched";
			if (in)
				out["note"] = std::format("the secondary hand's A/X was held {} ms and the player was still in power armor; the exit gesture may differ in this setup", kExitHoldMs);
			return out;
		}

		// ---- weapon / drawWeapon / sheatheWeapon ----------------------------------------------
		//
		// The switch is the engine's own draw / sheathe call, Actor::DrawWeaponMagicHands(bool)
		// (vtable slot 0xC9; on FO4VR 1.2.72 the player's slot is 0xF78D10, the function
		// F4VRControlsConfig and ROCK use: Modding-Reference F4VR/Analysis). It plays ActionDraw
		// / ActionSheath on the actor and, when the graph accepts, moves the weapon state to
		// drawing / sheathing in the same call; the animation then ends it at drawn / sheathed.
		// With no weapon equipped the same call sheathes drawn fists.
		//
		// The state is ActorState's weapon state. On FO4VR it is not where CommonLibF4 declares
		// it: 0xF78D10 reads it as (dword at ActorState+0x0C >> 2) & 7, one bit above the
		// header's 0C:01, so the VR read takes the bits itself. With the header's bits a settled
		// state still answers GetWeaponMagicDrawn() correctly, but the six states do not decode.
		//
		// The equipped weapon is the Papyrus native Actor.GetEquippedWeapon (VR 0x140D790, the
		// call F4VR-CommonFramework makes): it ignores its VM and stack arguments and answers
		// the object in equip index 0 only when that is a weapon. VR's MiddleHighProcessData
		// keeps equippedItems at +0x290, not the header's +0x288, so the array is not read here.

		constexpr std::uintptr_t kActorStateFlagsOffset = 0x0C;
		constexpr std::uint32_t  kVRWeaponStateShift = 2;
		constexpr std::uint32_t  kWeaponSheathed = 0;
		constexpr std::uint32_t  kWeaponDrawn = 3;
		constexpr std::uintptr_t kVRGetEquippedWeaponRVA = 0x140D790;
		// push rbx; sub rsp,30h; mov rcx,r8: the native's first bytes on 1.2.72
		constexpr std::uint8_t kVRGetEquippedWeaponProlog[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xC8 };

		/**
		 * Main thread. The player's RE::WEAPON_STATE value, from the bits the running game uses.
		 */
		std::uint32_t ReadWeaponState(RE::PlayerCharacter& a_player)
		{
			const RE::ActorState& state = a_player;
			if (!REL::Module::IsVR())
				return static_cast<std::uint32_t>(state.weaponState);
			std::uint32_t flags = 0;
			std::memcpy(&flags, reinterpret_cast<const std::uint8_t*>(&state) + kActorStateFlagsOffset, sizeof(flags));
			return (flags >> kVRWeaponStateShift) & 7;
		}

		std::string WeaponStateName(std::uint32_t a_state)
		{
			constexpr const char* kNames[] = { "sheathed", "wantToDraw", "drawing", "drawn", "wantToSheathe", "sheathing" };
			return a_state < std::size(kNames) ? kNames[a_state] : std::format("unknown({})", a_state);
		}

		/**
		 * Main thread, VR. The weapon in the player's equip index 0 as { name, formId }, null for
		 * none (fists). The native is checked by its first bytes, so another runtime fails with
		 * a diagnostic instead of calling into the wrong function.
		 */
		json EquippedWeaponJson(RE::PlayerCharacter& a_player)
		{
			const auto   address = REL::Module::get().base() + kVRGetEquippedWeaponRVA;
			std::uint8_t prolog[sizeof(kVRGetEquippedWeaponProlog)]{};
			if (!mem::SafeRead(reinterpret_cast<const void*>(address), prolog, sizeof(prolog)) ||
				std::memcmp(prolog, kVRGetEquippedWeaponProlog, sizeof(prolog)) != 0)
				throw ToolError(500, std::format("player weapon: Actor.GetEquippedWeapon at base+0x{:X} did not validate; this runtime is not the measured one", kVRGetEquippedWeaponRVA));
			using GetEquippedWeapon_t = RE::TESObjectWEAP* (*)(void*, std::uint64_t, RE::Actor*, std::uint64_t);
			auto* weapon = reinterpret_cast<GetEquippedWeapon_t>(address)(nullptr, 0, &a_player, 0);
			if (!weapon)
				return nullptr;
			return json{ { "name", std::string{ RE::TESFullName::GetFullName(*weapon) } }, { "formId", std::format("0x{:08X}", weapon->GetFormID()) } };
		}

		/**
		 * Main thread. { drawn, weaponState, equipped }: drawn is weaponState >= drawn, what
		 * GetWeaponMagicDrawn() means. `equipped` is VR only; flat has no measured read.
		 */
		json WeaponJson(RE::PlayerCharacter& a_player)
		{
			const auto state = ReadWeaponState(a_player);
			json       out{ { "drawn", state >= kWeaponDrawn }, { "weaponState", WeaponStateName(state) } };
			if (REL::Module::IsVR())
				out["equipped"] = EquippedWeaponJson(a_player);
			return out;
		}

		json HandleWeaponStatus()
		{
			return MainThread::RunAndWait([]() -> json { return WeaponJson(RequirePlayer("weapon")); });
		}

		// How long after a call that left the weapon at rest it is made again.
		constexpr std::chrono::milliseconds kWeaponCallRetry{ 250 };

		/**
		 * drawWeapon / sheatheWeapon: the engine's draw / sheathe call on the main thread, then
		 * short polls from the listener thread until the state settles at drawn / sheathed or
		 * timeoutMs passes. The graph ignores a call made just after the previous switch settled
		 * (the state stays at rest), so while the state is still at the starting rest the call is
		 * made again every kWeaponCallRetry. `outcome` is switched, already, refused or notWaited.
		 */
		json HandleWeaponSwitch(const std::string& a_action, const json& a_args)
		{
			const bool        draw = a_action == "drawWeapon";
			const bool        wait = a_args.value("wait", true);
			const auto        timeout = TimeoutArg(a_args);
			const std::string target = WeaponStateName(draw ? kWeaponDrawn : kWeaponSheathed);
			const std::string atRest = WeaponStateName(draw ? kWeaponSheathed : kWeaponDrawn);

			const auto call = [a_action, draw]() {
				return MainThread::RunAndWait([a_action, draw]() -> json {
					auto& player = RequirePlayer(a_action);
					if (ReadWeaponState(player) == (draw ? kWeaponDrawn : kWeaponSheathed)) {
						json already = WeaponJson(player);
						already["outcome"] = "already";
						return already;
					}
					player.DrawWeaponMagicHands(draw);
					return WeaponJson(player);
				});
			};
			json out = call();
			out["action"] = a_action;
			if (out.contains("outcome"))
				return out;
			if (!wait) {
				out["outcome"] = "notWaited";
				out["note"] = "weaponState was read right after the call; the animation ends it later, so read action='weapon' to see it settle";
				return out;
			}

			const auto start = std::chrono::steady_clock::now();
			const auto deadline = start + timeout;
			auto       lastCall = start;
			int        calls = 1;
			int        stalls = 0;
			while (out["weaponState"] != target) {
				const auto now = std::chrono::steady_clock::now();
				if (now >= deadline)
					break;
				if (out["weaponState"] == atRest && now - lastCall >= kWeaponCallRetry) {
					json again = call();
					again.erase("outcome");  // "already": it settled between the poll and the call
					out.update(again);
					lastCall = std::chrono::steady_clock::now();
					++calls;
					continue;
				}
				std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(kPollInterval, deadline - now));
				try {
					out.update(MainThread::RunAndWait([]() -> json { return WeaponJson(RequirePlayer("wait")); }, kPollTaskTimeout));
				} catch (const MainThread::TaskTimeout&) {
					++stalls;
				}
			}
			out["waitedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			if (calls > 1)
				out["calls"] = calls;
			if (stalls)
				out["mainThreadStalls"] = stalls;
			const bool switched = out["weaponState"] == target;
			out["outcome"] = switched ? "switched" : "refused";
			if (!switched) {
				out["note"] = std::format(
					"the weapon state is {} after {} ms, not {}. The engine did not take the {} (an animation in progress, a "
					"menu, a mod) or the animation has not finished",
					out["weaponState"].get<std::string>(), timeout.count(), target, draw ? "draw" : "sheathe");
			}
			return out;
		}

		json HandlePlayer(const json& a_args, const ToolContext& a_ctx)
		{
			if (!a_args.is_object())
				throw ToolError(400, "player arguments must be an object");
			const std::string action = a_args.value("action", std::string("status"));
			if (action == "status")
				return HandleStatus();
			if (action == "findPowerArmor")
				return HandleFind(a_args);
			if (action == "enterPowerArmor" || action == "exitPowerArmor")
				return HandleSwitch(action, a_args);
			if (action == "stepIn")
				return HandleStepIn(a_args, a_ctx);
			if (action == "stepOut")
				return HandleStepOut(a_args, a_ctx);
			if (action == "weapon")
				return HandleWeaponStatus();
			if (action == "drawWeapon" || action == "sheatheWeapon")
				return HandleWeaponSwitch(action, a_args);
			throw ToolError(400, std::format("player: unknown action '{}' (status|findPowerArmor|enterPowerArmor|exitPowerArmor|stepIn|stepOut|weapon|drawWeapon|sheatheWeapon)", action));
		}
	}

	void RegisterPlayerTool(ToolRegistry& a_registry)
	{
		ToolDescriptor player;
		player.name = "player";
		player.description =
			"The player's power armor and weapon. action='status' (default) → { inPowerArmor }, the engine's own answer "
			"(PowerArmor::ActorInPowerArmor). 'findPowerArmor' (radius default 500 units, limit default 5) → { inPowerArmor, "
			"radius, frames:[{ formId, baseFormId, name, distance }] }, the power armor frames (furniture with the "
			"FurnitureTypePowerArmor keyword) in the player's cell, nearest first; a frame across an exterior cell border is "
			"not listed. 'enterPowerArmor' (formId, default the nearest frame within radius) and 'exitPowerArmor' switch at "
			"once, without the step-in animation, through the console's player.cf \"Actor.SwitchToPowerArmor\" <frame> / "
			"none, then wait (wait default true) until the engine state flips or timeoutMs (default 10000, max 60000) "
			"passes, polling short main-thread reads so the skeleton swap is never blocked. Returns { action, frame?, "
			"command, compiled, lines, inPowerArmor, waitedMs, mainThreadStalls?, outcome, note? }; outcome is 'switched', "
			"'already' (nothing was run), 'refused' (the state did not change in time; a mod can block it) or 'notWaited'. "
			"Those two are instant setup, not the transition a player makes. 'stepIn' and 'stepOut' (VR only) make that "
			"transition through the controllers, so the game runs its own activation, walk-in and animations: stepIn "
			"(formId / radius as for enterPowerArmor) stands the player 70 units behind the frame facing it, injects a "
			"standing pose through the input tool (owner devbench.player; 409 while another owner holds it), turns it until "
			"the primary wand's activation ray is on the frame (read from the wand's ViewCaster), taps A on the primary hand "
			"and waits, tapping again every 1.5 s while the state has not flipped (a tap was "
			"lost twice in live runs, cause unknown); stepOut holds the secondary hand's A/X button for 2 s and waits. Both hand the controllers back when "
			"done. They return { action, frame?, aim?: { matched, yawDeg, passes:[{ yawDeg, wandYawDeg, wandErrorDeg, pick }] }, "
			"inPowerArmor, taps?, waitedMs, outcome, note? }, outcome as above plus 'notAimed' (the ray never landed on the frame, "
			"so nothing was pressed). The exit gesture was measured with KabutoVR loaded. 'weapon' → { drawn, weaponState, "
			"equipped }: weaponState is sheathed | wantToDraw | drawing | drawn | wantToSheathe | sheathing, drawn is weaponState "
			">= drawn (what GetWeaponMagicDrawn means, so true for drawn fists), equipped is the weapon in hand as { name, formId } "
			"or null for none (fists); equipped is VR only. 'drawWeapon' and 'sheatheWeapon' make the engine's own draw / sheathe "
			"call (Actor::DrawWeaponMagicHands), then wait (wait default true) until weaponState settles at drawn / sheathed or "
			"timeoutMs passes. The engine ignores a call made just after the previous switch settled, so while the weapon is still "
			"at rest the call is made again every 250 ms (`calls` counts them when more than one). "
			"They return { action, outcome, drawn, weaponState, equipped, waitedMs, calls?, mainThreadStalls?, note? }; "
			"outcome is 'switched', 'already' (nothing was called), 'refused' (the state did not settle in time) or 'notWaited'. "
			"sheatheWeapon also puts away drawn fists, which unequipping a weapon leaves up. 409 when no save is loaded.";
		player.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "status", "findPowerArmor", "enterPowerArmor", "exitPowerArmor", "stepIn", "stepOut", "weapon", "drawWeapon", "sheatheWeapon" }) }, { "description", "default 'status'" } } },
								{ "formId", json{ { "type", "string" }, { "description", "enterPowerArmor / stepIn: the frame reference, hex (\"0x0001F2A3\"), as findPowerArmor lists it; default the nearest frame within radius" } } },
								{ "radius", json{ { "type", "number" }, { "minimum", 1 }, { "maximum", kMaxRadius }, { "description", "findPowerArmor, or enterPowerArmor / stepIn without formId: how far to look, in game units (default 500)" } } },
								{ "limit", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", kMaxLimit }, { "description", "findPowerArmor: most frames to list (default 5)" } } },
								{ "wait", json{ { "type", "boolean" }, { "description", "enterPowerArmor / exitPowerArmor / drawWeapon / sheatheWeapon: wait for the engine state to flip (default true)" } } },
								{ "timeoutMs", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", kMaxTimeoutMs }, { "description", "enterPowerArmor / exitPowerArmor / stepIn / stepOut / drawWeapon / sheatheWeapon: how long to wait for the state (default 10000; stepOut adds its 2 s hold)" } } },
							} },
		};
		a_registry.Register(std::move(player), &HandlePlayer);
	}
}
