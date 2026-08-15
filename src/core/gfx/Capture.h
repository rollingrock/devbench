#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ID3D11Texture2D;

// Render-target readback, analysis, and dumping. Game-agnostic: it takes an
// ID3D11Texture2D from the platform seam (Device.h) and does the rest.
//
// This is the capability that a screenshot-and-compare bench cannot provide. A
// screenshot shows the final image; this shows the buffers that produced it — the
// G-buffer, the light accumulation, the shadow map — which is where a rendering
// bug actually lives.
//
// THREADING: call from a main-thread task only. See Device.h.

namespace dvb::gfx
{
	/// A CPU-side copy of one render target.
	struct Surface
	{
		std::uint32_t             width = 0;
		std::uint32_t             height = 0;
		std::uint32_t             format = 0;   ///< DXGI_FORMAT
		std::uint32_t             pitch = 0;    ///< row stride in bytes
		std::vector<std::uint8_t> bytes;

		[[nodiscard]] bool Valid() const { return width && height && !bytes.empty(); }
	};

	/// What the surface actually contains, as numbers rather than an image.
	///
	/// nonFinitePct is first because it is the one that gets missed: a buffer full of
	/// NaN displays as black but reads as bright to any exponent-threshold test, so
	/// "darkPct: 0" on its own has repeatedly meant "we are not measuring the
	/// problem". Report both, always, and never let one imply the other.
	struct Stats
	{
		std::uint64_t pixels = 0;
		double        nonFinitePct = 0.0;  ///< % of pixels with a NaN/Inf channel
		double        darkPct = 0.0;       ///< % of pixels below the near-black threshold
		double        meanLuma = 0.0;      ///< mean linear luminance, non-finite pixels excluded
		float         maxChannel = 0.0f;   ///< brightest finite channel seen
	};

	/// Copy a render target to system memory. Fails (returns an invalid Surface) for
	/// a format the decoder does not know, BEFORE mapping anything — reading a
	/// 4-byte surface at an assumed 8 bytes per pixel runs off the end of the mapped
	/// staging texture, which is a crash, and one that historically got blamed on
	/// whatever render step happened to contain the dump.
	///
	/// a_maxDimension downsamples by an integer stride when the target is larger, so
	/// a 4K buffer does not turn into a 30 MB JSON conversation or a 25 MB file.
	/// 0 = full resolution.
	Surface Readback(ID3D11Texture2D* a_texture, std::uint32_t a_maxDimension, std::string& a_error);

	/// Analyse a surface. Pure logic; no D3D, safe on any thread.
	Stats Analyse(const Surface& a_surface);

	/// Write a 24-bit BMP. HDR is Reinhard-tone-mapped so a dark scene and an
	/// overbright one are both distinguishable from true black.
	///
	/// NON-FINITE PIXELS ARE PAINTED MAGENTA rather than clamped to white. Clamping
	/// makes a NaN-filled buffer look identical to a legitimately blown-out frame —
	/// the first dump session in the investigation this came from read as "blown
	/// out" for exactly that reason, and cost a day.
	///
	/// BMP and not PNG on purpose: no encoder dependency, and the file is a debugging
	/// artefact that gets looked at once and deleted.
	bool WriteBMP(const std::filesystem::path& a_path, const Surface& a_surface, std::string& a_error);
}
