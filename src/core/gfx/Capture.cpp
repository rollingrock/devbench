#include "core/gfx/Capture.h"

#include "core/Log.h"
#include "core/gfx/Device.h"
#include "core/gfx/Format.h"

#include <d3d11.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace dvb::gfx
{
	namespace
	{
		// Round a 24bpp BMP row up to a 4-byte boundary.
		constexpr std::uint32_t BmpRowBytes(std::uint32_t a_width)
		{
			return ((a_width * 3u) + 3u) & ~3u;
		}
	}

	PendingReadback BeginReadback(ID3D11Texture2D* a_texture, std::string& a_error, bool a_queueCopy)
	{
		PendingReadback out;
		if (!a_texture) {
			a_error = "no such render target";
			return out;
		}
		auto* ctx = Context();
		auto* dev = Device();
		if (!ctx || !dev) {
			a_error = "the renderer is not available (no device/context)";
			return out;
		}

		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);

		if (BytesPerPixel(desc.Format) == 0) {
			// Refuse BEFORE creating anything. An unknown stride is not a decode
			// inconvenience, it is an out-of-bounds read of the mapped surface.
			a_error = std::format("format {} is not decodable by this build", FormatName(desc.Format));
			return out;
		}
		if (desc.Width == 0 || desc.Height == 0) {
			a_error = "render target has zero extent";
			return out;
		}

		// A staging copy is required: the engine's targets are GPU-only. MSAA targets
		// cannot be copied directly, so say so rather than failing opaquely inside D3D.
		if (desc.SampleDesc.Count > 1) {
			a_error = std::format("render target is {}x MSAA; resolve it first", desc.SampleDesc.Count);
			return out;
		}

		D3D11_TEXTURE2D_DESC sd = desc;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.SampleDesc = { 1, 0 };
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;

		ID3D11Texture2D* staging = nullptr;
		if (const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &staging); FAILED(hr) || !staging) {
			a_error = std::format("CreateTexture2D(STAGING) failed: 0x{:08X}", static_cast<std::uint32_t>(hr));
			return out;
		}

		// Owned from here, so every path out releases it, and only on this thread (see
		// PendingReadback::staging for why the thread matters).
		out.staging = std::shared_ptr<ID3D11Texture2D>(staging, [owner = ::GetCurrentThreadId()](ID3D11Texture2D* a_t) {
			if (::GetCurrentThreadId() == owner) {
				a_t->Release();
				return;
			}
			dlog::warn("gfx: readback staging texture reached its last release on thread {} instead of {}; leaked rather than released off-thread",
				::GetCurrentThreadId(), owner);
		});

		// Slice 0, mip 0. An array target (stereo VR eyes) reports only the first
		// slice; saying which is better than silently averaging or picking at random.
		// Queued, not executed: the GPU runs it whenever it reaches it.
		if (a_queueCopy)
			ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, a_texture, 0, nullptr);

		out.width = desc.Width;
		out.height = desc.Height;
		out.format = desc.Format;
		return out;
	}

	ReadbackPoll PollReadback(const PendingReadback& a_pending, std::uint32_t a_maxDimension, Surface& a_out, std::string& a_error)
	{
		if (!a_pending.Started()) {
			a_error = "no readback in flight";
			return ReadbackPoll::kFailed;
		}
		auto* ctx = Context();
		if (!ctx) {
			a_error = "the renderer is not available (no context)";
			return ReadbackPoll::kFailed;
		}

		// DO_NOT_WAIT is the point (see PendingReadback): a copy the GPU has not executed
		// yet answers WAS_STILL_DRAWING instead of stalling the game's frame until it has.
		D3D11_MAPPED_SUBRESOURCE m{};
		const HRESULT            hr = ctx->Map(a_pending.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			return ReadbackPoll::kPending;
		if (FAILED(hr)) {
			a_error = std::format("Map(D3D11_MAP_READ, DO_NOT_WAIT) failed: 0x{:08X}", static_cast<std::uint32_t>(hr));
			return ReadbackPoll::kFailed;
		}
		// Unmap on every path out, including a bad_alloc from the resize below.
		struct Unmap
		{
			ID3D11DeviceContext* ctx;
			ID3D11Texture2D*     texture;
			~Unmap() { ctx->Unmap(texture, 0); }
		} unmap{ ctx, a_pending.staging.get() };

		const std::uint32_t bpp = BytesPerPixel(a_pending.format);

		// Integer-stride downsample. Nearest-sample rather than a box filter on
		// purpose: this is an instrument, and averaging would hide exactly the
		// single-pixel NaN or the one-pixel-wide seam somebody is looking for.
		std::uint32_t step = 1;
		if (a_maxDimension > 0) {
			const auto largest = std::max(a_pending.width, a_pending.height);
			while (largest / step > a_maxDimension)
				++step;
		}

		Surface out;
		out.width = (a_pending.width + step - 1) / step;
		out.height = (a_pending.height + step - 1) / step;
		out.format = a_pending.format;
		out.pitch = out.width * bpp;
		out.bytes.resize(static_cast<std::size_t>(out.pitch) * out.height);

		const auto* src = static_cast<const std::uint8_t*>(m.pData);
		for (std::uint32_t y = 0; y < out.height; ++y) {
			const auto* srcRow = src + static_cast<std::size_t>(y) * step * m.RowPitch;
			auto*       dstRow = out.bytes.data() + static_cast<std::size_t>(y) * out.pitch;
			if (step == 1) {
				std::memcpy(dstRow, srcRow, out.pitch);
			} else {
				for (std::uint32_t x = 0; x < out.width; ++x)
					std::memcpy(dstRow + static_cast<std::size_t>(x) * bpp,
						srcRow + static_cast<std::size_t>(x) * step * bpp, bpp);
			}
		}

		a_out = std::move(out);
		return ReadbackPoll::kReady;
	}

	Stats Analyse(const Surface& a_surface)
	{
		Stats st;
		if (!a_surface.Valid())
			return st;

		const std::uint32_t bpp = BytesPerPixel(a_surface.format);
		if (bpp == 0)
			return st;

		std::uint64_t nonFinite = 0;
		std::uint64_t dark = 0;
		double        lumaSum = 0.0;
		std::uint64_t lumaCount = 0;

		for (std::uint32_t y = 0; y < a_surface.height; ++y) {
			const auto* row = a_surface.bytes.data() + static_cast<std::size_t>(y) * a_surface.pitch;
			for (std::uint32_t x = 0; x < a_surface.width; ++x) {
				const auto* p = row + static_cast<std::size_t>(x) * bpp;

				// The raw-bits tests want a little-endian word; 4-byte formats must not
				// read 8 bytes (that would run past the last pixel of the last row).
				std::uint64_t raw = 0;
				std::memcpy(&raw, p, bpp);

				const bool bad = IsNonFinite(a_surface.format, raw);
				if (bad)
					++nonFinite;
				// Ask BOTH questions of every pixel. A NaN pixel is reported in
				// nonFinitePct and excluded from darkPct and the mean, so neither
				// number quietly absorbs it.
				else {
					if (IsDark(a_surface.format, raw))
						++dark;
					const auto px = Decode(a_surface.format, p);
					st.maxChannel = std::max({ st.maxChannel, px.r, px.g, px.b });
					lumaSum += 0.2126 * px.r + 0.7152 * px.g + 0.0722 * px.b;
					++lumaCount;
				}
			}
		}

		st.pixels = static_cast<std::uint64_t>(a_surface.width) * a_surface.height;
		if (st.pixels) {
			st.nonFinitePct = 100.0 * static_cast<double>(nonFinite) / static_cast<double>(st.pixels);
			st.darkPct = 100.0 * static_cast<double>(dark) / static_cast<double>(st.pixels);
		}
		if (lumaCount)
			st.meanLuma = lumaSum / static_cast<double>(lumaCount);
		return st;
	}

	bool WriteBMP(const std::filesystem::path& a_path, const Surface& a_surface, std::string& a_error)
	{
		if (!a_surface.Valid()) {
			a_error = "empty surface";
			return false;
		}
		const std::uint32_t bpp = BytesPerPixel(a_surface.format);
		if (bpp == 0) {
			a_error = "undecodable format";
			return false;
		}

		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		std::ofstream out(a_path, std::ios::binary);
		if (!out) {
			a_error = "cannot open " + a_path.string();
			return false;
		}

		const std::uint32_t     rowBytes = BmpRowBytes(a_surface.width);
		const std::uint32_t     imageBytes = rowBytes * a_surface.height;
		constexpr std::uint32_t headerBytes = 14u + 40u;

		const auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
		const auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
		out.write("BM", 2);
		put32(headerBytes + imageBytes);
		put32(0);
		put32(headerBytes);
		put32(40);
		put32(a_surface.width);
		put32(a_surface.height);
		put16(1);
		put16(24);
		put32(0);
		put32(imageBytes);
		put32(2835);
		put32(2835);
		put32(0);
		put32(0);

		std::vector<std::uint8_t> row(rowBytes, 0);
		for (std::uint32_t y = 0; y < a_surface.height; ++y) {
			// BMP is bottom-up.
			const auto* src = a_surface.bytes.data() +
			                  static_cast<std::size_t>(a_surface.height - 1u - y) * a_surface.pitch;
			for (std::uint32_t x = 0; x < a_surface.width; ++x) {
				const auto   px = Decode(a_surface.format, src + static_cast<std::size_t>(x) * bpp);
				std::uint8_t r, g, b;
				if (px.nonFinite) {
					// MAGENTA — never a real scene colour, so a NaN region is
					// unmistakable at a glance instead of reading as "blown out".
					r = 255;
					g = 0;
					b = 255;
				} else {
					r = ToByte(px.r);
					g = ToByte(px.g);
					b = ToByte(px.b);
				}
				row[x * 3u + 0] = b;
				row[x * 3u + 1] = g;
				row[x * 3u + 2] = r;
			}
			out.write(reinterpret_cast<const char*>(row.data()), rowBytes);
		}
		return static_cast<bool>(out);
	}
}
