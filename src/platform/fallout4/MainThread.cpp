#include "core/MainThread.h"

#include "core/GameState.h"
#include "core/Log.h"
#include "core/MainThreadTask.h"
#include "core/ToolRegistry.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
	// Liveness markers read off the listener thread (via MainThread::LastCompletedFrame /
	// PendingTasks) so /api/health can tell "task queue draining" from "starved" without a
	// RunAndWait round-trip: g_lastTaskFrame is the engine frame when the most recent queued
	// task finished ON the main thread; g_pendingTasks is queued-but-not-yet-finished.
	std::atomic<int> g_lastTaskFrame{ -1 };
	std::atomic<int> g_pendingTasks{ 0 };

	// Which thread a task actually ran on, against the one it is meant for.
	//
	// MEASURED, 2026-10-28, FO4VR 1.2.72 (TS-031): F4SE VR's AddTask queue is drained by a
	// detour on the ENTRY of MessageQueueProcessTask, and worker threads call that function
	// too. Seven consecutive devbench tasks ran on six different threads; the D3D copies
	// they issued from worker threads crashed the NVIDIA driver. So "a task runs on the
	// main thread" is a claim to check, not to assume. g_owner is the thread F4SE loaded
	// the plugin on, which is the game's main thread.
	std::atomic<std::uint32_t> g_owner{ 0 };
	std::atomic<std::uint64_t> g_offThreadRuns{ 0 };

	// devbench's own main-thread queue, drained right after Main::OnIdle calls Main::Swap.
	//
	// Neither F4SE VR queue stays on the main thread. Measured 2026-10-28, FO4VR 1.2.72,
	// against cdb's thread 0: through AddTask, seven tasks ran on six threads; through
	// AddUITask, all 43 tasks ran on six worker threads and not one on thread 0. F4SE VR
	// drains AddTask from a detour on the ENTRY of MessageQueueProcessTask, so on every
	// thread that calls it.
	//
	// The first drain point tried, OnIdle's own call of MessageQueueProcessTask
	// (+0xD845E3), ran every task on thread 0 at the main menu and none at all in game:
	// that call is skipped during play (a console call after a load waited 300 frames and
	// timed out). Main::Swap is called from OnIdle every frame in both (thread 0's stack
	// holds OnIdle's return address 0xD844C1 at the menu and in game), and draws and
	// submits the frame, so draining after it runs tasks on thread 0 between frames.
	//
	// Exact-executable address, FO4VR 1.2.72 only (static scan and disassembly of
	// Fallout4VR.exe: `mov rcx, rdi; call Main::Swap` at 0xD844BC, Swap = 0xD831F0, which
	// takes rcx only and whose result the caller ignores). Checked before writing: it must
	// still be a 5-byte call, or nothing is patched. Another runtime, or a mismatch, keeps
	// F4SE's AddTask, and OffThreadRuns() then shows what that costs. The call is skipped
	// in some states (a `test al,al; jne` guards it); tasks then wait for the next frame
	// that draws.
	constexpr std::uintptr_t kVrOnIdleSwapCall = 0xD844BC;
	constexpr std::uintptr_t kVrMainSwap = 0xD831F0;

	// All four integer argument registers, passed through unchanged: Swap reads rcx only,
	// but forwarding the rest costs nothing and depends on nothing.
	using Swap_t = void (*)(void*, void*, void*, void*);
	Swap_t            g_swapOriginal = nullptr;
	std::atomic<bool> g_ownQueue{ false };

	std::mutex                         g_queueLock;
	std::vector<std::function<void()>> g_queue;

	// Replaces OnIdle's call: Swap first (the frame, and anything chained to this call),
	// then devbench's tasks. Tasks queued while these run wait for the next frame, so a
	// task that queues another cannot spin this loop.
	void SwapHook(void* a_main, void* a_2, void* a_3, void* a_4)
	{
		g_swapOriginal(a_main, a_2, a_3, a_4);
		std::vector<std::function<void()>> batch;
		{
			const std::scoped_lock lock(g_queueLock);
			batch.swap(g_queue);
		}
		for (auto& fn : batch)
			fn();
	}

	void CheckThread()
	{
		const auto owner = g_owner.load(std::memory_order_relaxed);
		const auto here = static_cast<std::uint32_t>(::GetCurrentThreadId());
		if (owner == 0 || here == owner)
			return;
		const auto n = g_offThreadRuns.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n <= 20 || n % 100 == 0)
			dvb::dlog::warn("devbench: a main-thread task ran on thread {}, not the main thread {} ({} so far)", here, owner, n);
	}
}

namespace dvb::platform
{
	// F4SEPlugin_Load runs on the game's main thread (measured: it is cdb's thread 0);
	// call it there, once, before the game loop starts.
	void MarkMainThread()
	{
		g_owner.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
		dvb::dlog::info("devbench: main thread is {} (the thread F4SE loaded the plugin on)", ::GetCurrentThreadId());
	}

