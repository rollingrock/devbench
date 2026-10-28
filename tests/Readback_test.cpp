// The render-target readback (core/gfx/Capture.cpp) against a real D3D11 device: WARP, the
// software rasterizer every Windows install has, so no GPU and no game is needed.
//
// Before this, the copy -> map -> downsample path had never run outside a game, and the only
// way to learn it was wrong was a live session — one of which hung FO4VR. These cases pin the
// contract the non-blocking split relies on: BeginReadback only queues, PollReadback answers
// kPending (never waits) until the copy has executed, then returns the exact pixels.

#include "test_framework.h"

#include "core/gfx/Capture.h"
#include "core/gfx/Device.h"
#include "core/gfx/Format.h"

#include <d3d11.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

// The graphics seam (core/gfx/Device.h) is defined per platform; here it is the test's WARP
// device. Only Device() and Context() are referenced by Capture.cpp.
namespace
{
	ID3D11Device*        g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;

	bool EnsureWarp()
	{
		if (g_device)
			return true;
		const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
			&g_device, nullptr, &g_context);
		return SUCCEEDED(hr) && g_device && g_context;
	}

	// A GPU-only texture, like the engine's targets, holding a_pixels (RGBA8, row-major).
	ID3D11Texture2D* MakeTarget(std::uint32_t a_w, std::uint32_t a_h, DXGI_FORMAT a_format,
		const std::vector<std::uint32_t>* a_pixels = nullptr, std::uint32_t a_samples = 1)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = a_w;
		d.Height = a_h;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = a_format;
		d.SampleDesc = { a_samples, 0 };
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{};
		if (a_pixels) {
			init.pSysMem = a_pixels->data();
			init.SysMemPitch = a_w * 4;
		}
		ID3D11Texture2D* t = nullptr;
		g_device->CreateTexture2D(&d, a_pixels ? &init : nullptr, &t);
		return t;
	}

	// Poll the way the tool does — one try per "frame" — with the engine's per-frame flush
	// stood in for by an explicit Flush between tries, and a frame's worth of time between
	// them. Bounded by time, not by a count: a fixed 1000 back-to-back tries outran WARP's
	// copy on a fast run, and the case then read an empty surface and crashed the suite.
	// It passed only when something slowed it down (a debugger, unbuffered stdout).
	dvb::gfx::ReadbackPoll PollUntilDone(const dvb::gfx::PendingReadback& a_pending, std::uint32_t a_maxDim,
		dvb::gfx::Surface& a_out, std::string& a_err, int& a_polls)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (a_polls = 1; std::chrono::steady_clock::now() < deadline; ++a_polls) {
			const auto r = dvb::gfx::PollReadback(a_pending, a_maxDim, a_out, a_err);
			if (r != dvb::gfx::ReadbackPoll::kPending)
				return r;
			g_context->Flush();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return dvb::gfx::ReadbackPoll::kPending;
	}

	std::uint32_t PixelAt(const dvb::gfx::Surface& a_s, std::uint32_t a_x, std::uint32_t a_y)
	{
		std::uint32_t v = 0;
		std::memcpy(&v, a_s.bytes.data() + static_cast<std::size_t>(a_y) * a_s.pitch + a_x * 4, 4);
		return v;
	}
}

namespace dvb::gfx
{
	ID3D11Device*        Device() { return g_device; }
	ID3D11DeviceContext* Context() { return g_context; }
}

TEST_CASE("readback returns the target's exact pixels after polling, without a blocking map")
{
	CHECK_MESSAGE(EnsureWarp(), "WARP D3D11 device unavailable");
	if (!g_device)
		return;
	const std::vector<std::uint32_t> pixels{ 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0x80808080u,
		0x01020304u, 0x0A0B0C0Du, 0xDEADBEEFu, 0x00000000u };
	ID3D11Texture2D*                 target = MakeTarget(4, 2, DXGI_FORMAT_R8G8B8A8_UNORM, &pixels);
	CHECK(target != nullptr);
	if (!target)
		return;

	std::string err;
	const auto  pending = dvb::gfx::BeginReadback(target, err);
	CHECK_MESSAGE(pending.Started(), err.c_str());
	target->Release();  // the readback holds only its own staging copy, not the engine's texture

	dvb::gfx::Surface surface;
	int               polls = 0;
	const auto        polled = PollUntilDone(pending, 0, surface, err, polls);
	CHECK(polled == dvb::gfx::ReadbackPoll::kReady);
	if (polled != dvb::gfx::ReadbackPoll::kReady || !surface.Valid())
		return;  // reported above; reading an empty surface would crash the suite instead
	CHECK(surface.width == 4);
	CHECK(surface.height == 2);
	CHECK(surface.format == DXGI_FORMAT_R8G8B8A8_UNORM);
	for (std::uint32_t y = 0; y < 2; ++y)
		for (std::uint32_t x = 0; x < 4; ++x)
			CHECK(PixelAt(surface, x, y) == pixels[y * 4 + x]);
}

