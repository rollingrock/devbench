#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Guarded process-memory access + the address-expression grammar the `memory` tool
// speaks. Split out of CommonTools.cpp because the SEH-guarded readers must live in
// functions with no C++ unwinding (MSVC forbids __try alongside objects needing
// destruction), and because this is the piece worth unit-testing on its own.
//
// PROVENANCE: this capability comes from the Fallout 4 VR scope investigation's
// in-plugin bench, where the debug loop was "attach x64dbg, find the struct, read
// eight bytes, detach". A reverse-engineering session spends most of its time
// reading and poking engine memory, and doing that over the same local endpoint as
// everything else is what makes an agent able to run the loop unattended.

namespace dvb::mem
{
	/// The running executable's image base and size, from its PE optional header.
	/// Cached; never throws. A heap pointer that merely happens to sit above the base
	/// is NOT in the image — callers use InImage to decide whether reporting an RVA
	/// would be meaningful or a plausible-looking lie.
	std::uintptr_t ImageBase();
	std::uintptr_t ImageSize();
	bool           InImage(std::uintptr_t a_va);

	/// SEH-guarded copies. A bad pointer from a bench request must never take the game
	/// down, so these return false instead of raising. noexcept and free of anything
	/// needing destruction, which is what lets them use __try/__except at all.
	bool SafeRead(const void* a_src, void* a_dst, std::size_t a_len) noexcept;
	bool SafeWrite(void* a_dst, const void* a_src, std::size_t a_len) noexcept;

	/// Resolve an address expression.
	///
	///   expr := term (('+' | '-') term)*
	///   term := '[' expr ']' | 'base' | 0xHEX | DEC
	///
	/// `base` is the executable's load base and `[x]` dereferences a qword, so an RVA
	/// out of a Ghidra session or a session note pastes in verbatim:
	///   base+0x1d98ff0        -> a function
	///   [base+0x6239340]+4    -> a field of a singleton reached through its pointer
	/// A dereference that faults fails the parse with the faulting address named,
	/// rather than returning a number that looks like an answer.
	bool ResolveAddress(std::string_view a_expr, std::uint64_t& a_out, std::string& a_error);
}
