#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

#include "core/Json.h"
#include "core/ToolRegistry.h"

namespace dvb::MainThread
{
	struct TaskTimeout : ToolError
	{
		bool started;
		TaskTimeout(bool a_started, const std::string& a_message) :
			ToolError(504, a_message), started(a_started) {}
	};

	/// Run `a_fn` on the main game thread (via SKSE's TaskInterface) and block the
	/// calling (listener) thread until it returns, propagating its JSON result — or
	/// rethrowing whatever it threw (e.g. a dvb::ToolError) on the caller's thread.
	///
	/// This is the value-returning primitive that lets tools read live game/render
	/// state synchronously (the agentic-renderdoc "Eval returns a value" model)
	/// rather than fire-and-forget + poll. Throws TaskTimeout (HTTP 504) if the task
	/// does not finish within `a_timeout` (e.g. the main thread is stalled mid-load).
	/// Tasks still queued at their deadline are abandoned and will not invoke a_fn.
	/// Already-running tasks cannot be interrupted and may finish after a timeout.
	///
	/// MUST be called from a non-main thread (the server listener). Calling it on the
	/// main thread would deadlock — the task can never run while this blocks.
	///
	/// a_keepWaiting: optional liveness flag. If it clears mid-wait, RunAndWait returns
	/// json(nullptr) at once (the caller stopped waiting, e.g. the recorder shutting down)
	/// instead of blocking the full timeout; a task that has not started is abandoned.
	/// An already completed result or exception is still returned; running tasks may finish.
	json RunAndWait(std::function<json()> a_fn,
		std::chrono::milliseconds         a_timeout = std::chrono::milliseconds(5000),
		const std::atomic<bool>*          a_keepWaiting = nullptr);

	/// Engine frame at which the most recently queued main-thread task finished (success or
	/// throw), or -1 if none has run yet. Lock-free; safe to call from the listener thread.
	/// Paired with PendingTasks() it distinguishes a busy-but-draining queue from a starved
	/// one: PendingTasks() > 0 while this stops advancing = tasks aren't completing.
	int LastCompletedFrame();

	/// Main-thread tasks queued via RunAndWait but not yet finished. Lock-free; listener-safe.
	int PendingTasks();

	/// The OS thread tasks are meant to run on (the game's main thread), or 0 when the
	/// platform does not record it. Listener-safe.
	std::uint32_t OwnerThreadId();

	/// How many tasks have run on some thread OTHER than OwnerThreadId() since load. Not
	/// hypothetical: on Fallout 4 VR, F4SE's AddTask queue drained on several worker
	/// threads, and D3D calls made from those tasks crashed the GPU driver (TS-031). 0 when
	/// the platform does not record it. Listener-safe.
	std::uint64_t OffThreadRuns();

	/// True only when the platform has established that every task runs on the game's
	/// main thread: on Fallout 4 VR 1.2.72, devbench's own drain after Main::Swap is
	/// installed. False means "not established", not "known bad". Anything that must
	/// not run on another thread (D3D calls on the immediate context) checks this.
	/// Listener-safe.
	bool ConfinedToMainThread();

	/// Queue a_fn to run once on the main thread and return at once, without waiting.
	/// Unlike RunAndWait it is safe from any thread, the main thread included (a_fn
	/// then runs on a later frame). Returns false, and a_fn is dropped, when the platform
	/// cannot queue at all.
	bool Post(std::function<void()> a_fn);
}
