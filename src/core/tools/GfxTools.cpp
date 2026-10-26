#include "core/tools/GfxTools.h"

#include "core/GameState.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/ToolRegistry.h"
#include "core/gfx/Device.h"
#include "core/gfx/Format.h"

#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <vector>

namespace dvb::tools
{
	namespace
	{
		// Why stats/dump refuse. A readback is a CopySubresourceRegion + Map on the game's
		// immediate context, issued from a devbench main-thread task. On Fallout 4 VR that
		// hung the game twice (blocking Map, 2026-10-15) and then crashed the NVIDIA driver
		// on the first call (non-blocking Map, 2026-10-26) - True Scopes register TS-031.
		// What is not known is which rule it broke: either those tasks do not run on the
		// thread the renderer drives the context from (every call races it), or they do and
		// the staging texture's final Release on the listener thread was the one
		// cross-thread call, fatal on a single-threaded device. list's `threading` reports
		// the facts that decide it. Until then the safe answer is no answer.
		constexpr const char* kReadbackDisabled =
			"rendertarget: action='stats' and 'dump' are disabled. A readback issues D3D11 copy/map calls on the game's "
			"immediate context from a devbench main-thread task, and on Fallout 4 VR that hung the game twice and then "
			"crashed the GPU driver (TS-031). They return once readback runs where the renderer allows it. "
			"action='list' is unaffected, and its 'threading' field reports what decides the fix.";

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
				// Say up front whether this one can be dumped at all, so a caller does
				// not have to discover it by getting an error per target.
				{ "decodable", gfx::IsSupported(desc.Format) },
			};
			if (const auto name = gfx::TargetName(a_ref.index); !name.empty())
				row["name"] = name;
			return row;
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

			// 501 and not 400: the action exists and is documented, it is switched off.
			if (action == "stats" || action == "dump")
				throw ToolError(501, kReadbackDisabled);

			throw ToolError(400, "rendertarget: unknown action '" + action + "' (list|stats|dump)");
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
				"Look INSIDE the renderer: list the engine's render targets — the G-buffer, the light "
				"accumulation, the shadow map, i.e. the buffers a rendering bug actually lives in. "
				"action='list' → every live target as { index, width, height, format, formatName, "
				"arraySize, sampleCount, decodable }, plus 'threading' { taskThreadId, "
				"taskThreadIdHex, deviceCreationFlags, singleThreadedDevice, multithreadProtected }: "
				"which thread devbench's main-thread tasks run on and whether the D3D11 device "
				"serialises calls from other threads (multithreadProtected null = not reported by "
				"the runtime). action='stats' and action='dump' (CPU readback, BMP dump) are "
				"currently DISABLED and answer 501: on Fallout 4 VR a readback hung the game twice "
				"and then crashed the GPU driver, and they stay off until readback runs where the "
				"renderer allows it.";
			rt.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "stats", "dump" }) }, { "description", "default 'list'; 'stats' and 'dump' are disabled (501)" } } },
									{ "index", json{ { "type", "integer" }, { "description", "stats/dump (disabled): engine render-target index, from action='list'" } } },
									{ "maxDimension", json{ { "type", "integer" }, { "description", "stats/dump (disabled): downsample so the longest side fits; default 1024, 0 = native" } } },
									{ "label", json{ { "type", "string" }, { "description", "dump (disabled): appended to the filename" } } } } }
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
