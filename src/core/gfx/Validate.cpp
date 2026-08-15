#include "core/gfx/Validate.h"

#include "core/tools/Memory.h"

#include <d3d11.h>

namespace
{
	// GetDesc is a virtual call, so even a COM-shaped pointer can still fault if the
	// object is not really a texture. Its own function, free of anything needing
	// destruction, because MSVC will not accept __try in a frame that requires unwinding.
	bool SafeGetDesc(ID3D11Texture2D* a_tex, D3D11_TEXTURE2D_DESC* a_out) noexcept
	{
		__try {
			a_tex->GetDesc(a_out);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
}

namespace dvb::gfx
{
	bool LooksLikeCOM(const void* a_p) noexcept
	{
		const auto va = reinterpret_cast<std::uintptr_t>(a_p);
		if (va < 0x10000 || (va & 7) != 0)
			return false;
		std::uintptr_t vtable = 0;
		if (!mem::SafeRead(a_p, &vtable, sizeof(vtable)) || vtable < 0x10000 || (vtable & 7) != 0)
			return false;
		std::uintptr_t firstMethod = 0;
		return mem::SafeRead(reinterpret_cast<const void*>(vtable), &firstMethod, sizeof(firstMethod)) &&
		       firstMethod >= 0x10000;
	}

	bool PlausibleTexture(ID3D11Texture2D* a_texture) noexcept
	{
		if (!LooksLikeCOM(a_texture))
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		if (!SafeGetDesc(a_texture, &desc))
			return false;
		return desc.Width > 0 && desc.Width <= 16384 && desc.Height > 0 && desc.Height <= 16384;
	}
}
