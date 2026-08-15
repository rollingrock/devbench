#pragma once

struct ID3D11Texture2D;

// Pointer sanity checks shared by every platform's graphics seam (Device.h).
//
// These exist because the failure mode on this seam is not "a wrong answer", it is a
// CRASH: a render-target array read at a wrong struct offset yields plausible-looking
// qwords, and handing one of those to D3D as an ID3D11Texture2D* takes the process down.
// The gate therefore has to sit upstream of the first D3D call, and it has to be
// SEH-guarded, because by definition it is dereferencing pointers we do not yet trust.
//
// Shared rather than copied per platform. Fallout and Skyrim reach their renderer by
// completely different routes, but "does this look like a live COM texture?" is the same
// question, and two copies of a safety check is two places for one of them to get weaker.

namespace dvb::gfx
{
	/// Cheap structural filter: a COM object's first qword is a vtable pointer, and that
	/// vtable's first entry is a function pointer. Both reads are SEH-guarded, so a wrong
	/// offset returns false instead of faulting. Runs BEFORE any virtual call.
	[[nodiscard]] bool LooksLikeCOM(const void* a_p) noexcept;

	/// LooksLikeCOM plus a guarded GetDesc with a plausibility check on the dimensions.
	/// A COM-shaped pointer that is not really a texture still faults on the virtual
	/// call, so the guard is not redundant with the check above.
	[[nodiscard]] bool PlausibleTexture(ID3D11Texture2D* a_texture) noexcept;
}
