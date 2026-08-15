#include "core/tools/Memory.h"

#include <Windows.h>

#include <charconv>
#include <cstring>
#include <format>

namespace dvb::mem
{
	namespace
	{
		std::string Hex(std::uint64_t a_v)
		{
			return std::format("0x{:x}", a_v);
		}

		// The image's true extent, read out of the PE optional header:
		//   base+0x3C            = e_lfanew
		//   base+e_lfanew+0x18   = IMAGE_OPTIONAL_HEADER64
		//   optionalHeader+0x38  = SizeOfImage
		// Doing it this way rather than trusting a constant is what stops a heap
		// pointer above the base from being reported with a meaningless RVA.
		std::uintptr_t ComputeImageSize(std::uintptr_t a_base)
		{
			std::uint32_t lfanew = 0;
			std::uint32_t sizeOfImage = 0;
			if (SafeRead(reinterpret_cast<const void*>(a_base + 0x3C), &lfanew, sizeof(lfanew)) &&
				lfanew > 0 && lfanew < 0x1000 &&
				SafeRead(reinterpret_cast<const void*>(a_base + lfanew + 0x18 + 0x38), &sizeOfImage, sizeof(sizeOfImage)) &&
				sizeOfImage > 0) {
				return sizeOfImage;
			}
			return 0x08000000;  // 128 MB fallback — larger than any Creation Engine exe
		}

		struct ExprParser
		{
			std::string_view s;
			std::size_t      i = 0;
			bool             ok = true;
			std::string      err;

			void Skip()
			{
				while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
					++i;
			}

			bool Match(char a_c)
			{
				Skip();
				if (i < s.size() && s[i] == a_c) {
					++i;
					return true;
				}
				return false;
			}

			std::uint64_t Term()
			{
				Skip();
				if (i >= s.size()) {
					ok = false;
					err = "unexpected end of address expression";
					return 0;
				}

				if (s[i] == '[') {
					++i;
					const std::uint64_t inner = Expr();
					if (!Match(']')) {
						ok = false;
						err = "missing ']'";
						return 0;
					}
					if (!ok)
						return 0;
					std::uint64_t v = 0;
					if (!SafeRead(reinterpret_cast<const void*>(inner), &v, sizeof(v))) {
						ok = false;
						err = "dereference faulted at " + Hex(inner);
						return 0;
					}
					return v;
				}

				if (s.compare(i, 4, "base") == 0) {
					i += 4;
					return ImageBase();
				}

				const std::size_t start = i;
				int               radix = 10;
				if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) {
					i += 2;
					radix = 16;
				}
				std::uint64_t v = 0;
				const auto    res = std::from_chars(s.data() + i, s.data() + s.size(), v, radix);
				if (res.ec != std::errc{}) {
					ok = false;
					err = "bad number at offset " + std::to_string(start);
					return 0;
				}
				i = static_cast<std::size_t>(res.ptr - s.data());
				return v;
			}

			std::uint64_t Expr()
			{
				std::uint64_t v = Term();
				while (ok) {
					Skip();
					if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
						const char          op = s[i++];
						const std::uint64_t r = Term();
						if (!ok)
							break;
						v = (op == '+') ? v + r : v - r;
					} else {
						break;
					}
				}
				return v;
			}
		};
	}

	bool SafeRead(const void* a_src, void* a_dst, std::size_t a_len) noexcept
	{
		__try {
			std::memcpy(a_dst, a_src, a_len);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool SafeWrite(void* a_dst, const void* a_src, std::size_t a_len) noexcept
	{
		__try {
			std::memcpy(a_dst, a_src, a_len);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::uintptr_t ImageBase()
	{
		static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
		return base;
	}

	std::uintptr_t ImageSize()
	{
		static const std::uintptr_t size = ComputeImageSize(ImageBase());
		return size;
	}

	bool InImage(std::uintptr_t a_va)
	{
		const auto base = ImageBase();
		return a_va >= base && a_va < base + ImageSize();
	}

	bool ResolveAddress(std::string_view a_expr, std::uint64_t& a_out, std::string& a_error)
	{
		ExprParser p{ a_expr };
		const std::uint64_t v = p.Expr();
		if (!p.ok) {
			a_error = p.err;
			return false;
		}
		p.Skip();
		if (p.i != a_expr.size()) {
			a_error = "trailing characters in address expression";
			return false;
		}
		a_out = v;
		return true;
	}
}
