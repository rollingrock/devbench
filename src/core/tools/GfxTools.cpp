#include "core/tools/GfxTools.h"

#include "core/GameState.h"
#include "core/Host.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/ToolRegistry.h"
#include "core/gfx/Capture.h"
#include "core/gfx/Device.h"
#include "core/gfx/Format.h"

#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// A readback is a CopySubresourceRegion + Map on the game's D3D11 immediate context,
// which the renderer drives from the game's main thread. The calls are safe only from
// that thread. True Scopes register TS-031 is what it cost to learn that: on Fallout 4 VR,
// F4SE ran devbench's "main-thread" tasks on worker threads, and readbacks issued from
// them hung the game twice and then crashed the NVIDIA driver. Two rules follow, and
// both are enforced here, not just assumed:
//   - stats/dump run only where the platform has established that tasks stay on the
//     main thread (MainThread::ConfinedToMainThread), and refuse with 501 elsewhere;
//   - every task that touches D3D checks its own thread first, and refuses rather than
//     calling D3D from anywhere else.
// Every D3D step is logged before and after the call, so if a game ever dies inside
// one, the last line of devbench.log names it.

namespace dvb::tools
{
	namespace
	{
		std::atomic<std::uint64_t> g_dumpSeq{ 0 };
		std::atomic<std::uint64_t> g_readbackSeq{ 0 };

		constexpr const char* kNoMainThread =
			"rendertarget: action='stats' and 'dump' are off on this runtime. A readback must run on the game's "
			"main thread, and devbench has not established that its tasks do here (on Fallout 4 VR, F4SE ran "
			"them on worker threads, and a readback from one crashed the GPU driver - TS-031). action='list' "
			"works, and its 'threading' field says what is known.";

		// Inside a task, before any D3D call. The drain can be installed and a task still
		// end up elsewhere only through a bug, and a bug here crashes a driver.
		void RequireMainThread()
		{
			const auto owner = MainThread::OwnerThreadId();
			if (owner != 0 && owner != ::GetCurrentThreadId())
				throw ToolError(503, std::format("rendertarget: this task ran on thread {}, not the main thread {}; "
												 "refusing to touch D3D from it",
										 ::GetCurrentThreadId(), owner));
		}

