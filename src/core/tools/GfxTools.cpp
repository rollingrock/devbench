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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <vector>

namespace dvb::tools
{
	namespace
	{
		std::atomic<std::uint64_t> g_dumpSeq{ 0 };

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
				// Everything here is a D3D call, so it belongs on the main thread even
				// though it only reads descriptors.
				return MainThread::RunAndWait([]() -> json {
					if (!gfx::Device())
						throw ToolError(503, "the renderer is not available yet (or could not be resolved on this runtime)");
					json rows = json::array();
					for (const auto& t : gfx::EnumerateTargets())
						rows.push_back(DescribeTarget(t));
					return json{ { "count", rows.size() }, { "targets", std::move(rows) } };
				});
			}

			if (action != "stats" && action != "dump")
				throw ToolError(400, "rendertarget: unknown action '" + action + "' (list|stats|dump)");

			if (!a_args.contains("index"))
				throw ToolError(400, "rendertarget: 'index' is required (see action='list')");
			const auto index = a_args["index"].get<std::uint32_t>();

			// 0 = full resolution. The default trades a little detail for a capture
			// that returns promptly and produces a file you can open, rather than a
			// 25 MB bitmap of a 4K buffer.
			const auto maxDim = a_args.value("maxDimension", 1024u);
			const bool wantFile = (action == "dump");
			const std::string label = a_args.value("label", std::string{});

			// One main-thread task does the copy, the analysis and the file write.
			// Splitting them would let the engine recreate the target between steps.
			return MainThread::RunAndWait(
				[index, maxDim, wantFile, label]() -> json {
					if (!gfx::Device())
						throw ToolError(503, "the renderer is not available yet");

					const auto target = FindTarget(index);
					if (!target.texture)
						throw ToolError(404, std::format("no render target with index {} (see action='list')", index));

					const json desc = DescribeTarget(target);

					std::string err;
					const auto  surface = gfx::Readback(target.texture, maxDim, err);
					if (!surface.Valid())
						throw ToolError(422, "rendertarget: " + err);

					json out{
						{ "index", index },
						{ "source", desc },
						{ "captured", json{ { "width", surface.width }, { "height", surface.height } } },
						{ "stats", StatsJson(gfx::Analyse(surface)) },
					};

					if (wantFile) {
						const auto seq = g_dumpSeq.fetch_add(1) + 1;
						const auto name = label.empty() ?
						                      std::format("rt{:03}_{:04}.bmp", index, seq) :
						                      std::format("rt{:03}_{:04}_{}.bmp", index, seq, label);
						const auto path = host::DataDir() / "captures" / name;
						if (!gfx::WriteBMP(path, surface, err))
							throw ToolError(500, "rendertarget: " + err);
						out["file"] = path.string();
					}
					return out;
				},
				// A full-surface readback stalls on the GPU; the default 5 s task
				// timeout is not always enough for a large target on a busy frame.
				std::chrono::milliseconds(15000));
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

			const auto  qpcFreq = []() { LARGE_INTEGER f{}; ::QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
			const auto  now = []() { LARGE_INTEGER c{}; ::QueryPerformanceCounter(&c); return c.QuadPart; };
			const auto  start = now();
			const auto  deadline = start + static_cast<std::int64_t>(qpcFreq * static_cast<double>(durationMs) / 1000.0);

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
				"Look INSIDE the renderer: list the engine's render targets, measure one, or dump it "
				"to an image. This is what a screenshot cannot give you — the G-buffer, the light "
				"accumulation, the shadow map, i.e. the buffers a rendering bug actually lives in. "
				"action='list' → every live target as { index, width, height, format, formatName, "
				"arraySize, sampleCount, decodable }. action='stats' → copies target 'index' back to "
				"the CPU and returns { nonFinitePct, darkPct, meanLuma, maxChannel } WITHOUT writing "
				"a file. action='dump' → the same plus a 24-bit BMP under "
				"Data/<extender>/Plugins/devbench/captures (optional 'label' in the filename). "
				"'maxDimension' (default 1024, 0 = native) integer-downsamples so a 4K buffer stays "
				"openable; sampling is nearest-neighbour, not averaged, because averaging hides the "
				"single-pixel artefact you are hunting. "
				"HDR is Reinhard-tone-mapped, and NaN/Inf pixels are painted MAGENTA rather than "
				"clamped — clamping makes a NaN-filled buffer indistinguishable from a legitimately "
				"overbright one. Read nonFinitePct before darkPct: NaN displays black but reads as "
				"BRIGHT to any exponent test, so 'darkPct: 0' alone has historically meant 'not "
				"measuring the problem'. Runs on the main thread and stalls a frame — an instrument, "
				"not something to poll.";
			rt.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "stats", "dump" }) }, { "description", "default 'list'" } } },
									{ "index", json{ { "type", "integer" }, { "description", "engine render-target index, from action='list'" } } },
									{ "maxDimension", json{ { "type", "integer" }, { "description", "downsample so the longest side fits; default 1024, 0 = native" } } },
									{ "label", json{ { "type", "string" }, { "description", "dump: appended to the filename" } } } } }
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
