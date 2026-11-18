#include "core/VRInput.h"

#include "core/EventBus.h"
#include "core/Host.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PoseControl.h"
#include "core/ToolRegistry.h"
#include "core/VRHost.h"
#include "core/VRInputState.h"

// Each platform puts one openvr.h on the include path: Skyrim through CommonLibSSE-NG,
// Fallout 4 through the vendored copy in extern/openvr. Only the vtable slot numbers below
// are tied to an interface version; the structs used here are the same in every version.
#include "openvr.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>

namespace dvb
{
	namespace
	{
		using namespace std::chrono;

		constexpr int         kVRContractVersion = 1;
		constexpr int         kVRContractMinorVersion = 1;
		constexpr const char* kDevice = "vrTrackedSet";
		constexpr int         kDefaultTailMs = 50;
		constexpr int         kMaximumTailMs = 1000;
		constexpr int         kControllerRestoreAttempts = 5;
		constexpr auto        kControllerRestoreRetryDelay = std::chrono::milliseconds(200);
		constexpr auto        kControllerRestoreWaitTimeout = std::chrono::seconds(30);
		constexpr auto        kControllerRecoveryRetryDelay = std::chrono::seconds(1);

		// Vtable slots from the pinned OpenVR IVRCompositor_022 and IVRSystem_019 ABIs.
		constexpr std::size_t kIVRCompositorWaitGetPosesSlot = 2;
		constexpr std::size_t kIVRCompositorGetLastPosesSlot = 3;
		constexpr std::size_t kIVRSystemGetDeviceToAbsoluteTrackingPoseSlot = 11;
		constexpr std::size_t kIVRSystemGetTrackedDeviceIndexForControllerRoleSlot = 18;
		constexpr std::size_t kIVRSystemGetControllerRoleForTrackedDeviceIndexSlot = 19;
		constexpr std::size_t kIVRSystemGetTrackedDeviceClassSlot = 20;
		constexpr std::size_t kIVRSystemIsTrackedDeviceConnectedSlot = 21;
		constexpr std::size_t kIVRSystemGetControllerStateSlot = 34;
		constexpr std::size_t kIVRSystemGetControllerStateWithPoseSlot = 35;

		std::atomic<bool> g_vrReady{ false };
		std::mutex        g_installMutex;

		struct HookSlot
		{
			const char* name;
			bool        closestToRuntime;
		};
		// Written once by InstallHooks before g_vrReady is set; read-only afterwards.
		std::vector<HookSlot> g_hookSlots;