		// What decides whether a D3D call from a main-thread task is safe: the thread those
		// tasks run on (compare it with the thread the renderer submits from), and whether
		// the device serialises calls made from any other thread. Read, never changed.
		json ThreadingJson(ID3D11Device* a_device)
		{
			const DWORD tid = ::GetCurrentThreadId();
			const UINT  flags = a_device->GetCreationFlags();
			json        out{
                // Decimal and hex, because a log line ("runs on thread 56820") and cdb ("~")
                // print it differently, and the point is to compare the two.
                { "taskThreadId", tid },
                { "taskThreadIdHex", std::format("0x{:X}", tid) },
                // 0 = the platform does not record it. Compare with taskThreadId: on FO4VR
                // they were found to differ (TS-031), which is why both are reported.
                { "mainThreadId", MainThread::OwnerThreadId() },
                { "tasksOffMainThread", MainThread::OffThreadRuns() },
                { "confinedToMainThread", MainThread::ConfinedToMainThread() },
                { "deviceCreationFlags", std::format("0x{:X}", flags) },
                { "singleThreadedDevice", (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0 },
			};
			// null, not false, when the runtime does not offer the interface: unknown is
			// a different answer from "off".
			out["multithreadProtected"] = nullptr;
			if (auto* ctx = gfx::Context()) {
				ID3D11Multithread* mt = nullptr;
				if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt))) && mt) {
					out["multithreadProtected"] = mt->GetMultithreadProtected() != FALSE;
					mt->Release();
				}
			}
			return out;
		}

		json DescribeTarget(const gfx::TargetRef& a_ref)
		{
			D3D11_TEXTURE2D_DESC desc{};
			a_ref.texture->GetDesc(&desc);
			json row{
				{ "index", a_ref.index },
				{ "width", desc.Width },
				{ "height", desc.Height },
				{ "format", desc.Format },
				{ "formatName", gfx::FormatName(desc.Format) },
				{ "arraySize", desc.ArraySize },
				{ "mipLevels", desc.MipLevels },
				{ "sampleCount", desc.SampleDesc.Count },
				// How the engine created it. Not decoration: a copy out of a SHARED /
				// KEYEDMUTEX resource without its sync is a driver-side fault, and the
				// flags are the only way to know from outside which kind a target is.
				{ "usage", desc.Usage },
				{ "bindFlags", std::format("0x{:X}", desc.BindFlags) },
				{ "miscFlags", std::format("0x{:X}", desc.MiscFlags) },
				{ "cpuAccessFlags", std::format("0x{:X}", desc.CPUAccessFlags) },
				// Say up front whether this one can be dumped at all, so a caller does
				// not have to discover it by getting an error per target.
				{ "decodable", gfx::IsSupported(desc.Format) },
			};
			if (const auto name = gfx::TargetName(a_ref.index); !name.empty())
				row["name"] = name;
			return row;
		}

		gfx::TargetRef FindTarget(std::uint32_t a_index)
		{
			for (const auto& t : gfx::EnumerateTargets()) {
				if (t.index == a_index)
					return t;
			}
			return {};
		}

		json StatsJson(const gfx::Stats& a_st)
		{
			return json{
				{ "pixels", a_st.pixels },
				// nonFinitePct is listed first deliberately — it is the number that
				// gets overlooked, and a NaN buffer displays black while reading
				// "bright" to any exponent test. darkPct EXCLUDES non-finite pixels,
				// so the two never quietly absorb each other.
				{ "nonFinitePct", a_st.nonFinitePct },
				{ "darkPct", a_st.darkPct },
				{ "meanLuma", a_st.meanLuma },
				{ "maxChannel", a_st.maxChannel },
			};
		}

		json RenderTargetHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("list"));

			if (action == "list") {
				// A main-thread task like every engine read. It reads only resource
				// descriptors (GetDesc) and the device's creation flags, never the
				// immediate context, which is why it is safe where a readback is not.
				return MainThread::RunAndWait([]() -> json {
					auto* const device = gfx::Device();
					if (!device)
						throw ToolError(503, "the renderer is not available yet (or could not be resolved on this runtime)");
					json rows = json::array();
					for (const auto& t : gfx::EnumerateTargets())
						rows.push_back(DescribeTarget(t));
					return json{ { "count", rows.size() }, { "targets", std::move(rows) }, { "threading", ThreadingJson(device) } };
				});
			}

			if (action != "stats" && action != "dump")
				throw ToolError(400, "rendertarget: unknown action '" + action + "' (list|stats|dump)");

			// 501, not 400: the action exists, this runtime cannot do it safely.
			if (!MainThread::ConfinedToMainThread())
				throw ToolError(501, kNoMainThread);

			if (!a_args.contains("index"))
				throw ToolError(400, "rendertarget: 'index' is required (see action='list')");
			const auto index = a_args["index"].get<std::uint32_t>();

			// 0 = full resolution. The default trades a little detail for a capture
			// that returns promptly and produces a file you can open, rather than a
			// 25 MB bitmap of a 4K buffer.
			const auto        maxDim = a_args.value("maxDimension", 1024u);
			const bool        wantFile = (action == "dump");
			const std::string label = a_args.value("label", std::string{});

			// Bisecting aid, kept because it is cheap and found TS-031: stop after one step,
			// so a live run can tell which D3D call breaks a game, not only that one does.
			//   create - create the staging texture and release it: no copy, no map
			//   copy   - create, queue the copy, let kSettleFrames frames pass, release: no map
			//   map    - the whole readback (default)
			const std::string stage = a_args.value("stage", std::string("map"));
			if (stage != "create" && stage != "copy" && stage != "map")
				throw ToolError(400, "rendertarget: 'stage' must be create, copy or map");
			if (wantFile && stage != "map")
				throw ToolError(400, "rendertarget: action='dump' needs stage='map'");
			constexpr int kSettleFrames = 10;

			// Every step logs before and after its D3D call, at info, which flushes. If the
			// game dies inside a call, the last line of devbench.log names that call. The
			// first non-blocking build logged nothing, and its crash could not be placed;
			// the build that did log is the one that showed the tasks' threads changing.
			const auto seq = g_readbackSeq.fetch_add(1) + 1;
			const auto step = [seq](std::string_view a_what) {
				dlog::info("rendertarget #{} [thread {} frame {}] {}", seq, ::GetCurrentThreadId(), game::CurrentFrame(), a_what);
			};

			// Two phases, never a wait on the GPU inside the game's frame (gfx::PendingReadback
			// says why): one main-thread task finds the target and queues its copy into
			// devbench's own staging texture; later tasks each try once to map that copy with
			// DO_NOT_WAIT. Analysis and the BMP write then run here on the listener thread:
			// they read only devbench's CPU copy.
			//
			// State the tasks write is heap-held, not captured by reference: RunAndWait can time
			// out while a task is already running, and that task must not then write through a
			// dangling reference.
			struct Shared
			{
				gfx::PendingReadback pending;
				gfx::Surface         surface;
				json                 source;
				std::string          error;
				unsigned long        taskThreadId = 0;
			};
			const auto shared = std::make_shared<Shared>();
			const auto started = std::chrono::steady_clock::now();
			const int  frameAtStart = game::CurrentFrame();
			const auto framesSoFar = [frameAtStart]() {
				const int now = game::CurrentFrame();
				return (frameAtStart < 0 || now < 0) ? -1 : now - frameAtStart;
			};

			// On every way out of this handler, including a throw, the staging texture is
			// released by a main-thread task: the thread that created it. Its deleter refuses
			// any other thread (Capture.h), so without this the texture would leak rather
			// than be released. If this task times out, the one still queued holds the last
			// reference and drops it when it runs, on the main thread.
			struct ReleaseOnTaskThread
			{
				std::shared_ptr<Shared>               shared;
				std::function<void(std::string_view)> step;
				~ReleaseOnTaskThread()
				{
					if (!shared->pending.Started())
						return;
					try {
						MainThread::RunAndWait([s = shared, log = step]() -> json {
							log("Release(staging): calling");
							s->pending.staging.reset();
							log("Release(staging): returned");
							return nullptr;
						});
					} catch (...) {
						// A timeout here is not an error to report: the queued task keeps
						// the texture alive until it runs, on the right thread.
					}
				}
			} release{ shared, step };

			MainThread::RunAndWait([shared, index, stage, step]() -> json {
				if (!gfx::Device())
					throw ToolError(503, "the renderer is not available yet");
				const auto target = FindTarget(index);
				if (!target.texture)
					throw ToolError(404, std::format("no render target with index {} (see action='list')", index));
				shared->source = DescribeTarget(target);
				shared->taskThreadId = ::GetCurrentThreadId();
				RequireMainThread();
				step(std::format("begin: target {} = {}, stage={}", index, shared->source.dump(), stage));
				const bool copy = (stage != "create");
				step(copy ? "CreateTexture2D(STAGING) + CopySubresourceRegion: calling" : "CreateTexture2D(STAGING): calling");
				shared->pending = gfx::BeginReadback(target.texture, shared->error, copy);
				step(shared->pending.Started() ? "begin: returned" : "begin: refused: " + shared->error);
				if (!shared->pending.Started())
					throw ToolError(422, "rendertarget: " + shared->error);
				return nullptr;
			});

			constexpr auto kBudget = std::chrono::milliseconds(5000);
			const auto     remaining = [&]() {
                return std::chrono::duration_cast<std::chrono::milliseconds>(started + kBudget - std::chrono::steady_clock::now());
			};
			int polls = 0;

			if (stage == "copy") {
				// Let the GPU run the copy without ever mapping it: one empty task per drain,
				// which is once per frame. Counted in drains, not frame-counter ticks, so it
				// still ends on a runtime without a frame signal.
				while (polls < kSettleFrames && remaining() > std::chrono::milliseconds(0)) {
					++polls;
					MainThread::RunAndWait([]() -> json { return nullptr; }, remaining());
				}
			} else if (stage == "map") {
				for (;;) {
					if (remaining() <= std::chrono::milliseconds(0))
						throw ToolError(504, std::format(
												 "rendertarget: the GPU had not executed the copy of target {} after {} polls in {} ms ({} frames, -1 = no frame "
												 "signal); nothing was mapped and nothing waited on the GPU. With frames advancing this is unexpected - report it.",
												 index, polls, kBudget.count(), framesSoFar()));
					++polls;
					const json state = MainThread::RunAndWait(
						[shared, maxDim, step, polls]() -> json {
							RequireMainThread();
							step(std::format("Map(READ, DO_NOT_WAIT) poll {}: calling", polls));
							gfx::Surface surface;
							std::string  err;
							switch (gfx::PollReadback(shared->pending, maxDim, surface, err)) {
							case gfx::ReadbackPoll::kPending:
								step(std::format("Map poll {}: WAS_STILL_DRAWING", polls));
								return "pending";
							case gfx::ReadbackPoll::kReady:
								step(std::format("Map poll {}: mapped, copied {}x{}, unmapped", polls, surface.width, surface.height));
								shared->surface = std::move(surface);
								return "ready";
							default:
								step(std::format("Map poll {}: failed: {}", polls, err));
								shared->error = std::move(err);
								return "failed";
							}
						},
						remaining());
					if (state == "ready")
						break;
					if (state == "failed")
						throw ToolError(500, "rendertarget: " + shared->error);
				}
			}

			// How the copy came back, so a caller can see it did not stall the game. The
			// thread id is the one the main-thread tasks ran on, in a debugger's terms.
			json readback{
				{ "seq", seq },
				{ "stage", stage },
				{ "method", "copy queued in one main-thread task, mapped with DO_NOT_WAIT in later ones" },
				{ "polls", polls },
				{ "ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() },
				{ "frames", framesSoFar() },
				{ "taskThreadId", std::format("0x{:X}", shared->taskThreadId) },
			};
			json out{
				{ "index", index },
				{ "source", shared->source },
				{ "readback", std::move(readback) },
			};
			if (stage != "map")
				return out;  // the release task runs as `release` goes out of scope

			const auto& surface = shared->surface;
			out["captured"] = json{ { "width", surface.width }, { "height", surface.height } };
			out["stats"] = StatsJson(gfx::Analyse(surface));
			if (wantFile) {
				const auto  n = g_dumpSeq.fetch_add(1) + 1;
				const auto  name = label.empty() ?
				                       std::format("rt{:03}_{:04}.bmp", index, n) :
				                       std::format("rt{:03}_{:04}_{}.bmp", index, n, label);
				const auto  path = host::DataDir() / "captures" / name;
				std::string err;
				if (!gfx::WriteBMP(path, surface, err))
					throw ToolError(500, "rendertarget: " + err);
				out["file"] = Utf8(path);
			}
			return out;
		}

		// ---------------------------------------------------------------- measure

		// Frame-time statistics with NO engine hook, by watching the frame counter the
		// platform already exposes and timestamping each change with QPC.
		//
		// The honest caveat, reported in every result: resolution is bounded by the
		// sampling interval, not by the engine. The sampler spins (with a pause
		// instruction, not a sleep) for the duration of the window, so it is accurate
		// to tens of microseconds but costs one busy core while it runs. That is an
		// acceptable trade for an explicitly-requested measurement and an unacceptable
		// one for a background service — which is why there is no background mode.
		json MeasureHandler(const json& a_args, const ToolContext&)
		{
			const auto durationMs = std::clamp<std::int64_t>(a_args.value("durationMs", 3000), 200, 60000);

			if (game::CurrentFrame() < 0)
				throw ToolError(503,
					"measure: this runtime has no frame counter available, so frame timing cannot be sampled "
					"(inspect kind='health' reports frame: -1)");

			std::vector<double> frames;  // ms
			frames.reserve(4096);

			const auto qpcFreq = []() { LARGE_INTEGER f{}; ::QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
			const auto now = []() { LARGE_INTEGER c{}; ::QueryPerformanceCounter(&c); return c.QuadPart; };
			const auto start = now();
			const auto deadline = start + static_cast<std::int64_t>(qpcFreq * static_cast<double>(durationMs) / 1000.0);

			int          lastFrame = game::CurrentFrame();
			std::int64_t lastTick = start;
			std::int64_t stalledFrames = 0;
			// The window opens partway through a frame, so the interval from `start` to
			// the FIRST transition is a fraction of a frame, not a frame. Recording it
			// would drag minMs and p50 down by an amount that depends on nothing but
			// when the request happened to arrive. Discard it and start timing from the
			// first real boundary.
			bool haveBoundary = false;

			while (now() < deadline) {
				const int f = game::CurrentFrame();
				if (f != lastFrame) {
					const auto t = now();
					// A counter that jumps by more than one means we missed frames —
					// record the interval but attribute it honestly rather than
					// dividing it up as if we had seen each one.
					if (f > lastFrame + 1)
						stalledFrames += (f - lastFrame - 1);
					if (haveBoundary)
						frames.push_back(static_cast<double>(t - lastTick) * 1000.0 / qpcFreq);
					haveBoundary = true;
					lastFrame = f;
					lastTick = t;
				} else {
					YieldProcessor();
				}
			}

			if (frames.size() < 2)
				throw ToolError(409,
					std::format("measure: only {} frame transitions in {}ms — is the game paused or minimised?",
						frames.size(), durationMs));

			std::sort(frames.begin(), frames.end());
			const auto pct = [&](double p) {
				const auto i = static_cast<std::size_t>(p * static_cast<double>(frames.size() - 1) + 0.5);
				return frames[i];
			};
			double sum = 0.0;
			for (const auto v : frames)
				sum += v;
			const double mean = sum / static_cast<double>(frames.size());

			return json{
				{ "durationMs", durationMs },
				{ "frames", frames.size() },
				{ "fps", 1000.0 / mean },
				{ "meanMs", mean },
				{ "minMs", frames.front() },
				{ "p50Ms", pct(0.50) },
				{ "p95Ms", pct(0.95) },
				{ "p99Ms", pct(0.99) },
				{ "maxMs", frames.back() },
				// Not decoration: a caller comparing two runs needs to know whether the
				// instrument missed transitions, and what its resolution actually was.
				{ "missedTransitions", stalledFrames },
				{ "method", "frame-counter polling (no engine hook); resolution ~tens of microseconds, one core busy for the window" }
			};
		}
	}

	void RegisterGfxTools(ToolRegistry& a_registry, EventBus&)
	{
		{
			ToolDescriptor rt;
			rt.name = "rendertarget";
			rt.description =
				"Look INSIDE the renderer: list the engine's render targets, measure one, or dump it to "
				"an image. This is what a screenshot cannot give you — the G-buffer, the light "
				"accumulation, the shadow map, i.e. the buffers a rendering bug actually lives in. "
				"action='list' → every live target as { index, width, height, format, formatName, "
				"arraySize, mipLevels, sampleCount, usage, bindFlags, miscFlags, cpuAccessFlags, "
				"decodable }, plus 'threading' { taskThreadId, taskThreadIdHex, mainThreadId, "
				"tasksOffMainThread, confinedToMainThread, deviceCreationFlags, singleThreadedDevice, "
				"multithreadProtected }. action='stats' → copies target 'index' back to the CPU and "
				"returns { nonFinitePct, darkPct, meanLuma, maxChannel } WITHOUT writing a file; "
				"action='dump' → the same plus a 24-bit BMP under "
				"Data/<extender>/Plugins/devbench/captures (optional 'label' in the filename). Both "
				"run only where devbench's tasks are established to stay on the game's main thread "
				"(threading.confinedToMainThread), and answer 501 elsewhere: a readback from another "
				"thread crashed the GPU driver on Fallout 4 VR. The copy is queued in one frame and "
				"mapped without waiting in a later one, so the game never stalls on the GPU; "
				"'readback' in the result says how it went { seq, stage, polls, ms, frames, "
				"taskThreadId }, and every D3D step is logged to devbench.log. 'maxDimension' "
				"(default 1024, 0 = native) integer-downsamples by nearest sample, never averaging, "
				"because averaging hides the single-pixel artefact you are hunting. HDR is "
				"Reinhard-tone-mapped and NaN/Inf pixels are painted MAGENTA rather than clamped. Read "
				"nonFinitePct before darkPct: NaN displays black but reads as BRIGHT to any exponent "
				"test. 'stage' (create | copy | map, default map) stops a stats call after that step, "
				"to bisect a failure.";
			rt.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "stats", "dump" }) }, { "description", "default 'list'" } } },
									{ "index", json{ { "type", "integer" }, { "description", "stats/dump: engine render-target index, from action='list'" } } },
									{ "maxDimension", json{ { "type", "integer" }, { "description", "stats/dump: downsample so the longest side fits; default 1024, 0 = native" } } },
									{ "label", json{ { "type", "string" }, { "description", "dump: appended to the filename" } } },
									{ "stage", json{ { "type", "string" }, { "enum", json::array({ "create", "copy", "map" }) }, { "description", "stats: stop after this step, to bisect a failure; default 'map' (the whole readback)" } } } } }
			};
			a_registry.Register(std::move(rt), &RenderTargetHandler);
		}

		{
			ToolDescriptor measure;
			measure.name = "measure";
			measure.description =
				"Sample frame time over a window and return { fps, meanMs, minMs, p50Ms, p95Ms, "
				"p99Ms, maxMs, frames, missedTransitions }. The benchmark primitive: pair it with "
				"`record`/`replay` so an A/B measures the change and not your hands. "
				"Takes NO engine hook — it watches the engine's own frame counter and timestamps "
				"each change with QPC, so it works on any game whose platform exposes that counter "
				"(a 503 says so plainly if it does not). Resolution is tens of microseconds and it "
				"busies one core for the window, so it is deliberately foreground-only: there is no "
				"background mode. 'missedTransitions' is reported because a caller comparing two "
				"runs has to know whether the instrument itself dropped frames.";
			measure.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "durationMs", json{ { "type", "integer" }, { "description", "sampling window, 200..60000, default 3000" } } } } }
			};
			measure.readOnly = true;
			a_registry.Register(std::move(measure), &MeasureHandler);
		}
	}
}
