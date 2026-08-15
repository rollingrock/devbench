#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// The graphics half of the platform seam. A game supplies its D3D11 device and the
// render targets its renderer owns; everything downstream (readback, decode, NaN
// analysis, dumps) is game-agnostic and lives in Capture.h / Format.h.
//
// Kept to three functions on purpose. Enumerating targets by ENGINE INDEX rather
// than by name is what keeps it that small: a name table is per-game trivia, while
// "the renderer owns N textures" is true everywhere. TargetName() is the optional
// escape hatch for a game that does know its own names.
//
// THREADING: the D3D11 immediate context is not thread-safe and the engine drives it
// from the main thread. Everything in Capture.h therefore has to be called from a
// main-thread task (MainThread::RunAndWait), never from the server's listener
// thread. The tools do this; anything new must too.

namespace dvb::gfx
{
	/// Null until the renderer is up (and on a runtime where the platform cannot
	/// resolve it). Every caller must handle null rather than assuming a live game.
	ID3D11Device*        Device();
	ID3D11DeviceContext* Context();

	struct TargetRef
	{
		std::uint32_t    index = 0;        ///< the engine's own render-target index
		ID3D11Texture2D* texture = nullptr;  ///< borrowed; NOT AddRef'd
	};

	/// Every render target the engine currently owns, by engine index. Empty when
	/// the renderer is not resolvable. Implementations MUST validate before
	/// returning — a wrong struct offset yields plausible-looking garbage pointers,
	/// and handing those to D3D is a crash rather than a bad answer.
	std::vector<TargetRef> EnumerateTargets();

	/// Human-readable name for a target index, or "" if the game has no name for it.
	std::string TargetName(std::uint32_t a_index);
}
