// The Fallout 4 graphics seam: hand the core the D3D11 device and the renderer's
// render targets. Everything downstream (readback, decode, NaN analysis, dumps) is
// game-agnostic and lives in src/core/gfx/.
//
// LAYOUT, AND WHY IT IS TRUSTED
//
//   BSGraphics::Renderer { bool skipNextPresent @0x00
//                          ResetRenderTargets_t @0x08
//                          RendererData data     @0x10 }
//   RendererData         { ... device @0x48, context @0x50,
//                          RenderTarget renderTargets[101] @0x0A58 }
//   RenderTarget         { ID3D11Texture2D* texture @0x00, ... rtView @0x10 }  (0x30 bytes)
//
// Those come from CommonLibF4 (flat-rim). They are corroborated independently for
// VR: the Fallout 4 VR scope investigation established by live x64dbg that the
// render-target VIEW for physical slot n sits at `renderer + 0xA78 + n*0x30`, and
// 0x10 + 0x0A58 + 0x10 == 0xA78 exactly. Two derivations from unrelated evidence
// agreeing on the same byte is the strongest signal available without a PDB.
//
// Resolution still differs per runtime, because the VR address library does not
// cover the renderer id:
//   * Fallout 4     REL::ID(1235449) via CommonLibF4's singleton (a RendererData**).
//   * Fallout 4 VR  the Renderer instance at its measured image offset, +0x10.
//
// Nothing here is trusted on faith: Resolve() validates the device, the context and
// a sample of the target array before publishing anything, and returns null rather
// than a plausible-looking wrong pointer. A garbage ID3D11Texture2D* handed to D3D
// is a crash, not a bad answer, so the gate has to be upstream of the first call.

#include "core/gfx/Device.h"

#include "core/Log.h"
#include "core/tools/Memory.h"

#include <d3d11.h>

namespace
{
	constexpr std::uintptr_t kRendererInstanceVR = 0x6239340;  // BSGraphics::Renderer, measured
	constexpr std::uintptr_t kRendererDataOffset = 0x10;       // Renderer::data
	constexpr std::uintptr_t kDeviceOffset = 0x48;             // RendererData::device
	constexpr std::uintptr_t kContextOffset = 0x50;            // RendererData::context
	constexpr std::uintptr_t kTargetsOffset = 0x0A58;          // RendererData::renderTargets
	constexpr std::uintptr_t kTargetStride = 0x30;             // sizeof(RenderTarget)
	constexpr std::uint32_t  kTargetCount = 101;

	// A COM object's first qword is its vtable, and that vtable lives in a loaded
	// module. Both reads are SEH-guarded, so a wrong offset produces `false` instead
	// of an access violation. This is the cheap filter that runs BEFORE any virtual
	// call on a candidate pointer.
	bool LooksLikeCOM(const void* a_p) noexcept
	{
		const auto va = reinterpret_cast<std::uintptr_t>(a_p);
		if (va < 0x10000 || (va & 7) != 0)
			return false;
		std::uintptr_t vtable = 0;
		if (!dvb::mem::SafeRead(a_p, &vtable, sizeof(vtable)) || vtable < 0x10000 || (vtable & 7) != 0)
			return false;
		std::uintptr_t firstMethod = 0;
		return dvb::mem::SafeRead(reinterpret_cast<const void*>(vtable), &firstMethod, sizeof(firstMethod)) &&
		       firstMethod >= 0x10000;
	}

