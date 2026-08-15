#include "core/gfx/Format.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dvb::gfx
{
	namespace
	{
		// R11G11B10_FLOAT channel decode: 5-bit exponent (bias 15) + 6/6/5-bit
		// mantissa, no sign bit.
		float DecodeSmallFloat(std::uint32_t a_bits, std::uint32_t a_mantBits, bool& a_nonFinite)
		{
			const auto mantMask = (1u << a_mantBits) - 1u;
			const auto mant = a_bits & mantMask;
			const auto exp = (a_bits >> a_mantBits) & 0x1f;
			if (exp == 0)
				return mant == 0 ? 0.0f : std::ldexp(static_cast<float>(mant) / static_cast<float>(mantMask + 1), -14);
			if (exp == 0x1f) {
				a_nonFinite = true;
				return 65504.0f;  // clamp: we only want to LOOK at it
			}
			return std::ldexp(1.0f + static_cast<float>(mant) / static_cast<float>(mantMask + 1),
				static_cast<int>(exp) - 15);
		}

		float DecodeHalf(std::uint16_t a_h, bool& a_nonFinite)
		{
			const auto sign = (a_h >> 15) & 1u;
			const auto exp = (a_h >> 10) & 0x1fu;
			const auto mant = a_h & 0x3ffu;
			if (exp == 0x1f) {
				a_nonFinite = true;
				return sign ? -65504.0f : 65504.0f;
			}
			const float v = exp == 0 ?
			                    std::ldexp(static_cast<float>(mant) / 1024.0f, -14) :
			                    std::ldexp(1.0f + static_cast<float>(mant) / 1024.0f, static_cast<int>(exp) - 15);
			return sign ? -v : v;
		}
	}

	std::string FormatName(std::uint32_t a_format)
	{
		switch (a_format) {
		case kR16G16B16A16_FLOAT:
			return "R16G16B16A16_FLOAT";
		case kR11G11B10_FLOAT:
			return "R11G11B10_FLOAT";
		case kR8G8B8A8_UNORM:
			return "R8G8B8A8_UNORM";
		case kR8G8B8A8_UNORM_SRGB:
			return "R8G8B8A8_UNORM_SRGB";
		case kR16G16_UNORM:
			return "R16G16_UNORM";
		case kB8G8R8A8_UNORM:
			return "B8G8R8A8_UNORM";
		case kB8G8R8A8_UNORM_SRGB:
			return "B8G8R8A8_UNORM_SRGB";
		default:
			return std::format("UNKNOWN({})", a_format);
		}
	}

	std::uint32_t BytesPerPixel(std::uint32_t a_format)
	{
		switch (a_format) {
		case kR16G16B16A16_FLOAT:
			return 8;
		case kR11G11B10_FLOAT:
		case kR8G8B8A8_UNORM:
		case kR8G8B8A8_UNORM_SRGB:
		case kR16G16_UNORM:
		case kB8G8R8A8_UNORM:
		case kB8G8R8A8_UNORM_SRGB:
			return 4;
		default:
			return 0;
		}
	}

	Pixel Decode(std::uint32_t a_format, const std::uint8_t* a_src)
	{
		Pixel out;
		switch (a_format) {
		case kR11G11B10_FLOAT: {
			std::uint32_t px = 0;
			std::memcpy(&px, a_src, 4);
			out.r = DecodeSmallFloat(px & 0x7ffu, 6, out.nonFinite);
			out.g = DecodeSmallFloat((px >> 11) & 0x7ffu, 6, out.nonFinite);
			out.b = DecodeSmallFloat((px >> 22) & 0x3ffu, 5, out.nonFinite);
			break;
		}
		case kR8G8B8A8_UNORM:
		case kR8G8B8A8_UNORM_SRGB:
			// Already display-referred; hand back 0..1 so ToByte is a no-op round trip
			// rather than tone-mapping an image that was never HDR.
			out.r = a_src[0] / 255.0f;
			out.g = a_src[1] / 255.0f;
			out.b = a_src[2] / 255.0f;
			break;
		case kB8G8R8A8_UNORM:
		case kB8G8R8A8_UNORM_SRGB:
			out.b = a_src[0] / 255.0f;
			out.g = a_src[1] / 255.0f;
			out.r = a_src[2] / 255.0f;
			break;
		case kR16G16_UNORM: {
			// A G-buffer normals target: two channels, Z reconstructed in-shader from
			// an encoding (octahedral, hemi, …) we would have to guess. Shown as
			// R=x, G=y, B=0 deliberately — guessing wrong would produce a
			// plausible-looking image that lies. Two channels of truth beat three of
			// speculation. UNORM cannot be NaN, so this never contributes to NaN%.
			std::uint16_t xy[2]{};
			std::memcpy(xy, a_src, 4);
			out.r = xy[0] / 65535.0f;
			out.g = xy[1] / 65535.0f;
			out.b = 0.0f;
			break;
		}
		case kR16G16B16A16_FLOAT: {
			std::uint16_t h[4]{};
			std::memcpy(h, a_src, 8);
			out.r = DecodeHalf(h[0], out.nonFinite);
			out.g = DecodeHalf(h[1], out.nonFinite);
			out.b = DecodeHalf(h[2], out.nonFinite);
			break;
		}
		default:
			break;  // caller must gate on IsSupported
		}
		return out;
	}

	bool IsNonFinite(std::uint32_t a_format, std::uint64_t a_raw)
	{
		switch (a_format) {
		case kR11G11B10_FLOAT: {
			// 5-bit exponent per channel; all-ones = Inf/NaN.
			const auto px = static_cast<std::uint32_t>(a_raw);
			return ((px >> 6) & 0x1f) == 0x1f || ((px >> 17) & 0x1f) == 0x1f || ((px >> 27) & 0x1f) == 0x1f;
		}
		case kR8G8B8A8_UNORM:
		case kR8G8B8A8_UNORM_SRGB:
		case kB8G8R8A8_UNORM:
		case kB8G8R8A8_UNORM_SRGB:
		case kR16G16_UNORM:
			return false;  // integer formats cannot hold NaN
		case kR16G16B16A16_FLOAT:
			for (int c = 0; c < 3; ++c) {
				if ((static_cast<std::uint16_t>(a_raw >> (16 * c)) & 0x7c00) == 0x7c00)
					return true;
			}
			return false;
		default:
			return false;
		}
	}

	bool IsDark(std::uint32_t a_format, std::uint64_t a_raw)
	{
		switch (a_format) {
		case kR11G11B10_FLOAT: {
			// Every channel exponent below 12 (~< 0.125). NOTE the exponent-31 NaN
			// case is NOT excluded here on purpose — see Format.h. Ask IsNonFinite too.
			const auto px = static_cast<std::uint32_t>(a_raw);
			return ((px >> 6) & 0x1f) < 12 && ((px >> 17) & 0x1f) < 12 && ((px >> 27) & 0x1f) < 12;
		}
		case kR8G8B8A8_UNORM:
		case kR8G8B8A8_UNORM_SRGB:
		case kB8G8R8A8_UNORM:
		case kB8G8R8A8_UNORM_SRGB:
			return (a_raw & 0xff) < 0x18 && ((a_raw >> 8) & 0xff) < 0x18 && ((a_raw >> 16) & 0xff) < 0x18;
		case kR16G16_UNORM:
			return (a_raw & 0xffff) < 0x1800 && ((a_raw >> 16) & 0xffff) < 0x1800;
		case kR16G16B16A16_FLOAT:
		default:
			// half magnitude below ~0.03 (0x2a00) on every channel
			for (int c = 0; c < 3; ++c) {
				if (((static_cast<std::uint16_t>(a_raw >> (16 * c))) & 0x7fff) >= 0x2a00)
					return false;
			}
			return true;
		}
	}

	std::uint8_t ToByte(float a_linear)
	{
		const auto tone = a_linear <= 0.0f ? 0.0f : a_linear / (1.0f + a_linear);
		const auto gamma = std::pow(tone, 1.0f / 2.2f);
		return static_cast<std::uint8_t>(std::clamp(gamma, 0.0f, 1.0f) * 255.0f + 0.5f);
	}
}