TEST_CASE("readback downsamples by nearest sample, never by averaging")
{
	if (!EnsureWarp())
		return;
	const std::vector<std::uint32_t> pixels{ 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
		0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u };
	ID3D11Texture2D*                 target = MakeTarget(4, 2, DXGI_FORMAT_R8G8B8A8_UNORM, &pixels);
	std::string                      err;
	const auto                       pending = dvb::gfx::BeginReadback(target, err);
	target->Release();

	dvb::gfx::Surface surface;
	int               polls = 0;
	const auto        polled = PollUntilDone(pending, 2, surface, err, polls);
	CHECK(polled == dvb::gfx::ReadbackPoll::kReady);
	if (polled != dvb::gfx::ReadbackPoll::kReady || !surface.Valid())
		return;
	CHECK(surface.width == 2);
	CHECK(surface.height == 1);
	CHECK(PixelAt(surface, 0, 0) == 0x11111111u);  // (0,0), not a blend of its neighbours
	CHECK(PixelAt(surface, 1, 0) == 0x33333333u);  // (2,0)
}

TEST_CASE("an undecodable format is refused before anything is created")
{
	if (!EnsureWarp())
		return;
	ID3D11Texture2D* target = MakeTarget(4, 4, DXGI_FORMAT_R32_UINT);
	std::string      err;
	const auto       pending = dvb::gfx::BeginReadback(target, err);
	target->Release();
	CHECK(!pending.Started());
	CHECK(err.find("not decodable") != std::string::npos);
}

TEST_CASE("an MSAA target is refused with a reason, not copied")
{
	if (!EnsureWarp())
		return;
	ID3D11Texture2D* target = MakeTarget(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 4);
	CHECK(target != nullptr);
	if (!target)
		return;
	std::string err;
	const auto  pending = dvb::gfx::BeginReadback(target, err);
	target->Release();
	CHECK(!pending.Started());
	CHECK(err.find("MSAA") != std::string::npos);
}

TEST_CASE("polling a readback that never started fails instead of mapping nothing")
{
	dvb::gfx::PendingReadback none;
	dvb::gfx::Surface         surface;
	std::string               err;
	CHECK(dvb::gfx::PollReadback(none, 0, surface, err) == dvb::gfx::ReadbackPoll::kFailed);
	CHECK(!surface.Valid());

	std::string nullErr;
	CHECK(!dvb::gfx::BeginReadback(nullptr, nullErr).Started());
	CHECK(!nullErr.empty());
}

// The first non-blocking build released its staging texture wherever the last reference
// died, usually the HTTP listener thread, while the game drove D3D on its own thread.
// The texture is now released only on the thread that created it; anywhere else, it is
// leaked on purpose.
TEST_CASE("the staging texture is released only on the thread that created it")
{
	if (!EnsureWarp())
		return;
	ID3D11Texture2D* target = MakeTarget(4, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
	std::string      err;
	auto             pending = dvb::gfx::BeginReadback(target, err);
	target->Release();
	CHECK(pending.Started());
	if (!pending.Started())
		return;

	// An extra reference of our own, so the texture's fate can be observed afterwards.
	ID3D11Texture2D* const staging = pending.staging.get();
	staging->AddRef();

	std::thread([moved = std::move(pending.staging)]() mutable { moved.reset(); }).join();
	// Refused off-thread: the readback's own reference is still there besides ours.
	CHECK(staging->Release() == 1);

	staging->Release();  // the deliberately leaked one, so the test itself leaks nothing
}

TEST_CASE("a create-only readback makes the staging texture and queues no copy")
{
	if (!EnsureWarp())
		return;
	ID3D11Texture2D* target = MakeTarget(4, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
	std::string      err;
	auto             pending = dvb::gfx::BeginReadback(target, err, false);
	target->Release();
	CHECK_MESSAGE(pending.Started(), err.c_str());
	CHECK(pending.width == 4);
	CHECK(pending.height == 2);
	pending.staging.reset();  // same thread: released, no warning
}