		HMODULE ModuleOf(const void* a_address)
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCWSTR>(a_address), &module);
			return module;
		}

		/**
		 * Replaces one slot of a COM-style object's vtable and returns the previous entry.
		 * The vtable is shared by every holder of the interface, so a hook installed later on
		 * the same slot (another mod) runs before this one and chains to it. Records whether
		 * the previous entry still pointed into the runtime module that owns the vtable, i.e.
		 * whether devbench sits closest to the runtime on that slot.
		 */
		template <class Fn>
		Fn PatchSlot(void* a_object, std::size_t a_slot, const char* a_name, Fn a_hook)
		{
			auto** vtable = *static_cast<void***>(a_object);
			DWORD  oldProtect = 0;
			VirtualProtect(&vtable[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
			void* previous = vtable[a_slot];
			vtable[a_slot] = reinterpret_cast<void*>(a_hook);
			VirtualProtect(&vtable[a_slot], sizeof(void*), oldProtect, &oldProtect);
			const HMODULE runtime = ModuleOf(vtable);
			g_hookSlots.push_back({ a_name, runtime && ModuleOf(previous) == runtime });
			return reinterpret_cast<Fn>(previous);
		}

		json HooksJson()
		{
			json out = json::object();
			for (const auto& slot : g_hookSlots)
				out[slot.name] = json{ { "closestToRuntime", slot.closestToRuntime } };
			return out;
		}

		bool BooleanArgument(const json& a_args, const char* a_name, bool a_default)
		{
			if (!a_args.contains(a_name))
				return a_default;
			if (!a_args.at(a_name).is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return a_args.at(a_name).get<bool>();
		}

		int IntegerArgument(const json& a_args, const char* a_name, int a_default)
		{
			try {
				return static_cast<int>(ParseBoundedIntegerArgument(a_args, a_name, a_default,
					std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
			} catch (const std::invalid_argument& e) {
				throw ToolError(400, e.what());
			}
		}

		std::string ResolveOwner(const json& a_args, const ToolContext& a_ctx)
		{
			std::string owner;
			if (a_args.contains("owner")) {
				if (!a_args["owner"].is_string())
					throw ToolError(400, "'owner' must be a string");
				owner = a_args["owner"].get<std::string>();
			} else if (!a_ctx.clientId.empty()) {
				owner = "mcp:" + a_ctx.clientId;
			} else {
				owner = "rest:anonymous";
			}
			if (owner.empty() || owner.size() > 128)
				throw ToolError(400, "'owner' must contain 1..128 characters");
			return owner;
		}

		std::string CreateControlToken()
		{
			std::random_device source;
			std::ostringstream token;
			token << std::hex << std::setfill('0');
			for (int i = 0; i < 4; ++i)
				token << std::setw(8) << source();
			return token.str();
		}

		json ContractJson()
		{
			return json{ { "name", "devbench.input.vrTrackedSet" },
				{ "version", json{ { "major", kVRContractVersion },
								 { "minor", kVRContractMinorVersion } } } };
		}

		vr::TrackedDevicePose_t OpenVRPose(const VRTrackedPoseState& a_pose)
		{
			vr::TrackedDevicePose_t out{};
			out.bDeviceIsConnected = a_pose.connected;
			out.bPoseIsValid = a_pose.valid;
			out.eTrackingResult = static_cast<vr::ETrackingResult>(a_pose.trackingResult);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 4; ++col)
					out.mDeviceToAbsoluteTracking.m[row][col] = a_pose.matrix[row * 4 + col];
			for (int i = 0; i < 3; ++i) {
				out.vVelocity.v[i] = a_pose.velocity[i];
				out.vAngularVelocity.v[i] = a_pose.angularVelocity[i];
			}
			return out;
		}

		vr::VRControllerState_t OpenVRController(const VRControllerState& a_state)
		{
			vr::VRControllerState_t out{};
			out.unPacketNum = a_state.packetNumber;
			out.ulButtonPressed = a_state.pressed;
			out.ulButtonTouched = a_state.touched;
			for (std::size_t i = 0; i < vr::k_unControllerStateAxisCount; ++i) {
				out.rAxis[i].x = a_state.axes[i * 2];
				out.rAxis[i].y = a_state.axes[i * 2 + 1];
			}
			return out;
		}

		class VRTrackedSetManager
		{
		public:
			static VRTrackedSetManager& Get()
			{
				// Deliberately process-lifetime: hook thunks and detached bounded playback workers may
				// reference it while the game is shutting down.
				static auto* instance = new VRTrackedSetManager();
				return *instance;
			}

			void SetEvents(EventBus& a_events)
			{
				std::lock_guard lock(m_mutex);
				m_events = &a_events;
			}

			bool HeldBy(std::string_view a_owner, std::string_view a_controlToken)
			{
				std::lock_guard lock(m_mutex);
				return m_transaction.CanReplace(a_owner, a_controlToken);
			}

			json Status()
			{
				std::lock_guard lock(m_mutex);
				json            out{
					{ "contract", ContractJson() },
					{ "device", kDevice },
					{ "ready", g_vrReady.load(std::memory_order_relaxed) },
					{ "active", m_transaction.Active() },
					{ "starting", m_transaction.Starting() },
					{ "restoring", m_transaction.Restoring() },
					{ "restorationAttemptActive", m_transaction.RestoreAttemptActive() },
					{ "generation", m_transaction.Generation() },
				};
				if (g_vrReady.load(std::memory_order_acquire))
					out["hooks"] = HooksJson();
				if (m_transaction.Active()) {
					out["owner"] = m_transaction.Owner();
					out["surviveLifecycle"] = m_transaction.SurvivesLifecycle();
					out["frameIndex"] = m_frameIndex;
					out["frameCount"] = m_frameCount;
					out["elapsedMs"] = duration_cast<milliseconds>(steady_clock::now() - m_started).count();
					out["durationMs"] = m_durationMs;
					if (m_current)
						out["current"] = json{ { "tMs", m_current->tMs }, { "seq", m_current->seq },
							{ "originCode", m_current->originCode } };
				} else if (!m_lastCompletion.empty()) {
					out["lastCompletion"] = m_lastCompletion;
				}
				return out;
			}

			json Start(const json& a_args, const std::string& a_owner, bool a_internal)
			{
				if (!g_vrReady.load(std::memory_order_relaxed))
					throw ToolError(503, "OpenVR tracked-set injection is not ready; the VR game and its OpenVR interfaces must be initialized");
				if (!a_args.contains("frames"))
					throw ToolError(400, "VR tracked-set sequence requires 'frames'");
				std::vector<VRTrackedInputFrame> frames;
				try {
					frames = ParseVRTrackedInputFrames(a_args["frames"]);
				} catch (const std::exception& e) {
					throw ToolError(400, e.what());
				}
				const int tailMs = IntegerArgument(a_args, "tailMs", kDefaultTailMs);
				if (tailMs < 10 || tailMs > kMaximumTailMs)
					throw ToolError(400, std::format("'tailMs' must be between 10 and {}", kMaximumTailMs));
				const bool surviveLifecycle = BooleanArgument(a_args, "surviveLifecycle", false);
				if (surviveLifecycle && !a_internal)
					throw ToolError(403, "'surviveLifecycle' is reserved for the internal recording-replay transaction");
				auto* compositor = vrhost::Compositor();
				if (!compositor)
					throw ToolError(503, "OpenVR compositor is unavailable");
				const auto requestedOrigin = static_cast<vr::ETrackingUniverseOrigin>(frames.front().originCode);
				if (compositor->GetTrackingSpace() != requestedOrigin)
					throw ToolError(409, "recorded OpenVR origin does not match the compositor tracking space; select a compatible capture instead of mutating global tracking state");
				if (a_args.contains("controlToken") && !a_args["controlToken"].is_string())
					throw ToolError(400, "'controlToken' must be a string");
				const std::string replaceToken = a_args.value("controlToken", std::string{});
				if (!replaceToken.empty()) {
					if (auto replaced = Replace(frames, tailMs, a_owner, replaceToken, surviveLifecycle))
						return *replaced;
				}

				std::uint64_t generation;
				std::uint64_t playback = 0;
				const auto    frameCount = frames.size();
				const auto    durationMs = frames.back().tMs + tailMs;
				const auto    controlToken = CreateControlToken();
				{
					std::lock_guard lock(m_mutex);
					if (!m_transaction.Begin(a_owner, controlToken, surviveLifecycle))
						throw ToolError(409, std::format("VR tracked-set sequence is owned by '{}'",
												 m_transaction.Owner()));
					m_pendingTerminalEvent.clear();
					m_pendingCompletion = json::object();
					generation = m_transaction.Generation();
				}

				try {
					if (!ApplyControllerIndices(generation, frames.front().left.pose.index,
							frames.front().right.pose.index))
						throw ToolError(409, "VR tracked-set activation was cancelled before controller indices were applied");
					steady_clock::time_point started;
					{
						std::lock_guard lock(m_mutex);
						if (!m_transaction.Commit(generation))
							throw ToolError(409, "VR tracked-set activation was superseded");
						started = steady_clock::now();
						playback = ++m_playback;
						m_leftIndex = frames.front().left.pose.index;
						m_rightIndex = frames.front().right.pose.index;
						m_frameIndex = 0;
						m_frameCount = frameCount;
						m_durationMs = durationMs;
						m_started = started;
						m_current = frames.front();
						m_lastCompletion = json::object();
						PublishLocked("started", json{ { "owner", a_owner }, { "generation", generation },
													 { "frameCount", frameCount }, { "durationMs", durationMs },
													 { "surviveLifecycle", surviveLifecycle } });
					}
					std::thread worker([this, generation, playback, owner = a_owner,
										   frames = std::move(frames), tailMs, started]() mutable {
						Playback(generation, playback, owner, std::move(frames), tailMs, started);
					});
					worker.detach();
				} catch (...) {
					AbortActivation(generation);
					throw;
				}
				return json{ { "contract", ContractJson() }, { "device", kDevice },
					{ "action", "sequence" }, { "queued", true }, { "owner", a_owner },
					{ "controlToken", controlToken },
					{ "generation", generation }, { "frameCount", frameCount },
					{ "durationMs", durationMs }, { "surviveLifecycle", surviveLifecycle } };
			}

			void StopForLifecycle(std::string_view a_reason)
			{
				std::uint64_t generation = 0;
				std::string   owner;
				{
					std::lock_guard lock(m_mutex);
					const auto      decision = m_transaction.CancelForLifecycle();
					if (!decision.present)
						return;
					if (decision.preserved) {
						dlog::info("devbench: preserving owned VR tracked-set sequence '{}' across {}",
							decision.owner, a_reason);
						return;
					}
					owner = decision.owner;
					generation = decision.generation;
					// Cancel activation before the lifecycle callback returns. A queued index
					// mutation rechecks the transaction and becomes a no-op after cancellation.
					m_current.reset();
				}
				m_cv.notify_all();
				const std::string reason(a_reason);
				try {
					std::thread([this, generation, owner = std::move(owner), reason]() mutable {
						if (!Finish(generation, "stopped", json{ { "owner", std::move(owner) }, { "generation", generation }, { "reason", reason }, { "completed", false } }))
							ScheduleRestorationRecovery(generation);
					}).detach();
				} catch (const std::exception& e) {
					dlog::warn("devbench: VR lifecycle cleanup worker could not start: {}", e.what());
					ScheduleRestorationRecovery(generation);
				}
			}

			json Stop(const std::string& a_owner, const std::string& a_controlToken,
				bool a_force, bool a_internal, std::string_view a_reason)
			{
				std::string   owner;
				std::uint64_t generation = 0;
				json          completion;
				{
					std::lock_guard lock(m_mutex);
					const auto      access = m_transaction.AuthorizeStop(
						a_owner, a_controlToken, a_force, a_internal);
					if (access == VRSequenceStopAccess::kNotActive)
						return json{ { "contract", ContractJson() }, { "device", kDevice },
							{ "action", "stop" }, { "stopped", false }, { "notActive", true } };
					if (access == VRSequenceStopAccess::kForceRequiresInternal)
						throw ToolError(403, "cross-owner VR cleanup is reserved for internal lifecycle/replay paths");
					if (access == VRSequenceStopAccess::kControlTokenMismatch)
						throw ToolError(409, "VR tracked-set stop requires the sequence's controlToken");
					if (access == VRSequenceStopAccess::kOwnerMismatch)
						throw ToolError(409, std::format("VR tracked-set sequence is owned by '{}' (not '{}')",
												 m_transaction.Owner(), a_owner));
					owner = m_transaction.Owner();
					generation = m_transaction.Generation();
					completion = json{ { "owner", owner }, { "generation", generation },
						{ "reason", a_reason }, { "completed", false } };
				}
				bool restored = Finish(generation, "stopped", std::move(completion));
				if (!restored)
					restored = WaitForConcurrentRestoration(generation);
				return json{ { "contract", ContractJson() }, { "device", kDevice },
					{ "action", "stop" }, { "stopRequested", true }, { "stopped", restored }, { "owner", owner },
					{ "generation", generation }, { "reason", a_reason },
					{ "restored", restored }, { "restorationPending", !restored } };
			}

			/**
			 * Lays a `press` over the active sequence for one hand. Only the sequence's owner may
			 * press, and the press ends with that sequence even if its duration has not run out.
			 * Each hand has its own slot: presses on both hands overlap, a second on one hand
			 * replaces its first.
			 */
			json Press(const json& a_args, const std::string& a_owner)
			{
				VRPressRequest request;
				try {
					request = ParseVRPress(a_args);
				} catch (const std::exception& e) {
					throw ToolError(400, e.what());
				}
				std::lock_guard lock(m_mutex);
				if (!m_transaction.Active())
					throw ToolError(409, "press needs an active sequence (a pose hold, or a reach run that is holding, is one) to lay the buttons over");
				if (m_transaction.Owner() != a_owner)
					throw ToolError(409, std::format("the active sequence is owned by '{}' (not '{}'); press as its owner",
											 m_transaction.Owner(), a_owner));
				const auto now = steady_clock::now();
				const auto slot = request.right ? 1u : 0u;
				const auto remainingMs = m_durationMs - duration_cast<milliseconds>(now - m_started).count();
				json       result{ { "contract", ContractJson() }, { "device", kDevice }, { "action", "press" },
					{ "owner", a_owner }, { "generation", m_transaction.Generation() },
					{ "hand", request.right ? "right" : "left" }, { "buttons", request.buttons },
					{ "delayMs", request.delayMs }, { "durationMs", request.durationMs },
					{ "sequenceRemainingMs", remainingMs } };
				if (request.stick)
					result["stick"] = *request.stick;
				m_presses[slot] = ActivePress{ std::move(request), m_transaction.Generation(),
					now + milliseconds(result["delayMs"].get<std::int64_t>()),
					now + milliseconds(result["delayMs"].get<std::int64_t>() + result["durationMs"].get<std::int64_t>()) };
				++m_pressStarts[slot];
				PublishLocked("press", result);
				return result;
			}

			std::optional<VRTrackedInputFrame> Current() const
			{
				std::lock_guard lock(m_mutex);
				if (!m_transaction.Active() || !m_current)
					return std::nullopt;
				VRTrackedInputFrame frame = *m_current;
				const auto          now = steady_clock::now();
				ApplyPress(frame.left.controller, 0, now);
				ApplyPress(frame.right.controller, 1, now);
				return frame;
			}

		private:
			struct ActivePress
			{
				VRPressRequest           request;
				std::uint64_t            generation = 0;
				steady_clock::time_point from;
				steady_clock::time_point until;
			};

			/**
			 * Applies a hand's press while its window is open. The packet number moves on at every
			 * press start and end (offset 2n-1 inside the n-th press, 2n outside), so a reader that
			 * only re-reads state when the packet number changes still sees both edges.
			 */
			void ApplyPress(VRControllerState& a_state, std::size_t a_slot, steady_clock::time_point a_now) const
			{
				const auto& press = m_presses[a_slot];
				const bool  inWindow = press && press->generation == m_transaction.Generation() &&
				                       a_now >= press->from && a_now < press->until;
				if (inWindow)
					a_state = ApplyVRPress(a_state, press->request);
				a_state.packetNumber += 2 * m_pressStarts[a_slot] - (inWindow ? 1 : 0);
			}

			/**
			 * Swaps the running sequence's frames for new ones without releasing the devices to
			 * the physical controllers: same transaction, owner, token and applied controller
			 * indices; the old playback worker sees its playback number superseded and exits.
			 * Returns nullopt when there is nothing this caller may replace, so a normal start runs.
			 */
			std::optional<json> Replace(std::vector<VRTrackedInputFrame>& a_frames, int a_tailMs,
				const std::string& a_owner, const std::string& a_controlToken, bool a_surviveLifecycle)
			{
				const auto               frameCount = a_frames.size();
				const auto               durationMs = a_frames.back().tMs + a_tailMs;
				std::uint64_t            generation = 0;
				std::uint64_t            playback = 0;
				steady_clock::time_point started;
				{
					std::lock_guard lock(m_mutex);
					if (!m_transaction.CanReplace(a_owner, a_controlToken))
						return std::nullopt;
					if (a_frames.front().left.pose.index != m_leftIndex ||
						a_frames.front().right.pose.index != m_rightIndex)
						throw ToolError(409, "replacing a running sequence keeps its controller indices; stop it to change them");
					if (a_surviveLifecycle != m_transaction.SurvivesLifecycle())
						throw ToolError(409, "replacing a running sequence keeps its 'surviveLifecycle'");
					generation = m_transaction.Generation();
					playback = ++m_playback;
					started = steady_clock::now();
					m_frameIndex = 0;
					m_frameCount = frameCount;
					m_durationMs = durationMs;
					m_started = started;
					m_current = a_frames.front();
					PublishLocked("replaced", json{ { "owner", a_owner }, { "generation", generation },
												  { "frameCount", frameCount }, { "durationMs", durationMs } });
				}
				m_cv.notify_all();
				try {
					std::thread([this, generation, playback, owner = a_owner,
									frames = std::move(a_frames), a_tailMs, started]() mutable {
						Playback(generation, playback, owner, std::move(frames), a_tailMs, started);
					}).detach();
				} catch (...) {
					AbortActivation(generation);
					throw;
				}
				return json{ { "contract", ContractJson() }, { "device", kDevice },
					{ "action", "sequence" }, { "queued", true }, { "replaced", true }, { "owner", a_owner },
					{ "controlToken", a_controlToken },
					{ "generation", generation }, { "frameCount", frameCount },
					{ "durationMs", durationMs }, { "surviveLifecycle", a_surviveLifecycle } };
			}

			/**
			 * Plays one sequence's frames on its own thread until they and the tail run out, then
			 * finishes the transaction. Exits quietly when the transaction ends or a replace
			 * takes over (the generation or playback number moves on).
			 */
			void Playback(std::uint64_t a_generation, std::uint64_t a_playback, const std::string& a_owner,
				std::vector<VRTrackedInputFrame> a_frames, int a_tailMs,
				steady_clock::time_point a_started)
			{
				const auto superseded = [&] {
					return !m_transaction.Active() || m_transaction.Generation() != a_generation ||
					       m_playback != a_playback;
				};
				for (std::size_t i = 1; i < a_frames.size(); ++i) {
					std::unique_lock lock(m_mutex);
					const auto       due = a_started + milliseconds(a_frames[i].tMs);
					if (m_cv.wait_until(lock, due, superseded))
						return;
					m_current = a_frames[i];
					m_frameIndex = i;
					lock.unlock();
				}
				json completion;
				{
					std::unique_lock lock(m_mutex);
					const auto       due = a_started + milliseconds(a_frames.back().tMs + a_tailMs);
					if (m_cv.wait_until(lock, due, superseded))
						return;
					completion = json{ { "owner", a_owner }, { "generation", a_generation },
						{ "reason", "complete" }, { "completed", true }, { "frameCount", a_frames.size() } };
				}
				if (!Finish(a_generation, "finished", std::move(completion), a_playback))
					ScheduleRestorationRecovery(a_generation);
			}

			bool ApplyControllerIndices(std::uint64_t a_generation, std::uint32_t a_left,
				std::uint32_t a_right)
			{
				const json result = MainThread::RunAndWait([this, a_generation, a_left, a_right]() -> json {
					std::lock_guard lock(m_mutex);
					if (!m_transaction.CanApplyIndices(a_generation))
						return json{ { "applied", false }, { "superseded", true } };
					std::tie(m_previousLeftIndex, m_previousRightIndex) = vrhost::SetControllerIndices(a_left, a_right);
					m_transaction.MarkIndicesApplied(a_generation);
					return json{ { "applied", true } };
				});
				return result.value("applied", false);
			}

			bool RestoreControllerIndices(std::uint64_t a_generation)
			{
				for (int attempt = 1; attempt <= kControllerRestoreAttempts; ++attempt) {
					try {
						json result = MainThread::RunAndWait([this, a_generation]() -> json {
							std::string event;
							json        completion;
							{
								std::lock_guard lock(m_mutex);
								if (!m_transaction.Restoring() ||
									m_transaction.Generation() != a_generation)
									return json{ { "restored", !m_transaction.IndicesApplied() },
										{ "superseded", true } };
								vrhost::SetControllerIndices(m_previousLeftIndex, m_previousRightIndex);
								if (!m_transaction.RestoreSucceeded(a_generation))
									return json{ { "restored", false }, { "superseded", true } };
								completion = m_pendingCompletion;
								completion["controllerIndicesRestored"] = true;
								event = m_pendingTerminalEvent;
								m_pendingCompletion = json::object();
								m_pendingTerminalEvent.clear();
								m_lastCompletion = completion;
								if (!event.empty())
									PublishLocked(event.c_str(), std::move(completion));
							}
							m_cv.notify_all();
							return json{ { "restored", true } };
						});
						if (result.value("restored", false))
							return true;
					} catch (const ToolError& e) {
						if (e.code == 503 && attempt < kControllerRestoreAttempts) {
							dlog::warn("devbench: VR tracked-set controller restoration attempt {}/{} deferred: {}",
								attempt, kControllerRestoreAttempts, e.what());
							std::this_thread::sleep_for(kControllerRestoreRetryDelay);
							continue;
						}
						dlog::warn("devbench: VR tracked-set controller restoration remains pending after {} attempt(s): {}",
							attempt, e.what());
						break;
					} catch (const std::exception& e) {
						dlog::warn("devbench: VR tracked-set controller restoration remains pending: {}", e.what());
						break;
					}
				}
				{
					std::lock_guard lock(m_mutex);
					if (m_transaction.Generation() == a_generation) {
						m_transaction.RestoreFailed(a_generation);
						m_lastCompletion = m_pendingCompletion;
						m_lastCompletion["controllerIndicesRestored"] = false;
						m_lastCompletion["restorationPending"] = true;
					}
				}
				m_cv.notify_all();
				return false;
			}

			void AbortActivation(std::uint64_t a_generation)
			{
				std::string owner;
				{
					std::lock_guard lock(m_mutex);
					owner = m_transaction.Owner();
				}
				if (!Finish(a_generation, "aborted", json{ { "owner", std::move(owner) }, { "generation", a_generation }, { "reason", "activationFailed" }, { "completed", false } }))
					ScheduleRestorationRecovery(a_generation);
			}

			bool WaitForConcurrentRestoration(std::uint64_t a_generation)
			{
				std::unique_lock lock(m_mutex);
				if (m_transaction.Generation() != a_generation)
					return true;
				if (!m_transaction.Restoring())
					return !m_transaction.IndicesApplied();
				if (!m_transaction.RestoreAttemptActive())
					return false;

				if (!m_cv.wait_for(lock, kControllerRestoreWaitTimeout, [&] {
						return m_transaction.Generation() != a_generation ||
						       !m_transaction.Restoring() ||
						       !m_transaction.RestoreAttemptActive();
					}))
					return false;
				return m_transaction.Generation() != a_generation ||
				       (!m_transaction.Restoring() && !m_transaction.IndicesApplied());
			}

			void ScheduleRestorationRecovery(std::uint64_t a_generation) noexcept
			{
				{
					std::lock_guard lock(m_mutex);
					if (m_transaction.Generation() != a_generation || !m_transaction.Restoring() ||
						(m_recoveryGeneration && *m_recoveryGeneration == a_generation))
						return;
					m_recoveryGeneration = a_generation;
				}

				try {
					std::thread([this, a_generation] {
						for (;;) {
							std::this_thread::sleep_for(kControllerRecoveryRetryDelay);
							{
								std::lock_guard lock(m_mutex);
								if (m_transaction.Generation() != a_generation ||
									!m_transaction.Restoring()) {
									if (m_recoveryGeneration && *m_recoveryGeneration == a_generation)
										m_recoveryGeneration.reset();
									return;
								}
							}
							if (Finish(a_generation, nullptr, json::object())) {
								std::lock_guard lock(m_mutex);
								if (m_recoveryGeneration && *m_recoveryGeneration == a_generation)
									m_recoveryGeneration.reset();
								return;
							}
						}
					}).detach();
				} catch (const std::exception& e) {
					std::lock_guard lock(m_mutex);
					if (m_recoveryGeneration && *m_recoveryGeneration == a_generation)
						m_recoveryGeneration.reset();
					dlog::warn("devbench: unable to schedule VR restoration recovery: {}", e.what());
				} catch (...) {
					std::lock_guard lock(m_mutex);
					if (m_recoveryGeneration && *m_recoveryGeneration == a_generation)
						m_recoveryGeneration.reset();
					dlog::warn("devbench: unable to schedule VR restoration recovery");
				}
			}

			/**
			 * Ends the transaction for a_generation (once) and restores controller indices if they
			 * were applied. A playback worker passes its playback number so a replace that took
			 * over after its last wait cannot be ended by it.
			 */
			bool Finish(std::uint64_t a_generation, const char* a_event, json a_completion,
				std::optional<std::uint64_t> a_playback = std::nullopt)
			{
				VRSequenceFinishAction action = VRSequenceFinishAction::kNone;
				json                   completionToPublish;
				std::string            eventToPublish;
				{
					std::lock_guard lock(m_mutex);
					if (a_playback && *a_playback != m_playback)
						return true;
					const bool firstClaim = !m_transaction.TerminalClaimed();
					action = m_transaction.ClaimFinish(a_generation);
					if (action == VRSequenceFinishAction::kNone)
						return !m_transaction.Restoring() && !m_transaction.IndicesApplied();
					if (firstClaim) {
						m_current.reset();
						m_pendingTerminalEvent = a_event ? a_event : "";
						m_pendingCompletion = std::move(a_completion);
						m_pendingCompletion["controllerIndicesRestored"] =
							!m_transaction.Restoring();
						m_lastCompletion = m_pendingCompletion;
					}
					if (action == VRSequenceFinishAction::kPublish) {
						completionToPublish = std::move(m_pendingCompletion);
						eventToPublish = std::move(m_pendingTerminalEvent);
						m_pendingCompletion = json::object();
						m_pendingTerminalEvent.clear();
						m_lastCompletion = completionToPublish;
						if (!eventToPublish.empty())
							PublishLocked(eventToPublish.c_str(), std::move(completionToPublish));
					}
				}
				m_cv.notify_all();
				if (action == VRSequenceFinishAction::kRestore)
					return RestoreControllerIndices(a_generation);
				return true;
			}

			// Call while holding m_mutex so terminal events cannot overtake "started".
			// EventBus only queues here; subscriber callbacks run on its worker thread.
			void PublishLocked(const char* a_state, json a_payload) noexcept
			{
				if (m_events)
					try {
						a_payload["state"] = a_state;
						m_events->Publish("input.vrTrackedSet", std::move(a_payload));
					} catch (const std::exception& e) {
						dlog::warn("devbench: VR tracked-set event publish failed: {}", e.what());
					}
			}

			mutable std::mutex                        m_mutex;
			std::condition_variable                   m_cv;
			EventBus*                                 m_events = nullptr;
			VRSequenceTransaction                     m_transaction;
			std::uint32_t                             m_previousLeftIndex = vr::k_unTrackedDeviceIndexInvalid;
			std::uint32_t                             m_previousRightIndex = vr::k_unTrackedDeviceIndexInvalid;
			std::string                               m_pendingTerminalEvent;
			std::uint64_t                             m_playback = 0;
			std::uint32_t                             m_leftIndex = vr::k_unTrackedDeviceIndexInvalid;
			std::uint32_t                             m_rightIndex = vr::k_unTrackedDeviceIndexInvalid;
			std::size_t                               m_frameIndex = 0;
			std::size_t                               m_frameCount = 0;
			std::int64_t                              m_durationMs = 0;
			steady_clock::time_point                  m_started;
			std::optional<VRTrackedInputFrame>        m_current;
			json                                      m_pendingCompletion = json::object();
			json                                      m_lastCompletion = json::object();
			std::optional<std::uint64_t>              m_recoveryGeneration;
			std::array<std::optional<ActivePress>, 2> m_presses;
			std::array<std::uint32_t, 2>              m_pressStarts{};
		};

		bool CanOverridePoseArray(const VRTrackedInputFrame& a_frame,
			vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count)
		{
			if (a_count == 0)
				return true;
			if (!a_poses)
				return false;
			return a_frame.hmd.index < a_count && a_frame.left.pose.index < a_count &&
			       a_frame.right.pose.index < a_count;
		}

		void OverridePoseArray(const VRTrackedInputFrame& a_frame,
			vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count)
		{
			if (a_count == 0)
				return;
			const auto apply = [&](const VRTrackedPoseState& a_pose) {
				if (a_pose.index < a_count)
					a_poses[a_pose.index] = OpenVRPose(a_pose);
			};
			apply(a_frame.hmd);
			apply(a_frame.left.pose);
			apply(a_frame.right.pose);
		}

		using WaitGetPosesFn = vr::EVRCompositorError (*)(vr::IVRCompositor*,
			vr::TrackedDevicePose_t*, std::uint32_t, vr::TrackedDevicePose_t*, std::uint32_t);
		using GetTrackingPoseFn = void (*)(vr::IVRSystem*, vr::ETrackingUniverseOrigin, float,
			vr::TrackedDevicePose_t*, std::uint32_t);
		using RoleIndexFn = vr::TrackedDeviceIndex_t (*)(vr::IVRSystem*, vr::ETrackedControllerRole);
		using IndexRoleFn = vr::ETrackedControllerRole (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t);
		using DeviceClassFn = vr::ETrackedDeviceClass (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t);
		using ConnectedFn = bool (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t);
		using ControllerStateFn = bool (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t,
			vr::VRControllerState_t*, std::uint32_t);
		using ControllerStatePoseFn = bool (*)(vr::IVRSystem*, vr::ETrackingUniverseOrigin,
			vr::TrackedDeviceIndex_t, vr::VRControllerState_t*, std::uint32_t,
			vr::TrackedDevicePose_t*);

		WaitGetPosesFn        g_waitGetPoses = nullptr;
		WaitGetPosesFn        g_getLastPoses = nullptr;
		GetTrackingPoseFn     g_getTrackingPose = nullptr;
		RoleIndexFn           g_roleIndex = nullptr;
		IndexRoleFn           g_indexRole = nullptr;
		DeviceClassFn         g_deviceClass = nullptr;
		ConnectedFn           g_connected = nullptr;
		ControllerStateFn     g_controllerState = nullptr;
		ControllerStatePoseFn g_controllerStatePose = nullptr;

		VRTrackedPoseState ReadPose(const vr::TrackedDevicePose_t& a_pose,
			vr::TrackedDeviceIndex_t                               a_index)
		{
			VRTrackedPoseState out;
			out.available = a_index != vr::k_unTrackedDeviceIndexInvalid;
			if (!out.available)
				return out;
			out.index = a_index;
			out.connected = a_pose.bDeviceIsConnected;
			out.valid = a_pose.bPoseIsValid;
			out.trackingResult = static_cast<std::int32_t>(a_pose.eTrackingResult);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 4; ++col)
					out.matrix[row * 4 + col] = a_pose.mDeviceToAbsoluteTracking.m[row][col];
			for (int i = 0; i < 3; ++i) {
				out.velocity[i] = a_pose.vVelocity.v[i];
				out.angularVelocity[i] = a_pose.vAngularVelocity.v[i];
			}
			return out;
		}

		VRControllerState ReadController(vr::IVRSystem* a_system,
			vr::TrackedDeviceIndex_t                    a_index)
		{
			VRControllerState out;
			if (a_index == vr::k_unTrackedDeviceIndexInvalid)
				return out;
			vr::VRControllerState_t state{};
			if (!g_controllerState(a_system, a_index, &state, sizeof(state)))
				return out;
			out.packetNumber = state.unPacketNum;
			out.pressed = state.ulButtonPressed;
			out.touched = state.ulButtonTouched;
			for (std::size_t i = 0; i < 5; ++i) {
				out.axes[i * 2] = state.rAxis[i].x;
				out.axes[i * 2 + 1] = state.rAxis[i].y;
			}
			return out;
		}

		json ObservePhysicalTrackedSet()
		{
			if (!g_vrReady.load(std::memory_order_relaxed))
				throw ToolError(503, "OpenVR tracked-set observation is not ready");
			auto* system = vrhost::System();
			if (!system)
				throw ToolError(503, "OpenVR system is unavailable");

			auto*                                                              compositor = vrhost::Compositor();
			const auto                                                         origin = compositor ? compositor->GetTrackingSpace() : vr::TrackingUniverseStanding;
			std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
			g_getTrackingPose(system, origin, 0.0f, poses.data(), static_cast<std::uint32_t>(poses.size()));
			const auto leftIndex = g_roleIndex(system, vr::TrackedControllerRole_LeftHand);
			const auto rightIndex = g_roleIndex(system, vr::TrackedControllerRole_RightHand);
			const auto poseFor = [&](vr::TrackedDeviceIndex_t a_index) {
				return a_index < poses.size() ? ReadPose(poses[a_index], a_index) : VRTrackedPoseState{};
			};

			VRTrackedInputFrame frame;
			frame.tMs = 0;
			frame.seq = 1;
			frame.originCode = static_cast<std::int32_t>(origin);
			frame.hmd = poseFor(vr::k_unTrackedDeviceIndex_Hmd);
			frame.left.pose = poseFor(leftIndex);
			frame.left.controller = ReadController(system, leftIndex);
			frame.right.pose = poseFor(rightIndex);
			frame.right.controller = ReadController(system, rightIndex);
			return json{ { "contract", ContractJson() }, { "device", kDevice },
				{ "action", "observe" }, { "source", "physical_openvr" },
				{ "frame", VRTrackedInputFrameJson(frame) } };
		}

		vr::EVRCompositorError WaitGetPosesHook(vr::IVRCompositor* a_self,
			vr::TrackedDevicePose_t* a_render, std::uint32_t a_renderCount,
			vr::TrackedDevicePose_t* a_game, std::uint32_t a_gameCount)
		{
			const auto result = g_waitGetPoses(a_self, a_render, a_renderCount, a_game, a_gameCount);
			if (result != vr::VRCompositorError_None)
				return result;
			const auto frame = VRTrackedSetManager::Get().Current();
			if (!frame || a_self->GetTrackingSpace() != static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) ||
				!CanOverridePoseArray(*frame, a_render, a_renderCount) ||
				!CanOverridePoseArray(*frame, a_game, a_gameCount))
				return result;
			OverridePoseArray(*frame, a_render, a_renderCount);
			OverridePoseArray(*frame, a_game, a_gameCount);
			return vr::VRCompositorError_None;
		}

		vr::EVRCompositorError GetLastPosesHook(vr::IVRCompositor* a_self,
			vr::TrackedDevicePose_t* a_render, std::uint32_t a_renderCount,
			vr::TrackedDevicePose_t* a_game, std::uint32_t a_gameCount)
		{
			const auto result = g_getLastPoses(a_self, a_render, a_renderCount, a_game, a_gameCount);
			if (result != vr::VRCompositorError_None)
				return result;
			const auto frame = VRTrackedSetManager::Get().Current();
			if (!frame || a_self->GetTrackingSpace() != static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) ||
				!CanOverridePoseArray(*frame, a_render, a_renderCount) ||
				!CanOverridePoseArray(*frame, a_game, a_gameCount))
				return result;
			OverridePoseArray(*frame, a_render, a_renderCount);
			OverridePoseArray(*frame, a_game, a_gameCount);
			return vr::VRCompositorError_None;
		}

		void GetTrackingPoseHook(vr::IVRSystem* a_self, vr::ETrackingUniverseOrigin a_origin,
			float a_prediction, vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count)
		{
			g_getTrackingPose(a_self, a_origin, a_prediction, a_poses, a_count);
			const auto frame = VRTrackedSetManager::Get().Current();
			if (frame && a_origin == static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) &&
				CanOverridePoseArray(*frame, a_poses, a_count))
				OverridePoseArray(*frame, a_poses, a_count);
		}

		vr::TrackedDeviceIndex_t RoleIndexHook(vr::IVRSystem* a_self, vr::ETrackedControllerRole a_role)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				if (a_role == vr::TrackedControllerRole_LeftHand && frame->left.pose.available)
					return frame->left.pose.index;
				if (a_role == vr::TrackedControllerRole_RightHand && frame->right.pose.available)
					return frame->right.pose.index;
			}
			return g_roleIndex(a_self, a_role);
		}

		vr::ETrackedControllerRole IndexRoleHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				if (frame->left.pose.available && a_index == frame->left.pose.index)
					return vr::TrackedControllerRole_LeftHand;
				if (frame->right.pose.available && a_index == frame->right.pose.index)
					return vr::TrackedControllerRole_RightHand;
			}
			return g_indexRole(a_self, a_index);
		}

		vr::ETrackedDeviceClass DeviceClassHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				if ((frame->hmd.available && a_index == frame->hmd.index))
					return vr::TrackedDeviceClass_HMD;
				if ((frame->left.pose.available && a_index == frame->left.pose.index) ||
					(frame->right.pose.available && a_index == frame->right.pose.index))
					return vr::TrackedDeviceClass_Controller;
			}
			return g_deviceClass(a_self, a_index);
		}

		bool ConnectedHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				for (const auto* pose : { &frame->hmd, &frame->left.pose, &frame->right.pose })
					if (pose->available && a_index == pose->index)
						return pose->connected;
			}
			return g_connected(a_self, a_index);
		}

		bool SyntheticController(vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state,
			std::uint32_t a_size, vr::TrackedDevicePose_t* a_pose)
		{
			if (!a_state)
				return false;
			const auto frame = VRTrackedSetManager::Get().Current();
			if (!frame)
				return false;
			const VRTrackedDeviceState* device = nullptr;
			if (frame->left.pose.available && a_index == frame->left.pose.index)
				device = &frame->left;
			else if (frame->right.pose.available && a_index == frame->right.pose.index)
				device = &frame->right;
			if (!device)
				return false;
			const auto state = OpenVRController(device->controller);
			std::memcpy(a_state, &state, std::min<std::size_t>(a_size, sizeof(state)));
			if (a_pose)
				*a_pose = OpenVRPose(device->pose);
			return true;
		}

		bool ControllerStateHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index,
			vr::VRControllerState_t* a_state, std::uint32_t a_size)
		{
			return SyntheticController(a_index, a_state, a_size, nullptr) ||
			       g_controllerState(a_self, a_index, a_state, a_size);
		}

		bool ControllerStatePoseHook(vr::IVRSystem* a_self, vr::ETrackingUniverseOrigin a_origin,
			vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state,
			std::uint32_t a_size, vr::TrackedDevicePose_t* a_pose)
		{
			const auto frame = VRTrackedSetManager::Get().Current();
			if (frame && a_origin == static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) &&
				SyntheticController(a_index, a_state, a_size, a_pose))
				return true;
			return g_controllerStatePose(a_self, a_origin, a_index, a_state, a_size, a_pose);
		}

		bool InstallHooks()
		{
			if (!host::Get().vr)
				return false;
			auto* system = vrhost::System();
			auto* compositor = vrhost::Compositor();
			if (!system || !compositor)
				return false;

			g_waitGetPoses = PatchSlot(compositor, kIVRCompositorWaitGetPosesSlot, "WaitGetPoses", &WaitGetPosesHook);
			g_getLastPoses = PatchSlot(compositor, kIVRCompositorGetLastPosesSlot, "GetLastPoses", &GetLastPosesHook);

			g_getTrackingPose = PatchSlot(system, kIVRSystemGetDeviceToAbsoluteTrackingPoseSlot, "GetDeviceToAbsoluteTrackingPose", &GetTrackingPoseHook);
			g_roleIndex = PatchSlot(system, kIVRSystemGetTrackedDeviceIndexForControllerRoleSlot, "GetTrackedDeviceIndexForControllerRole", &RoleIndexHook);
			g_indexRole = PatchSlot(system, kIVRSystemGetControllerRoleForTrackedDeviceIndexSlot, "GetControllerRoleForTrackedDeviceIndex", &IndexRoleHook);
			g_deviceClass = PatchSlot(system, kIVRSystemGetTrackedDeviceClassSlot, "GetTrackedDeviceClass", &DeviceClassHook);
			g_connected = PatchSlot(system, kIVRSystemIsTrackedDeviceConnectedSlot, "IsTrackedDeviceConnected", &ConnectedHook);
			g_controllerState = PatchSlot(system, kIVRSystemGetControllerStateSlot, "GetControllerState", &ControllerStateHook);
			g_controllerStatePose = PatchSlot(system, kIVRSystemGetControllerStateWithPoseSlot, "GetControllerStateWithPose", &ControllerStatePoseHook);
			return true;
		}
	}

	json VRInputCapabilities()
	{
		json actions = json::array({ "status", "observe", "sequence", "stop", "releaseAll", "press" });
		for (const auto& action : vrhost::PlatformActions())
			actions.push_back(action);
		json out{
			{ "version", json{ { "major", kVRContractVersion },
							 { "minor", kVRContractMinorVersion } } },
			{ "available", host::Get().vr },
			{ "ready", g_vrReady.load(std::memory_order_relaxed) },
			{ "device", kDevice },
			{ "atomicDevices", json::array({ "hmd", "left", "right" }) },
			{ "injection", "OpenVR IVRCompositor poses + IVRSystem controller state" },
			{ "actions", std::move(actions) },
			{ "pressButtons", VRPressButtonsJson() },
			{ "pressStick", "stick [x, y] in [-1, 1] drives axis 0 (the thumbstick) and touches it for the press" },
			{ "timing", "steadyMillisecondsFromSequenceStart" },
			{ "ordering", "tMs then seq" },
			{ "poseEncoding", "OpenVR device-to-absolute 3x4 row-major" },
			{ "controllerEncoding", "OpenVR packetNumber/pressed/touched/five x-y axes" },
			{ "validation", "complete sequence before activation" },
			{ "ownership", "one bounded sequence owner; start returns a required external cleanup token" },
			{ "replace", "sequence with the running sequence's owner and controlToken swaps its frames without releasing the devices" },
			{ "lifecyclePolicy", "default stop; recording replay may explicitly survive recorded load/new-game boundaries" },
			{ "passThroughWhenInactive", true },
			{ "maximumFrames", kMaximumVRTrackedFrames },
			{ "maximumDurationMs", kMaximumVRTrackedDurationMs },
		};
		if (const auto leftHanded = vrhost::LeftHanded())
			out["leftHanded"] = *leftHanded;
		return out;
	}

	namespace
	{
		/**
		 * The arguments with primary / offhand replaced by the physical controllers for the
		 * game's handedness, read now; unchanged when they name no logical hand.
		 */
		json ResolveLogicalHands(const json& a_args)
		{
			if (!HandednessLogic::UsesLogicalHands(a_args))
				return a_args;
			const auto leftHanded = vrhost::LeftHanded();
			if (!leftHanded)
				throw ToolError(501, "the primary / offhand hand names are not available on this game yet; name the controller (left / right)");
			std::string error;
			auto        resolved = HandednessLogic::ResolveHands(a_args, *leftHanded, error);
			if (!resolved)
				throw ToolError(400, error);
			return *resolved;
		}
	}

	bool IsVRInputDevice(std::string_view a_device)
	{
		return a_device == kDevice;
	}

	/**
	 * Whether the running sequence is a_owner's under a_controlToken: false once it ran out,
	 * stopped, ended at a load, or another sequence took the devices.
	 */
	bool VRSequenceHeldBy(std::string_view a_owner, std::string_view a_controlToken)
	{
		return VRTrackedSetManager::Get().HeldBy(a_owner, a_controlToken);
	}

	std::string ResolveVRInputOwner(const json& a_args, const ToolContext& a_ctx)
	{
		return ResolveOwner(a_args, a_ctx);
	}

	json HandleVRInput(const json& a_args, const ToolContext& a_ctx)
	{
		try {
			if (a_args.contains("action") && !a_args["action"].is_string())
				throw ToolError(400, "'action' must be a string");
			const std::string action = a_args.value("action", std::string("status"));
			if (action == "status")
				return VRTrackedSetManager::Get().Status();
			if (action == "observe")
				return ObservePhysicalTrackedSet();
			const std::string owner = ResolveOwner(a_args, a_ctx);
			if (action == "sequence")
				return VRTrackedSetManager::Get().Start(a_args, owner, a_ctx.internal);
			if (action == "handedness")
				return PoseControl::Handedness(a_args, owner, a_ctx);
			if (action == "pose" || action == "press") {
				const json args = ResolveLogicalHands(a_args);
				json       result = action == "pose" ? PoseControl::Place(args, owner, a_ctx) : VRTrackedSetManager::Get().Press(args, owner);
				if (const auto leftHanded = vrhost::LeftHanded(); leftHanded && result.is_object())
					result["leftHanded"] = *leftHanded;
				return result;
			}
			if (action == "stop" || action == "releaseAll") {
				if (a_args.contains("controlToken") && !a_args["controlToken"].is_string())
					throw ToolError(400, "'controlToken' must be a string");
				return VRTrackedSetManager::Get().Stop(owner,
					a_args.value("controlToken", std::string{}), BooleanArgument(a_args, "all", false),
					a_ctx.internal,
					action == "releaseAll" ? "releaseAll" : "request");
			}
			throw ToolError(400, std::format("unknown vrTrackedSet action '{}' (status|observe|sequence|stop|releaseAll|press|pose|handedness)", action));
		} catch (const json::exception& e) {
			throw ToolError(400, std::format("invalid VR tracked-set JSON: {}", e.what()));
		}
	}

	/**
	 * Installs the hooks once the OpenVR interfaces exist. Idempotent and safe to call
	 * repeatedly from any thread: a platform that cannot tell when OpenVR comes up retries
	 * until this returns true. The "unavailable" warning is logged once.
	 */
	bool MarkVRInputReady(EventBus& a_events)
	{
		std::lock_guard lock(g_installMutex);
		if (g_vrReady.load(std::memory_order_relaxed))
			return true;
		VRTrackedSetManager::Get().SetEvents(a_events);
		if (!InstallHooks()) {
			static bool warned = false;
			if (host::Get().vr && !warned) {
				warned = true;
				dlog::warn("devbench: atomic HMD/controller input API unavailable (OpenVR interfaces not initialized)");
			}
			return false;
		}
		g_vrReady.store(true, std::memory_order_release);
		std::string behind;
		for (const auto& slot : g_hookSlots)
			if (!slot.closestToRuntime)
				behind += std::string(behind.empty() ? "" : ", ") + slot.name;
		dlog::info("devbench: atomic HMD/controller input API ready (contract v{}, OpenVR pass-through hooks)", kVRContractVersion);
		if (!behind.empty())
			dlog::warn("devbench: another hook was already on OpenVR {}; injected input there skips it", behind);
		return true;
	}

	void ReleaseVRInputForLifecycle(const char* a_reason)
	{
		if (!g_vrReady.load(std::memory_order_relaxed))
			return;
		const std::string reason = a_reason ? a_reason : "lifecycle";
		try {
			VRTrackedSetManager::Get().StopForLifecycle(reason);
		} catch (const std::exception& e) {
			dlog::warn("devbench: lifecycle VR tracked-set release failed ({}): {}", reason, e.what());
		}
	}
}
