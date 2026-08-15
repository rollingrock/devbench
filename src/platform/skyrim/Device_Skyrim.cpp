// The Skyrim graphics seam: hand the core the D3D11 device and the renderer's render
// targets. Everything downstream (readback, decode, NaN analysis, dumps) is game-agnostic
// and lives in src/core/gfx/.
//
// This is the twin of Device_Fallout4.cpp, and it is much shorter for one reason:
// CommonLibSSE-NG already models BSGraphics::RendererData as a TYPED struct with
// per-runtime layouts (SE / AE / AE-1130 / VR) selected for us. So there are no hand-held
// offsets here to get wrong. Fallout needs them because the VR address library does not
// cover its renderer id; Skyrim does not.
//
// ⚠ THE STRUCT COMMENTS IN Renderer.h ARE STALE — they say device @0x50 / context @0x58,
// while the header's own static_assert says offsetof(RendererData, context) == 0x40.
// Reading through the typed members is what makes that irrelevant: the compiler applies
// whichever layout the build selected. Do not "simplify" this into raw offsets.
//
// Everything is still validated before use. A typed pointer is not a live pointer: the
// renderer does not exist yet at plugin load, and the engine frees and recreates targets
// on a resolution change, so a slot can be stale. gfx::PlausibleTexture is the gate, and
// it is shared with Fallout rather than copied.

#include "core/gfx/Device.h"

#include "core/Log.h"
#include "core/gfx/Validate.h"

#include <d3d11.h>

namespace
{
	// The engine's own render-target indices ARE RE::RENDER_TARGET values on Skyrim —
	// unlike Fallout, where the renderer is addressed by a logical id through a remap
	// table. That is why TargetName() below can answer and Fallout's cannot.
	constexpr std::uint32_t kFlatTargetCount = RE::RENDER_TARGET::kTOTAL;    // 114 in this header
	constexpr std::uint32_t kVRTargetCount = RE::RENDER_TARGET::kVRTOTAL;    // 125

	// Generated from RE/B/BSShaderRenderTargets.h; index == RENDER_TARGET value.
	// Entries 114..124 exist only on VR.
	constexpr const char* kTargetNames[] = {
#include "RenderTargetNames.inl"
	};
	static_assert(std::size(kTargetNames) == kVRTargetCount,
		"the generated name table must cover every RENDER_TARGET slot, or TargetName() "
		"would silently shift and label a buffer with its neighbour's name");

	struct Resolved
	{
		ID3D11Device*                  device = nullptr;
		ID3D11DeviceContext*           context = nullptr;
		RE::BSGraphics::RenderTargetData* targets = nullptr;
		std::uint32_t                  count = 0;
		bool                           tried = false;
		bool                           ok = false;
	};

	Resolved& State()
	{
		static Resolved s;
		return s;
	}

	// Resolve once, lazily. Deliberately NOT at plugin load: the renderer does not exist
	// then, and an early failure that cached "unavailable" would leave the graphics tools
	// dead for the whole session.
	Resolved& Resolve()
	{
		auto& s = State();
		if (s.tried)
			return s;

		auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
		if (!data)
			return s;  // not up yet — do NOT latch

		// Both from the same object, so they cannot disagree about which renderer they
		// describe. (Renderer::GetDevice() reads a separate global; using it here would
		// mean the device and the context came from two different resolutions.)
		auto* device = reinterpret_cast<ID3D11Device*>(data->forwarder);
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		if (!dvb::gfx::LooksLikeCOM(device) || !dvb::gfx::LooksLikeCOM(context))
			return s;  // renderer allocated but not initialised yet — still do not latch

		// VR's array really is longer (the extra slots are the HMD framebuffer and the
		// world-space UI targets). In a non-EXCLUSIVE NG build the struct is declared at
		// the flat size, so the tail is addressed by stride off the base rather than by
		// subscript — the underlying object on VR genuinely is that large.
		const std::uint32_t count = REL::Module::IsVR() ? kVRTargetCount : kFlatTargetCount;

		// Sample before believing. If the layout were wrong we would be handing D3D a
		// list of arbitrary qwords; requiring several slots to be real textures makes
		// that essentially impossible to pass by accident.
		auto* const   targets = data->renderTargets;
		std::uint32_t valid = 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			auto* tex = reinterpret_cast<ID3D11Texture2D*>(targets[i].texture);
			if (tex && dvb::gfx::PlausibleTexture(tex))
				++valid;
		}
		if (valid < 4) {
			s.tried = true;  // the device was fine, so this IS a layout problem — latch it
			dvb::dlog::warn(
				"gfx: renderer resolved but only {} of {} target slots look like textures — "
				"refusing to use a layout that does not check out",
				valid, count);
			return s;
		}

		s.device = device;
		s.context = context;
		s.targets = targets;
		s.count = count;
		s.ok = true;
		s.tried = true;
		dvb::dlog::info("gfx: renderer resolved ({} live render targets of {})", valid, count);
		return s;
	}
}

namespace dvb::gfx
{
	ID3D11Device* Device()
	{
		const auto& s = Resolve();
		return s.ok ? s.device : nullptr;
	}

	ID3D11DeviceContext* Context()
	{
		const auto& s = Resolve();
		return s.ok ? s.context : nullptr;
	}

	std::vector<TargetRef> EnumerateTargets()
	{
		std::vector<TargetRef> out;
		const auto&            s = Resolve();
		if (!s.ok)
			return out;

		for (std::uint32_t i = 0; i < s.count; ++i) {
			auto* texture = reinterpret_cast<ID3D11Texture2D*>(s.targets[i].texture);
			if (!texture)
				continue;
			// Re-validated per call, not just at resolve time: the engine frees and
			// recreates targets on a resolution change, and a stale slot would be a
			// dangling pointer we then hand to CopySubresourceRegion.
			if (!PlausibleTexture(texture))
				continue;
			out.push_back(TargetRef{ i, texture });
		}
		return out;
	}

	std::string TargetName(std::uint32_t a_index)
	{
		// AE 1.6.1130 has 116 targets where this header describes 114, so the enum and
		// the game disagree from the insertion point onward. Naming a buffer with its
		// neighbour's name is worse than not naming it — the same rule Fallout's
		// TargetName() follows for the logical/physical index split — so on that runtime
		// we decline rather than guess. The index is unambiguous either way.
		if (REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_1130)
			return {};
		if (a_index >= std::size(kTargetNames))
			return {};
		// VR-only slots are meaningless on flat even though the table carries them.
		if (a_index >= kFlatTargetCount && !REL::Module::IsVR())
			return {};
		return kTargetNames[a_index];
	}
}
