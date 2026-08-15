// Host-independent coverage for the graphics format decoder.
//
// The centrepiece is NaNReadsAsBright. It is not a normal unit test — it asserts a
// TRAP rather than a feature, and it exists so that nobody ever "simplifies"
// IsDark and IsNonFinite back into one call.
//
// The history: a rendering bug in Fallout 4 VR produced intermittently black
// frames. The probe classified a pixel as dark by testing whether its float
// exponent was below 12. A NaN's exponent is 31, so every NaN pixel was classified
// BRIGHT — while displaying as black. Twelve thousand readbacks across five
// debugging sessions all reported "0 dark" against a visibly black screen, and nine
// innocent suspects were eliminated against that blind instrument before anyone
// questioned the instrument itself.

#include "test_framework.h"

#include "core/gfx/Format.h"

#include <cstdint>
#include <cstring>

namespace
{
	// R11G11B10_FLOAT bit layout: R = bits 0..10 (6-bit mantissa, 5-bit exponent),
	// G = bits 11..21 (same), B = bits 22..31 (5-bit mantissa, 5-bit exponent).
	constexpr std::uint32_t PackR11G11B10(std::uint32_t a_r, std::uint32_t a_g, std::uint32_t a_b)
	{
		return (a_r & 0x7ff) | ((a_g & 0x7ff) << 11) | ((a_b & 0x3ff) << 22);
	}

	// Exponent field all-ones = Inf/NaN.
	constexpr std::uint32_t kChan11NaN = 0x1fu << 6;   // 5-bit exponent above a 6-bit mantissa
	constexpr std::uint32_t kChan10NaN = 0x1fu << 5;   // 5-bit exponent above a 5-bit mantissa
	constexpr std::uint32_t kChan11Zero = 0u;

	// A mid-grey-ish value: exponent 14 (= 2^-1), zero mantissa.
	constexpr std::uint32_t kChan11Half = 14u << 6;
	constexpr std::uint32_t kChan10Half = 14u << 5;

	std::uint16_t HalfBits(std::uint16_t a_exp, std::uint16_t a_mant)
	{
		return static_cast<std::uint16_t>((a_exp << 10) | a_mant);
	}
}

TEST_CASE("format: NaN reads as BRIGHT to the darkness test -- ask IsNonFinite too")
{
	const std::uint64_t nan = PackR11G11B10(kChan11NaN, kChan11NaN, kChan10NaN);

	// THE TRAP, asserted so it cannot be forgotten: a NaN pixel — which shows on
	// screen as BLACK — is NOT reported as dark, because its exponent is the
	// largest possible rather than the smallest.
	CHECK_MESSAGE(!dvb::gfx::IsDark(dvb::gfx::kR11G11B10_FLOAT, nan),
		"IsDark must NOT claim to catch NaN; if this now passes as 'dark', the two tests "
		"have been merged and the NaN blind spot is back");

	// ...which is exactly why the caller has to ask the other question.
	CHECK(dvb::gfx::IsNonFinite(dvb::gfx::kR11G11B10_FLOAT, nan));
}

TEST_CASE("format: NaN reads as bright in RGBA16F too")
{
	std::uint64_t nan = 0;
	const std::uint16_t chans[4] = { HalfBits(0x1f, 1), HalfBits(0x1f, 1), HalfBits(0x1f, 1), 0 };
	std::memcpy(&nan, chans, sizeof(chans));

	CHECK(dvb::gfx::IsNonFinite(dvb::gfx::kR16G16B16A16_FLOAT, nan));
	CHECK_MESSAGE(!dvb::gfx::IsDark(dvb::gfx::kR16G16B16A16_FLOAT, nan),
		"a NaN half is magnitude 0x7c00+, far above the near-black threshold");
}

TEST_CASE("format: integer formats can never be non-finite")
{
	// UNORM has no exponent, so every bit pattern is a finite number. Reporting
	// otherwise would put a normals or albedo buffer permanently at 100% NaN.
	for (const auto fmt : { dvb::gfx::kR8G8B8A8_UNORM, dvb::gfx::kB8G8R8A8_UNORM, dvb::gfx::kR16G16_UNORM }) {
		CHECK(!dvb::gfx::IsNonFinite(fmt, 0xffffffffffffffffull));
		CHECK(!dvb::gfx::IsNonFinite(fmt, 0x7c007c007c007c00ull));
	}
}

