#pragma once

#include <cstdint>
#include <string>

// DXGI pixel decode, NaN detection, and darkness classification — pure logic, no
// D3D11, no game. Split out so it can be unit-tested and so the interesting part of
// the graphics bench is readable without a running renderer.
//
// PROVENANCE AND WHY THE NaN PATH EXISTS AT ALL
//
// The Fallout 4 VR scope investigation spent five sessions and eliminated nine
// suspects chasing intermittent black frames, against an instrument that could not
// see the cause. The probe classified a pixel as "dark" by testing whether its
// float exponent was below 12. A NaN's exponent is 31. So NaN — which DISPLAYS as
// black — probed as *bright*, and 12,000+ readbacks across those sessions all
// reported "0 dark" while the screen was visibly black.
//
// That is the single most expensive lesson in the toolkit, and it is why
// IsNonFinite is a separate, explicitly-called test rather than something folded
// into IsDark, and why the dump paints non-finite pixels magenta instead of
// clamping them to white. An overbright frame and a frame full of NaN look
// identical once clamped; the first dump session read as "blown out" for exactly
// that reason.

namespace dvb::gfx
{
	/// The DXGI_FORMAT values the decoder handles. Deliberately a closed set: the
	/// original code had a catch-all that assumed 8 bytes per pixel, so a 4-byte
	/// surface was read at twice its row length and walked off the end of the mapped
	/// staging texture. That crash was misattributed to an unrelated render step for
	/// a whole session. An unknown format is now REPORTED as unsupported and skipped
	/// before anything is mapped.
	enum : std::uint32_t
	{
		kR16G16B16A16_FLOAT = 10,
		kR11G11B10_FLOAT = 26,
		kR8G8B8A8_UNORM = 28,
		kR8G8B8A8_UNORM_SRGB = 29,
		kR16G16_UNORM = 35,
		kB8G8R8A8_UNORM = 87,
		kB8G8R8A8_UNORM_SRGB = 91,
	};

	/// DXGI_FORMAT name for reporting, e.g. "R11G11B10_FLOAT". Returns
	/// "UNKNOWN(<n>)" for anything outside the table — never an empty string, so a
	/// result always says what it saw.
	std::string FormatName(std::uint32_t a_format);

	/// Bytes per pixel, or 0 if this decoder cannot read the format. 0 is the gate:
	/// callers must not map a surface whose stride they do not know.
	std::uint32_t BytesPerPixel(std::uint32_t a_format);

	/// True if the decoder can turn this format into viewable RGB.
	inline bool IsSupported(std::uint32_t a_format) { return BytesPerPixel(a_format) != 0; }

	/// One decoded pixel, linear (NOT tone-mapped), plus whether it was non-finite.
	struct Pixel
	{
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
		bool  nonFinite = false;  ///< any channel was NaN or Inf
	};

	/// Decode one pixel from raw bytes. `a_src` must point at BytesPerPixel bytes.
	/// Behaviour is undefined for an unsupported format — check IsSupported first.
	Pixel Decode(std::uint32_t a_format, const std::uint8_t* a_src);

	/// Non-finite test on the raw little-endian pixel bits, without a full decode.
	/// Used for cheap single-pixel probes. Integer formats always return false —
	/// they cannot hold NaN.
	bool IsNonFinite(std::uint32_t a_format, std::uint64_t a_raw);

	/// Near-black test on raw bits. NOTE: this deliberately does NOT account for
	/// NaN — call IsNonFinite separately. Folding the two together is how the
	/// original probe lied for five sessions (see the header comment); keeping them
	/// separate forces the caller to ask both questions.
	bool IsDark(std::uint32_t a_format, std::uint64_t a_raw);

	/// Linear HDR -> viewable 8-bit: Reinhard tone map + 2.2 gamma. Keeps a very
	/// dark scene and a very bright one both distinguishable from true black, which
	/// is the entire point of looking at the surface.
	std::uint8_t ToByte(float a_linear);
}