	// GetDesc is a virtual call, so even a COM-shaped pointer can still fault if the
	// object is not really a texture. Guarded, and free of anything needing
	// destruction so MSVC will accept __try here.
	bool SafeGetDesc(ID3D11Texture2D* a_tex, D3D11_TEXTURE2D_DESC* a_out) noexcept
	{
		__try {
			a_tex->GetDesc(a_out);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool PlausibleTarget(ID3D11Texture2D* a_tex) noexcept
	{
		if (!LooksLikeCOM(a_tex))
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		if (!SafeGetDesc(a_tex, &desc))
			return false;
		return desc.Width > 0 && desc.Width <= 16384 && desc.Height > 0 && desc.Height <= 16384;
	}

	struct Resolved
	{
		ID3D11Device*        device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		std::uintptr_t       targets = 0;  // &renderTargets[0]
		bool                 tried = false;
		bool                 ok = false;
	};

	Resolved& State()
	{
		static Resolved s;
		return s;
	}

	// Resolve once. Deliberately lazy rather than done at plugin load: the renderer
	// does not exist yet at F4SEPlugin_Load, and a failed early attempt that cached
	// "unavailable" would make the graphics tools permanently dead for the session.
	Resolved& Resolve()
	{
		auto& s = State();
		if (s.tried)
			return s;

		std::uintptr_t rendererData = 0;
		if (REL::Module::IsVR()) {
			rendererData = REL::Module::get().base() + kRendererInstanceVR + kRendererDataOffset;
		} else {
			try {
				// REL::ID(1235449) is a RendererData** — the global holding the pointer.
				const auto slot = REL::ID(1235449).address();
				std::uintptr_t p = 0;
				if (dvb::mem::SafeRead(reinterpret_cast<const void*>(slot), &p, sizeof(p)))
					rendererData = p;
			} catch (...) {
				rendererData = 0;
			}
		}

		if (!rendererData) {
			s.tried = true;
			dvb::dlog::warn("gfx: could not resolve the renderer on this runtime — graphics tools unavailable");
			return s;
		}

		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
		dvb::mem::SafeRead(reinterpret_cast<const void*>(rendererData + kDeviceOffset), &device, sizeof(device));
		dvb::mem::SafeRead(reinterpret_cast<const void*>(rendererData + kContextOffset), &context, sizeof(context));

		if (!LooksLikeCOM(reinterpret_cast<void*>(device)) || !LooksLikeCOM(reinterpret_cast<void*>(context))) {
			// The renderer is either not up yet or the offsets are wrong for this
			// build. Do NOT latch: leaving `tried` false lets a later call succeed
			// once the device exists, which is the overwhelmingly common case.
			return s;
		}

		// Sample the target array before believing it. If the base were wrong we would
		// be handing D3D a list of arbitrary qwords; requiring several entries to be
		// real textures makes that essentially impossible to pass by accident.
		const std::uintptr_t targets = rendererData + kTargetsOffset;
		std::uint32_t        valid = 0;
		for (std::uint32_t i = 0; i < kTargetCount; ++i) {
			std::uintptr_t tex = 0;
			if (dvb::mem::SafeRead(reinterpret_cast<const void*>(targets + i * kTargetStride), &tex, sizeof(tex)) &&
				tex && PlausibleTarget(reinterpret_cast<ID3D11Texture2D*>(tex)))
				++valid;
		}
		if (valid < 4) {
			s.tried = true;  // device was fine, so this IS a layout problem — latch it
			dvb::dlog::warn(
				"gfx: renderer resolved but only {} of {} target slots look like textures — refusing to use a layout that does not check out",
				valid, kTargetCount);
			return s;
		}

		s.device = reinterpret_cast<ID3D11Device*>(device);
		s.context = reinterpret_cast<ID3D11DeviceContext*>(context);
		s.targets = targets;
		s.ok = true;
		s.tried = true;
		dvb::dlog::info("gfx: renderer resolved ({} live render targets)", valid);
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

		for (std::uint32_t i = 0; i < kTargetCount; ++i) {
			std::uintptr_t tex = 0;
			if (!mem::SafeRead(reinterpret_cast<const void*>(s.targets + i * kTargetStride), &tex, sizeof(tex)) || !tex)
				continue;
			auto* texture = reinterpret_cast<ID3D11Texture2D*>(tex);
			// Re-validate per call, not just at resolve time: the engine frees and
			// recreates targets on a resolution change, and a stale slot would be a
			// dangling pointer we then hand to CopySubresourceRegion.
			if (!PlausibleTarget(texture))
				continue;
			out.push_back(TargetRef{ i, texture });
		}
		return out;
	}

	std::string TargetName(std::uint32_t)
	{
		// Fallout's targets are addressed by LOGICAL id through
		// RenderTargetManager's remap table, which is a different index space from
		// the physical slots enumerated above. Rather than print a name that might
		// belong to a different buffer, print none. Mapping the two spaces is
		// follow-up work; the physical index is unambiguous in the meantime.
		return {};
	}
}