TEST_CASE("format: darkness classification on real values")
{
	const std::uint64_t black = PackR11G11B10(kChan11Zero, kChan11Zero, 0);
	const std::uint64_t grey = PackR11G11B10(kChan11Half, kChan11Half, kChan10Half);
	CHECK(dvb::gfx::IsDark(dvb::gfx::kR11G11B10_FLOAT, black));
	CHECK(!dvb::gfx::IsDark(dvb::gfx::kR11G11B10_FLOAT, grey));

	// 8-bit: threshold is 0x18 per channel.
	CHECK(dvb::gfx::IsDark(dvb::gfx::kR8G8B8A8_UNORM, 0x000A0A0Aull));
	CHECK(!dvb::gfx::IsDark(dvb::gfx::kR8G8B8A8_UNORM, 0x00808080ull));
}

TEST_CASE("format: unknown formats are refused, not guessed")
{
	// An unknown format used to fall through to an 8-bytes-per-pixel decode. Any
	// 4-byte surface was then read at twice its row length, running off the end of
	// the mapped staging texture -- a crash that got attributed to an unrelated
	// render step for a whole session. BytesPerPixel returning 0 is the gate that
	// stops the surface being mapped at all.
	CHECK(dvb::gfx::BytesPerPixel(0) == 0);
	CHECK(dvb::gfx::BytesPerPixel(999) == 0);
	CHECK(!dvb::gfx::IsSupported(999));
	CHECK(dvb::gfx::FormatName(999) == "UNKNOWN(999)");

	// And every format we DO claim has a stride, or the gate is meaningless.
	for (const auto fmt : { dvb::gfx::kR16G16B16A16_FLOAT, dvb::gfx::kR11G11B10_FLOAT,
			 dvb::gfx::kR8G8B8A8_UNORM, dvb::gfx::kR8G8B8A8_UNORM_SRGB, dvb::gfx::kR16G16_UNORM,
			 dvb::gfx::kB8G8R8A8_UNORM, dvb::gfx::kB8G8R8A8_UNORM_SRGB }) {
		CHECK(dvb::gfx::IsSupported(fmt));
		CHECK(dvb::gfx::BytesPerPixel(fmt) == 4 || dvb::gfx::BytesPerPixel(fmt) == 8);
		CHECK(dvb::gfx::FormatName(fmt).find("UNKNOWN") == std::string::npos);
	}
}

TEST_CASE("format: decode marks non-finite pixels so the dump can colour them")
{
	std::uint32_t nan = PackR11G11B10(kChan11NaN, kChan11Half, kChan10Half);
	std::uint8_t  bytes[4];
	std::memcpy(bytes, &nan, 4);
	const auto px = dvb::gfx::Decode(dvb::gfx::kR11G11B10_FLOAT, bytes);
	CHECK_MESSAGE(px.nonFinite,
		"a single non-finite channel must flag the whole pixel; the dump paints these "
		"magenta, and clamping them to white makes a NaN buffer look merely overbright");

	std::uint32_t ok = PackR11G11B10(kChan11Half, kChan11Half, kChan10Half);
	std::memcpy(bytes, &ok, 4);
	CHECK(!dvb::gfx::Decode(dvb::gfx::kR11G11B10_FLOAT, bytes).nonFinite);
}

TEST_CASE("format: R16G16_UNORM decodes two channels and does not invent a third")
{
	// This is the G-buffer normals encoding. Z is reconstructed in-shader from an
	// encoding we would have to guess at; a guessed Z produces a plausible-looking
	// image that lies, so B is deliberately left at 0.
	const std::uint16_t xy[2] = { 0xffff, 0x0000 };
	std::uint8_t        bytes[4];
	std::memcpy(bytes, xy, 4);
	const auto px = dvb::gfx::Decode(dvb::gfx::kR16G16_UNORM, bytes);
	CHECK(px.r > 0.99f);
	CHECK(px.g < 0.01f);
	CHECK_MESSAGE(px.b == 0.0f, "blue must stay 0 -- two channels of truth beat three of speculation");
}

TEST_CASE("format: tone mapping keeps dark and overbright distinguishable from black")
{
	CHECK(dvb::gfx::ToByte(0.0f) == 0);
	// A very dim value must not round to pure black, or "dark" and "empty" become
	// the same picture.
	CHECK(dvb::gfx::ToByte(0.01f) > 0);
	// Reinhard never saturates, so a huge value stays below 255 and remains
	// distinguishable from a merely bright one.
	CHECK(dvb::gfx::ToByte(1.0f) < dvb::gfx::ToByte(100.0f));
	CHECK(dvb::gfx::ToByte(100.0f) <= 255);
}
