#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
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
// THREADING: BeginReadback / PollReadback only from a main-thread task, and only where the
// platform has established that tasks stay on the main thread (see Device.h). The staging
// texture is released only on the thread that created it (PendingReadback).
// Analyse / WriteBMP touch no D3D and are safe on any thread.

namespace dvb::gfx
{
	/// A CPU-side copy of one render target.
	struct Surface
	{
		std::uint32_t             width = 0;
		std::uint32_t             height = 0;
		std::uint32_t             format = 0;  ///< DXGI_FORMAT
		std::uint32_t             pitch = 0;   ///< row stride in bytes
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

	/// A render target copy in flight: the target has been copied into a staging texture
	/// this object owns, and the GPU may not have executed that copy yet.
	///
	/// WHY TWO STEPS, and never a blocking Map: Map(READ) without DO_NOT_WAIT makes the
	/// CPU wait for the GPU to drain everything queued before the copy, on the thread
	/// that drives the renderer. On Fallout 4 VR, readbacks done that way were followed
	/// twice by a permanent hang inside OpenVR's submit. BeginReadback queues the copy and
	/// returns; PollReadback, called on later frames, maps only once the GPU is done.
	/// devbench never waits on the GPU inside the game's frame.
	///
	/// Both forms failed on Fallout 4 VR for a reason outside this file: F4SE ran the
	/// "main-thread" tasks on worker threads, and the copy raced the renderer (TS-031).
	/// Called from the main thread, this ran clean, including the 1024² target that had
	/// crashed the driver (FO4VR 1.2.72, 2026-10-28).
	///
	/// Holds no engine object, only its own staging texture, so the engine is free to
	/// recreate the target between the two steps.
	struct PendingReadback
	{
		/// Released with the last copy, but ONLY on the thread that created it. A release
		/// reached on any other thread is refused and the texture leaked (and logged): a
		/// few MB lost is a better failure than a D3D call racing the renderer. The first
		/// build released it wherever the last reference died, usually the HTTP listener.
		std::shared_ptr<ID3D11Texture2D> staging;
		std::uint32_t                    width = 0;
		std::uint32_t                    height = 0;
		std::uint32_t                    format = 0;  ///< DXGI_FORMAT

		[[nodiscard]] bool Started() const { return staging != nullptr; }
	};

	/// Step 1, main thread: queue a copy of slice 0 / mip 0 of a_texture into a fresh
	/// staging texture. Never waits. Fails (Started() false, a_error set) for a format
	/// the decoder does not know, or MSAA, BEFORE anything is created — reading a 4-byte
	/// surface at an assumed 8 bytes per pixel runs off the end of the mapped staging
	/// texture, a crash that historically got blamed on whatever render step contained
	/// the dump.
	///
	/// a_queueCopy=false stops after creating the staging texture: an investigation step,
	/// so a live run can tell "creating a texture breaks it" from "the copy breaks it".
	PendingReadback BeginReadback(ID3D11Texture2D* a_texture, std::string& a_error, bool a_queueCopy = true);

	enum class ReadbackPoll
	{
		kPending,  ///< the GPU has not executed the copy yet — poll again on a later frame
		kReady,    ///< a_out is filled
		kFailed,   ///< a_error says why; polling again will not help
	};

	/// Step 2, main thread, a later frame: map the staging copy WITHOUT waiting.
	///
	/// a_maxDimension downsamples by an integer stride when the target is larger, so
	/// a 4K buffer does not turn into a 30 MB JSON conversation or a 25 MB file.
	/// 0 = full resolution.
	ReadbackPoll PollReadback(const PendingReadback& a_pending, std::uint32_t a_maxDimension, Surface& a_out, std::string& a_error);

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
