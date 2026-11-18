#include "ReachControl.h"

#include "core/Log.h"
#include "core/MainThread.h"
#include "core/ReachLogic.h"
#include "core/VRHost.h"
#include "core/VRInput.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dvb::ReachControl
{
	namespace
	{
		namespace RL = dvb::ReachLogic;

		constexpr std::size_t kMaxRuns = 16;
		constexpr double      kCalibrationStep = 0.2;
		constexpr long        kIterationSettleMs = 2400;
		constexpr long        kStopPollMs = 100;
		constexpr long        kShortHoldMs = 3000;
		constexpr long        kSequenceTailMs = 300;
		constexpr long        kCalibrationTailMs = 1000;

		/**
		 * Tracking-space rest pose Reach calibrates around and returns to: the HMD 3 cm over the
		 * origin and both controllers at the origin's height, 0.25 m to the side and 0.3 m in
		 * front. That is the pose Skyrim's Reach has been verified with.
		 */
		struct RestPose
		{
			RL::Vec3 hmd;
			RL::Vec3 left;
			RL::Vec3 right;
		};

		RestPose ReadRestPose()
		{
			constexpr double kEyeHeight = 0.03;
			constexpr double kHandHeight = 0.0;
			return RestPose{ { 0.0, kEyeHeight, 0.0 }, { -0.25, kHandHeight, -0.3 }, { 0.25, kHandHeight, -0.3 } };
		}

		struct Run
		{
			long              runId = 0;
			std::string       owner;
			std::atomic<bool> stop{ false };
			std::atomic<bool> active{ true };
			RL::Progress      progress{ RL::Phase::kQueued, 0, std::nullopt, std::nullopt, std::string{} };
		};

		std::mutex                        g_mutex;
		std::vector<std::shared_ptr<Run>> g_runs;
		long                              g_nextRunId = 1;

		struct ActiveFlag
		{
			std::shared_ptr<Run> run;
			~ActiveFlag() { run->active.store(false); }
		};

		struct StopRequested
		{
		};

		void SetPhase(const std::shared_ptr<Run>& a_run, RL::Phase a_phase)
		{
			std::lock_guard lock(g_mutex);
			a_run->progress.phase = a_phase;
		}

		void SetError(const std::shared_ptr<Run>& a_run, std::string a_error)
		{
			std::lock_guard lock(g_mutex);
			a_run->progress.error = std::move(a_error);
		}

		std::string TokenOf(const json& a_result)
		{
			if (a_result.contains("controlToken") && a_result["controlToken"].is_string())
				return a_result["controlToken"].get<std::string>();
			return {};
		}

		/**
		 * Starts a sequence, or with a_controlToken replaces the running one in place so the
		 * hands never fall back to the physical controllers between Reach's sequences.
		 */
		json StartSequence(const std::string& a_owner, const json& a_frames, long a_tailMs, const std::string& a_controlToken, const ToolContext& a_ctx)
		{
			json args{
				{ "action", "sequence" },
				{ "owner", a_owner },
				{ "frames", a_frames },
				{ "tailMs", a_tailMs },
			};
			if (!a_controlToken.empty())
				args["controlToken"] = a_controlToken;
			return dvb::HandleVRInput(args, a_ctx);
		}

		void StopSequence(const std::string& a_owner, const std::string& a_token, const ToolContext& a_ctx)
		{
			try {
				json args{
					{ "action", "stop" },
					{ "owner", a_owner },
				};
				if (!a_token.empty())
					args["controlToken"] = a_token;
				const json result = dvb::HandleVRInput(args, a_ctx);
				if (result.value("restorationPending", false))
					dlog::warn("devbench: reach '{}' sequence stop left controller restoration pending", a_owner);
			} catch (const std::exception& e) {
				// The sequence may already have expired on its own; anything else is worth a log line.
				dlog::warn("devbench: reach '{}' sequence stop failed: {}", a_owner, e.what());
			}
		}

		/**
		 * Owns Reach's one sequence: each Start replaces the frames of the sequence it started
		 * before (a fresh one if that has already run out), and Stop or destruction releases it.
		 */
		class SequenceGuard
		{
		public:
			SequenceGuard(const std::string& a_owner, const ToolContext& a_ctx) :
				m_owner(a_owner), m_ctx(a_ctx)
			{
			}

			~SequenceGuard()
			{
				Stop();
			}

			SequenceGuard(const SequenceGuard&) = delete;
			SequenceGuard& operator=(const SequenceGuard&) = delete;

			void Start(const json& a_frames, long a_tailMs)
			{
				const json result = StartSequence(m_owner, a_frames, a_tailMs, m_active ? m_token : std::string{}, m_ctx);
				m_token = TokenOf(result);
				m_active = true;
			}

			void Stop()
			{
				if (!m_active)
					return;
				m_active = false;
				const std::string token = m_token;
				m_token.clear();
				StopSequence(m_owner, token, m_ctx);
			}

		private:
			const std::string& m_owner;
			const ToolContext& m_ctx;
			std::string        m_token;
			bool               m_active = false;
		};

		json Vec3Json(const RL::Vec3& a_v)
		{
			return json{ { "x", a_v.x }, { "y", a_v.y }, { "z", a_v.z } };
		}

		RL::Vec3 Vec3From(const json& a_v)
		{
			return RL::Vec3{ a_v.at("x").get<double>(), a_v.at("y").get<double>(), a_v.at("z").get<double>() };
		}

		double ReadPlayerYaw()
		{
			const json result = dvb::MainThread::RunAndWait([]() -> json {
				return json{ { "yaw", vrhost::PlayerYaw() } };
			});
			return result.at("yaw").get<double>();
		}

		RL::Vec3 ReadNodeWorld(const std::string& a_name, const std::string& a_root = {})
		{
			return Vec3From(dvb::MainThread::RunAndWait([a_name, a_root]() -> json {
				return Vec3Json(vrhost::ReachNodeWorld(a_name, a_root));
			}));
		}

		RL::Vec3 ReadHandWorld(Hand a_hand)
		{
			return Vec3From(dvb::MainThread::RunAndWait([a_hand]() -> json {
				return Vec3Json(vrhost::ReachHandWorld(a_hand));
			}));
		}

		RL::Vec3 WorldTarget(const RL::HandTarget& a_target, const RL::Vec3& a_facing)
		{
			const RL::Vec3 node = ReadNodeWorld(a_target.node, a_target.root);
			return RL::Add(node, RL::Add(RL::Scale(a_facing, a_target.front), a_target.offset));
		}

		std::optional<RL::HandProgress> MeasureHand(const std::optional<RL::HandTarget>& a_target, Hand a_hand, const RL::Vec3& a_facing)
		{
			if (!a_target)
				return std::nullopt;
			const RL::Vec3 hand = ReadHandWorld(a_hand);
			const RL::Vec3 node = ReadNodeWorld(a_target->node, a_target->root);
			const RL::Vec3 target = RL::Add(node, RL::Add(RL::Scale(a_facing, a_target->front), a_target->offset));
			return RL::HandProgress{ hand, node, RL::Sub(target, hand) };
		}

		void SleepChecked(const std::shared_ptr<Run>& a_run, long a_ms)
		{
			long remaining = a_ms;
			while (remaining > 0) {
				if (a_run->stop.load())
					throw StopRequested{};
				const long slice = std::min<long>(remaining, kStopPollMs);
				std::this_thread::sleep_for(std::chrono::milliseconds(slice));
				remaining -= slice;
			}
			if (a_run->stop.load())
				throw StopRequested{};
		}

		void SleepUntil(const std::shared_ptr<Run>& a_run, std::chrono::steady_clock::time_point a_deadline)
		{
			for (;;) {
				if (a_run->stop.load())
					throw StopRequested{};
				const auto now = std::chrono::steady_clock::now();
				if (now >= a_deadline)
					return;
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(a_deadline - now).count();
				const long slice = std::min<long>(static_cast<long>(remaining), kStopPollMs);
				std::this_thread::sleep_for(std::chrono::milliseconds(std::max<long>(slice, 1)));
			}
		}

		RL::Calibration CalibrateHand(Hand a_hand, const RL::Pose& a_hmd, const RL::Vec3& a_rest, const RL::Vec3& a_otherTracking, double a_step, SequenceGuard& a_guard, const std::shared_ptr<Run>& a_run)
		{
			const json frames = RL::CalibrationFrames(a_hand, a_hmd, a_rest, a_otherTracking, a_step);

			// The caller replaces this sequence right after the last sample, while its last leg
			// (and the long tail) still hold the hand, so it never falls back to the controller.
			const auto t0 = std::chrono::steady_clock::now();
			a_guard.Start(frames, kCalibrationTailMs);

			std::array<RL::Vec3, 4> points{};
			for (int i = 0; i < 4; ++i) {
				SleepUntil(a_run, t0 + std::chrono::milliseconds(RL::CalibrationSampleMs(i)));
				points[static_cast<std::size_t>(i)] = ReadHandWorld(a_hand);
			}

			const auto cal = RL::CalibrationFromPoints(points, a_step);
			if (!cal)
				throw ToolError(500, "calibration is singular: the hand did not move along every axis");
			return *cal;
		}

		/**
		 * A hand's last measured calibration and the player yaw it was measured at. Reach reuses
		 * it while the yaw holds, re-measuring only its rest point, and drops it after a run
		 * that did not converge.
		 */
		struct CachedCalibration
		{
			RL::Calibration calibration;
			double          yaw = 0.0;
		};

		std::mutex                       g_calibrationMutex;
		std::optional<CachedCalibration> g_calibration[2];  // by HandIndex

		std::optional<RL::Calibration> ReusableCalibration(Hand a_hand, double a_yaw)
		{
			std::lock_guard lock(g_calibrationMutex);
			const auto&     cached = g_calibration[HandIndex(a_hand)];
			if (cached && RL::CalibrationReusable(cached->yaw, a_yaw))
				return cached->calibration;
			return std::nullopt;
		}

		void StoreCalibration(Hand a_hand, const std::optional<RL::Calibration>& a_calibration, double a_yaw)
		{
			std::lock_guard lock(g_calibrationMutex);
			auto&           slot = g_calibration[HandIndex(a_hand)];
			if (a_calibration)
				slot = CachedCalibration{ *a_calibration, a_yaw };
			else
				slot.reset();
		}

		/**
		 * Holds both hands at rest and returns where the game puts each requested hand once the
		 * pose has settled: the rest point a reused calibration is anchored to.
		 */
		std::pair<RL::Vec3, RL::Vec3> MeasureRest(const RL::Pose& a_hmd, const RestPose& a_rest, SequenceGuard& a_guard, const std::shared_ptr<Run>& a_run)
		{
			const long settle = RL::CalibrationSampleMs(0);
			const auto t0 = std::chrono::steady_clock::now();
			a_guard.Start(RL::ApproachAndHoldFrames(a_hmd, a_rest.left, a_rest.left, a_rest.right, a_rest.right, settle + 1000), kCalibrationTailMs);
			SleepUntil(a_run, t0 + std::chrono::milliseconds(settle));
			return { ReadHandWorld(Hand::Left), ReadHandWorld(Hand::Right) };
		}

		void RunWorker(std::shared_ptr<Run> a_run, RL::ReachRequest a_req, ToolContext a_ctx)
		{
			SequenceGuard    guard(a_run->owner, a_ctx);
			const ActiveFlag activeFlag{ a_run };
			try {
				SetPhase(a_run, RL::Phase::kCalibrating);

				const double   yaw = ReadPlayerYaw();
				const RL::Vec3 facing{ std::sin(yaw), std::cos(yaw), 0.0 };
				const RestPose rest = ReadRestPose();
				const RL::Pose calibHmd = RL::Upright(rest.hmd);

				// A cached calibration is reused while the player faces the same way: one rest
				// sample re-anchors it instead of four legs per hand.
				std::optional<RL::Calibration> calLeft;
				std::optional<RL::Calibration> calRight;
				if (!a_req.recalibrate) {
					if (a_req.left)
						calLeft = ReusableCalibration(Hand::Left, yaw);
					if (a_req.right)
						calRight = ReusableCalibration(Hand::Right, yaw);
				}
				const bool reuseLeft = calLeft.has_value();
				const bool reuseRight = calRight.has_value();
				if (reuseLeft || reuseRight) {
					const auto [restLeft, restRight] = MeasureRest(calibHmd, rest, guard, a_run);
					if (reuseLeft)
						calLeft->rest = restLeft;
					if (reuseRight)
						calRight->rest = restRight;
				}

				// Where calibration leaves the hands: the last measured hand on its +z leg.
				const RL::Vec3 lastLeg{ 0.0, 0.0, kCalibrationStep };
				RL::Vec3       heldLeft = rest.left;
				RL::Vec3       heldRight = rest.right;
				if (a_req.left && !reuseLeft) {
					calLeft = CalibrateHand(Hand::Left, calibHmd, rest.left, rest.right, kCalibrationStep, guard, a_run);
					heldLeft = RL::Add(rest.left, lastLeg);
				}
				if (a_req.right && !reuseRight) {
					calRight = CalibrateHand(Hand::Right, calibHmd, rest.right, rest.left, kCalibrationStep, guard, a_run);
					heldLeft = rest.left;
					heldRight = RL::Add(rest.right, lastLeg);
				}
				{
					std::lock_guard lock(g_mutex);
					if (a_req.left)
						a_run->progress.calibrationLeft = reuseLeft ? "reused" : "measured";
					if (a_req.right)
						a_run->progress.calibrationRight = reuseRight ? "reused" : "measured";
				}
				const auto storeCalibrations = [&](bool a_converged) {
					if (a_req.left)
						StoreCalibration(Hand::Left, a_converged ? calLeft : std::nullopt, yaw);
					if (a_req.right)
						StoreCalibration(Hand::Right, a_converged ? calRight : std::nullopt, yaw);
				};

				RL::Vec3 trackLeft = rest.left;
				RL::Vec3 trackRight = rest.right;
				if (a_req.left)
					trackLeft = RL::TrackingFor(*calLeft, rest.left, WorldTarget(*a_req.left, facing));
				if (a_req.right)
					trackRight = RL::TrackingFor(*calRight, rest.right, WorldTarget(*a_req.right, facing));

				RL::Pose hmd = RL::Upright(rest.hmd);
				if (a_req.head.aim != RL::HeadTarget::Aim::kNone) {
					RL::Vec3 aim{};
					if (a_req.head.aim == RL::HeadTarget::Aim::kHands) {
						if (a_req.left && a_req.right)
							aim = RL::Scale(RL::Add(trackLeft, trackRight), 0.5);
						else if (a_req.right)
							aim = trackRight;
						else
							aim = trackLeft;
					} else {
						const bool             useRight = a_req.right.has_value();
						const RL::Calibration& cal = useRight ? *calRight : *calLeft;
						const RL::Vec3         restHand = useRight ? rest.right : rest.left;
						aim = RL::TrackingFor(cal, restHand, ReadNodeWorld(a_req.head.node));
					}
					RL::Vec3 pos = a_req.head.position ? *a_req.head.position : RL::Add(aim, RL::Vec3{ 0.0, a_req.head.up, a_req.head.back });
					if (a_req.head.eyeHeight)
						pos.y = *a_req.head.eyeHeight;
					hmd = RL::LookAt(pos, aim);
				}

				SetPhase(a_run, RL::Phase::kReaching);

				// Every sequence blends from where the previous one left the hands (heldLeft /
				// heldRight), so nothing snaps; a lead-in carries them to approachUnits behind
				// the targets.
				RL::Vec3       approachLeft = trackLeft;
				RL::Vec3       approachRight = trackRight;
				const RL::Vec3 back = RL::Scale(facing, -a_req.approachUnits);
				if (a_req.left) {
					if (const auto d = RL::TrackingDelta(*calLeft, back))
						approachLeft = RL::Add(trackLeft, *d);
				}
				if (a_req.right) {
					if (const auto d = RL::TrackingDelta(*calRight, back))
						approachRight = RL::Add(trackRight, *d);
				}
				guard.Start(RL::ApproachAndHoldFrames(hmd, heldLeft, approachLeft, heldRight, approachRight, kShortHoldMs), kSequenceTailMs);
				SleepChecked(a_run, RL::kApproachBlendMs);
				heldLeft = approachLeft;
				heldRight = approachRight;

				const int iterations = std::max(1, a_req.iterations);
				for (int attempt = 1; attempt <= iterations; ++attempt) {
					const bool last = (attempt == iterations);
					const long hold = last ? a_req.holdMs : kShortHoldMs;

					guard.Start(RL::ApproachAndHoldFrames(hmd, heldLeft, trackLeft, heldRight, trackRight, hold), kSequenceTailMs);
					heldLeft = trackLeft;
					heldRight = trackRight;
					auto sequenceEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(RL::ApproachAndHoldDurationMs(hold));
					SleepChecked(a_run, kIterationSettleMs);

					const auto left = MeasureHand(a_req.left, Hand::Left, facing);
					const auto right = MeasureHand(a_req.right, Hand::Right, facing);
					double     worst = 0.0;
					if (left)
						worst = std::max(worst, RL::Norm(left->error));
					if (right)
						worst = std::max(worst, RL::Norm(right->error));

					{
						std::lock_guard lock(g_mutex);
						a_run->progress.iteration = attempt;
						a_run->progress.left = left;
						a_run->progress.right = right;
					}

					if (RL::Converged(worst, a_req.toleranceUnits)) {
						storeCalibrations(true);
						if (!last) {
							guard.Start(RL::ApproachAndHoldFrames(hmd, trackLeft, trackLeft, trackRight, trackRight, a_req.holdMs), kSequenceTailMs);
							sequenceEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(RL::ApproachAndHoldDurationMs(a_req.holdMs));
						}
						SetPhase(a_run, RL::Phase::kHolding);
						SleepUntil(a_run, sequenceEnd);
						SetPhase(a_run, RL::Phase::kDone);
						return;
					}

					if (!last) {
						if (left) {
							if (const auto d = RL::TrackingDelta(*calLeft, left->error))
								trackLeft = RL::Add(trackLeft, *d);
						}
						if (right) {
							if (const auto d = RL::TrackingDelta(*calRight, right->error))
								trackRight = RL::Add(trackRight, *d);
						}
						continue;
					}

					storeCalibrations(false);
					SetPhase(a_run, RL::Phase::kHolding);
					SleepUntil(a_run, sequenceEnd);
					SetError(a_run, std::format("did not reach tolerance after {} iterations", iterations));
					SetPhase(a_run, RL::Phase::kDone);
					return;
				}
			} catch (const StopRequested&) {
				guard.Stop();
				SetPhase(a_run, RL::Phase::kStopped);
			} catch (const ToolError& e) {
				guard.Stop();
				SetError(a_run, e.what());
				SetPhase(a_run, RL::Phase::kFailed);
			} catch (const std::exception& e) {
				guard.Stop();
				SetError(a_run, e.what());
				SetPhase(a_run, RL::Phase::kFailed);
			} catch (...) {
				guard.Stop();
				SetError(a_run, "unknown error");
				SetPhase(a_run, RL::Phase::kFailed);
			}
		}
	}

	/// Starts an asynchronous reach run and returns {queued, runId, owner, contract}.
	json Start(const json& a_args, const std::string& a_owner, const ToolContext& a_ctx)
	{
		std::string error;
		const auto  parsed = RL::ParseReach(a_args, error);
		if (!parsed)
			throw ToolError(400, std::format("reach: {}", error));

		const json status = dvb::HandleVRInput(json{ { "action", "status" } }, a_ctx);
		if (!status.value("ready", false))
			throw ToolError(503, "OpenVR is not ready");

		std::shared_ptr<Run> run;
		{
			std::lock_guard lock(g_mutex);
			for (const auto& existing : g_runs) {
				if (existing->active.load())
					throw ToolError(409, "a reach run is already active");
			}
			run = std::make_shared<Run>();
			run->runId = g_nextRunId++;
			run->owner = a_owner;
			g_runs.push_back(run);
			while (g_runs.size() > kMaxRuns)
				g_runs.erase(g_runs.begin());
		}

		const long        runId = run->runId;
		const std::string owner = run->owner;
		ToolContext       ctx = a_ctx;
		try {
			std::thread([run, request = *parsed, ctx]() { RunWorker(run, request, ctx); }).detach();
		} catch (...) {
			run->active.store(false);
			throw;
		}

		return json{
			{ "queued", true },
			{ "runId", runId },
			{ "owner", owner },
			{ "contract", "devbench.input.vrTrackedSet" },
		};
	}

	/// Reports progress for a run (the latest run when 'runId' is omitted).
	json Status(const json& a_args)
	{
		std::optional<long> runId;
		if (a_args.contains("runId") && !a_args["runId"].is_null()) {
			if (!a_args["runId"].is_number_integer())
				throw ToolError(400, "'runId' must be an integer");
			runId = a_args["runId"].get<long>();
		}

		std::lock_guard      lock(g_mutex);
		std::shared_ptr<Run> run;
		if (runId) {
			for (const auto& candidate : g_runs) {
				if (candidate->runId == *runId) {
					run = candidate;
					break;
				}
			}
			if (!run)
				throw ToolError(404, std::format("no reach run with runId {}", *runId));
		} else {
			if (g_runs.empty())
				throw ToolError(404, "no reach run");
			run = g_runs.back();
		}

		json result = RL::ProgressJson(run->progress);
		result["runId"] = run->runId;
		result["owner"] = run->owner;
		const bool active = run->active.load();
		result["active"] = active;
		result["stopping"] = active && run->stop.load();
		return result;
	}

	/// Requests the active run to stop; the owner must match.
	json Stop(const std::string& a_owner)
	{
		std::lock_guard      lock(g_mutex);
		std::shared_ptr<Run> active;
		for (const auto& candidate : g_runs) {
			if (candidate->active.load())
				active = candidate;
		}
		if (!active)
			return json{ { "stopped", false }, { "notActive", true } };
		if (active->owner != a_owner)
			throw ToolError(409, std::format("reach run {} is owned by '{}'", active->runId, active->owner));
		active->stop.store(true);
		return json{
			{ "stopped", true },
			{ "runId", active->runId },
			{ "owner", active->owner },
		};
	}

	std::vector<std::string> Actions()
	{
		return { "reach", "reachStatus", "reachStop" };
	}

	bool IsAction(const std::string& a_action)
	{
		return a_action == "reach" || a_action == "reachStatus" || a_action == "reachStop";
	}

	/**
	 * Resolves the owner as every vrTrackedSet action does, so 'press' over a reach hold matches
	 * the run's owner; malformed JSON answers 400 as the shared actions do.
	 */
	json Handle(const std::string& a_action, const json& a_args, const ToolContext& a_ctx)
	{
		try {
			if (a_action == "reachStatus")
				return Status(a_args);
			const std::string owner = ResolveVRInputOwner(a_args, a_ctx);
			if (a_action == "reach")
				return Start(a_args, owner, a_ctx);
			return Stop(owner);
		} catch (const json::exception& e) {
			throw ToolError(400, std::format("invalid VR tracked-set JSON: {}", e.what()));
		}
	}

	void EndSession()
	{
		{
			std::lock_guard lock(g_mutex);
			for (const auto& candidate : g_runs) {
				if (candidate->active.load())
					candidate->stop.store(true);
			}
		}
		std::lock_guard lock(g_calibrationMutex);
		g_calibration[0].reset();
		g_calibration[1].reset();
	}
}
