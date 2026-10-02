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

		// MSVC x64 RTTI as the compiler lays it out. Every reference inside is an
		// image-relative RVA, and the locator's RVA of itself (`self`) is what recovers
		// the image base — so this works for a class from any module, not only the exe.
		struct CompleteObjectLocator
		{
			std::uint32_t signature;  // 1 on x64; 0 is the x86 layout (absolute pointers)
			std::uint32_t offset;
			std::uint32_t cdOffset;
			std::int32_t  typeDescriptor;
			std::int32_t  classDescriptor;
			std::int32_t  self;
		};

		struct ClassHierarchyDescriptor
		{
			std::uint32_t signature;
			std::uint32_t attributes;
			std::uint32_t numBaseClasses;
			std::int32_t  baseClassArray;  // RVA of int32 RVAs, one per BaseClassDescriptor
		};

		// The object's first qword is a vtable, vtable[-1] a locator, and the base the
		// locator implies is a loaded PE image. The last check is the one that matters:
		// without it any aligned qword pair that happens to read as `1` qualifies.
		bool ReadLocator(std::uintptr_t a_object, CompleteObjectLocator& a_col, std::uintptr_t& a_moduleBase)
		{
			if (a_object < 0x10000 || (a_object & 7) != 0)
				return false;
			std::uintptr_t vtable = 0;
			if (!SafeRead(reinterpret_cast<const void*>(a_object), &vtable, sizeof(vtable)) || vtable < 0x10000 || (vtable & 7) != 0)
				return false;
			std::uintptr_t colVA = 0;
			if (!SafeRead(reinterpret_cast<const void*>(vtable - 8), &colVA, sizeof(colVA)) || colVA < 0x10000)
				return false;
			if (!SafeRead(reinterpret_cast<const void*>(colVA), &a_col, sizeof(a_col)) || a_col.signature != 1 || a_col.self <= 0)
				return false;
			a_moduleBase = colVA - static_cast<std::uint32_t>(a_col.self);
			std::uint16_t mz = 0;
			return (a_moduleBase & 0xFFFF) == 0 &&
			       SafeRead(reinterpret_cast<const void*>(a_moduleBase), &mz, sizeof(mz)) && mz == 0x5A4D;
		}

		// TypeDescriptor::name, at +0x10: ".?AVNiNode@@". Read a byte at a time so a
		// name ending just before an unreadable page is not refused for what follows it.
		bool ReadMangledName(std::uintptr_t a_typeDescriptor, std::string& a_out)
		{
			a_out.clear();
			for (std::size_t i = 0; i < 512; ++i) {
				char c = 0;
				if (!SafeRead(reinterpret_cast<const void*>(a_typeDescriptor + 0x10 + i), &c, 1))
					return false;
				if (c == '\0')
					return a_out.starts_with(".?A");
				a_out.push_back(c);
			}
			return false;
		}

		// ".?AVNiNode@@" -> "NiNode", ".?AUFoo@ns@@" -> "ns::Foo". Plain class and struct
		// names only: a template ("?$") comes back past the prefix, undecoded, which is
		// still unique and still readable enough to act on.
		std::string Demangle(std::string_view a_mangled)
		{
			std::string_view s = a_mangled.substr(4);  // ".?AV" (class) / ".?AU" (struct)
			if (!s.ends_with("@@") || s.find("?$") != std::string_view::npos)
				return std::string(s);
			s.remove_suffix(2);
			std::string out;
			while (!s.empty()) {
				const auto at = s.rfind('@');
				const auto part = (at == std::string_view::npos) ? s : s.substr(at + 1);
				if (!out.empty())
					out += "::";
				out += part.starts_with("?A") ? "`anonymous namespace'" : std::string(part);
				if (at == std::string_view::npos)
					break;
				s = s.substr(0, at);
			}
			return out;
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

	std::optional<std::string> RttiClassName(std::uintptr_t a_object)
	{
		CompleteObjectLocator col{};
		std::uintptr_t        base = 0;
		std::string           mangled;
		if (!ReadLocator(a_object, col, base) || col.typeDescriptor <= 0 || !ReadMangledName(base + col.typeDescriptor, mangled))
			return std::nullopt;
		return Demangle(mangled);
	}

	bool RttiIsA(std::uintptr_t a_object, std::string_view a_class)
	{
		CompleteObjectLocator    col{};
		std::uintptr_t           base = 0;
		ClassHierarchyDescriptor chd{};
		if (!ReadLocator(a_object, col, base) || col.classDescriptor <= 0 ||
			!SafeRead(reinterpret_cast<const void*>(base + col.classDescriptor), &chd, sizeof(chd)) ||
			chd.numBaseClasses == 0 || chd.numBaseClasses > 256 || chd.baseClassArray <= 0)
			return false;
		// Entry 0 is the class itself, the rest its bases, each a BaseClassDescriptor
		// whose first field is the RVA of that class's TypeDescriptor.
		std::string mangled;
		for (std::uint32_t i = 0; i < chd.numBaseClasses; ++i) {
			std::int32_t bcd = 0;
			std::int32_t td = 0;
			if (!SafeRead(reinterpret_cast<const void*>(base + chd.baseClassArray + i * 4), &bcd, sizeof(bcd)) || bcd <= 0 ||
				!SafeRead(reinterpret_cast<const void*>(base + bcd), &td, sizeof(td)) || td <= 0 ||
				!ReadMangledName(base + td, mangled))
				return false;
			if (Demangle(mangled) == a_class)
				return true;
		}
		return false;
	}

	bool ResolveAddress(std::string_view a_expr, std::uint64_t& a_out, std::string& a_error)
	{
		ExprParser          p{ a_expr };
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