	// Installs the post-Swap drain (see kVrOnIdleSwapCall). From F4SEPlugin_Load, after
	// F4SE::Init. Leaves AddTask in place, and says why, whenever it does not patch.
	void InstallMainThreadQueue()
	{
		const auto& module = REL::Module::get();
		if (!REL::Module::IsVR() || module.version() != REL::Version(1, 2, 72, 0)) {
			dvb::dlog::warn(
				"devbench: no main-thread drain for this runtime; tasks go through F4SE's AddTask, "
				"which may run them off the main thread (rendertarget list reports tasksOffMainThread)");
			return;
		}
		const auto  site = module.base() + kVrOnIdleSwapCall;
		const auto* code = reinterpret_cast<const std::uint8_t*>(site);
		if (code[0] != 0xE8) {
			dvb::dlog::warn("devbench: Main::OnIdle+{:#x} is not a call (byte {:#04x}); not patched, tasks go through AddTask",
				kVrOnIdleSwapCall, code[0]);
			return;
		}
		std::int32_t rel = 0;
		std::memcpy(&rel, code + 1, sizeof(rel));
		const auto target = site + 5 + static_cast<std::intptr_t>(rel);
		// A different target means another plugin hooked this call first. Chaining to it
		// is still correct (it ends in Main::Swap), so say so and go on.
		if (target != module.base() + kVrMainSwap)
			dvb::dlog::info("devbench: OnIdle's Swap call already points at {:#x}, not Main::Swap; chaining", target);

		F4SE::AllocTrampoline(64);
		g_swapOriginal = reinterpret_cast<Swap_t>(
			F4SE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&SwapHook)));
		g_ownQueue.store(true, std::memory_order_release);
		dvb::dlog::info("devbench: main-thread tasks drain after Main::Swap (OnIdle call at +{:#x})", kVrOnIdleSwapCall);
	}
}

namespace
{
	// Onto devbench's own queue when the drain is installed, otherwise F4SE's AddTask.
	// False only when neither exists.
	bool Enqueue(std::function<void()> a_fn)
	{
		if (g_ownQueue.load(std::memory_order_acquire)) {
			const std::scoped_lock lock(g_queueLock);
			g_queue.push_back(std::move(a_fn));
			return true;
		}
		auto* task = F4SE::GetTaskInterface();
		if (!task)
			return false;
		task->AddTask(std::move(a_fn));
		return true;
	}
}

namespace dvb::MainThread
{
	std::uint32_t OwnerThreadId() { return g_owner.load(std::memory_order_relaxed); }
	std::uint64_t OffThreadRuns() { return g_offThreadRuns.load(std::memory_order_relaxed); }
	bool          ConfinedToMainThread() { return g_ownQueue.load(std::memory_order_acquire); }

	bool Post(std::function<void()> a_fn)
	{
		return Enqueue([fn = std::move(a_fn)]() {
			CheckThread();
			// Nothing above this frame on the main thread would catch it.
			try {
				fn();
			} catch (const std::exception& e) {
				dvb::dlog::error("devbench: a posted main-thread task threw: {}", e.what());
			} catch (...) {
				dvb::dlog::error("devbench: a posted main-thread task threw a non-standard exception");
			}
		});
	}

	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds a_timeout, const std::atomic<bool>* a_keepWaiting)
	{
		if (!g_ownQueue.load(std::memory_order_acquire) && !F4SE::GetTaskInterface())
			throw ToolError(500, "F4SE TaskInterface unavailable");
		if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed))
			return json(nullptr);

		const auto deadline = QueuedTask::Clock::now() + a_timeout;
		auto       invocation = std::make_shared<QueuedTask>(std::move(a_fn), deadline);
		auto       future = invocation->GetFuture();

		g_pendingTasks.fetch_add(1, std::memory_order_relaxed);
		auto run = [invocation]() {
			// Stamp completion + drop the pending count whether fn() returns or throws — a
			// routine ToolError must not leave the markers looking like a stalled queue.
			struct Finally
			{
				~Finally()
				{
					g_lastTaskFrame.store(game::CurrentFrame(), std::memory_order_relaxed);
					g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
				}
			} finally;
			CheckThread();
			invocation->Run();
		};
		if (!Enqueue(std::move(run))) {
			g_pendingTasks.fetch_sub(1, std::memory_order_relaxed);
			throw ToolError(500, "F4SE TaskInterface unavailable");
		}

		// Slice the wait so a caller that withdraws interest (a_keepWaiting clears) doesn't
		// block the full timeout — e.g. the recorder stopping while the main thread is stalled
		// mid-load. Abandon prevents a queued mutation from running after this call returns;
		// a mutation that already started cannot be interrupted.
		const int      frameAtStart = game::CurrentFrame();
		constexpr auto kSlice = std::chrono::milliseconds(100);
		while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			if (a_keepWaiting && !a_keepWaiting->load(std::memory_order_relaxed)) {
				invocation->Abandon();
				if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
					return future.get();
				return json(nullptr);
			}
			const auto now = QueuedTask::Clock::now();
			if (now >= deadline)
				break;
			future.wait_until(std::min(deadline, now + kSlice));
		}
		if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			const bool abandoned = invocation->Abandon();
			if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				return future.get();
			if (!abandoned)
				throw TaskTimeout(true, std::format("main-thread task did not finish within {}ms; it already started and may still complete", a_timeout.count()));
			// The engine's frame counter discriminates the two 504 causes: still
			// advancing = main thread busy; frozen = hung OR
			// fully paused (the counter also freezes in pause menus).
			const int frameNow = game::CurrentFrame();
			if (frameAtStart < 0 || frameNow < 0)
				throw TaskTimeout(false, std::format("main-thread task did not start within {}ms; queued task abandoned", a_timeout.count()));
			if (frameNow == frameAtStart)
				throw TaskTimeout(false, std::format(
											 "main-thread task did not start within {}ms; queued task abandoned and the game frame counter has not advanced -- main thread hung or the game is fully paused; if no pause menu is open, only a process restart recovers",
											 a_timeout.count()));
			throw TaskTimeout(false, std::format(
										 "main-thread task did not start within {}ms; queued task abandoned ({} frames elapsed -- main thread busy)",
										 a_timeout.count(), frameNow - frameAtStart));
		}

		return future.get();  // rethrows the handler's exception on the listener thread
	}

	int LastCompletedFrame()
	{
		return g_lastTaskFrame.load(std::memory_order_relaxed);
	}

	int PendingTasks()
	{
		return g_pendingTasks.load(std::memory_order_relaxed);
	}
}
