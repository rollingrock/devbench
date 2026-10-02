// Host-independent coverage for mem::RttiClassName / mem::RttiIsA.
//
// These are what the Fallout `nodes` tool uses to decide whether a pointer read
// through a per-runtime struct offset really is a scene-graph node BEFORE it makes a
// single virtual call on it. So the cases that matter most are the refusals: a
// plausible-looking qword that is not an object has to come back as "not a class",
// never as a guess. The test binary's own classes stand in for the game's — the
// reader recovers the image base from the RTTI itself, so the module it runs against
// makes no difference.

#include "test_framework.h"

#include "core/tools/Memory.h"

#include <cstdint>
#include <memory>

namespace rtti_test
{
	struct Base
	{
		virtual ~Base() = default;
		virtual int   Value() const { return 1; }
		std::uint64_t payload = 0x1234;
	};

	class Middle : public Base
	{
	public:
		int Value() const override { return 2; }
	};

	class Leaf final : public Middle
	{
	public:
		int Value() const override { return 3; }
	};

	struct Unrelated
	{
		virtual ~Unrelated() = default;
	};

	// Not polymorphic: its first qword is data, which is exactly what a wrong struct
	// offset hands the reader.
	struct Plain
	{
		std::uint64_t a = 0x0000'7FF6'1234'5678;
		std::uint64_t b = 0;
	};
}

namespace
{
	struct Hidden
	{
		virtual ~Hidden() = default;
	};

	template <class T>
	std::uintptr_t Addr(const T* a_p)
	{
		return reinterpret_cast<std::uintptr_t>(a_p);
	}
}

TEST_CASE("rtti: names the dynamic class, not the static type of the pointer")
{
	std::unique_ptr<rtti_test::Base> leaf = std::make_unique<rtti_test::Leaf>();
	CHECK(leaf->Value() == 3);  // keep the vtable honestly in use
	const auto name = dvb::mem::RttiClassName(Addr(leaf.get()));
	CHECK(name.has_value());
	CHECK_MESSAGE(name.value_or("") == "rtti_test::Leaf", "got '" + name.value_or("<none>") + "'");
}

TEST_CASE("rtti: IsA sees the whole base chain and nothing outside it")
{
	const auto leaf = std::make_unique<rtti_test::Leaf>();
	const auto addr = Addr(leaf.get());
	CHECK(dvb::mem::RttiIsA(addr, "rtti_test::Leaf"));
	CHECK(dvb::mem::RttiIsA(addr, "rtti_test::Middle"));
	CHECK(dvb::mem::RttiIsA(addr, "rtti_test::Base"));
	CHECK(!dvb::mem::RttiIsA(addr, "rtti_test::Unrelated"));
	// Unqualified is a different name: a match has to be exact, not a suffix.
	CHECK(!dvb::mem::RttiIsA(addr, "Leaf"));

	const auto middle = std::make_unique<rtti_test::Middle>();
	CHECK(dvb::mem::RttiIsA(Addr(middle.get()), "rtti_test::Base"));
	CHECK(!dvb::mem::RttiIsA(Addr(middle.get()), "rtti_test::Leaf"));
}

TEST_CASE("rtti: an anonymous-namespace class demangles readably")
{
	const auto hidden = std::make_unique<Hidden>();
	const auto name = dvb::mem::RttiClassName(Addr(hidden.get()));
	CHECK_MESSAGE(name.value_or("") == "`anonymous namespace'::Hidden", "got '" + name.value_or("<none>") + "'");
}

TEST_CASE("rtti: memory that is not a polymorphic object is refused, not guessed at")
{
	const rtti_test::Plain plain;
	CHECK(!dvb::mem::RttiClassName(Addr(&plain)).has_value());
	CHECK(!dvb::mem::RttiIsA(Addr(&plain), "rtti_test::Base"));

	// A forged chain: object -> "vtable" -> "locator" with the right x64 signature (1)
	// and positive RVAs. Everything a vtable check alone looks at holds up; only the
	// image base the locator implies does not, and that is what has to refuse it.
	alignas(16) std::uint64_t forged[8]{};
	forged[0] = Addr(&forged[2]);                       // object: vtable pointer
	forged[1] = Addr(&forged[4]);                       // vtable[-1]: the "locator"
	forged[4] = 1;                                      // signature 1, offset 0
	forged[5] = std::uint64_t{ 0x10 } << 32;            // cdOffset 0, typeDescriptor 0x10
	forged[6] = 0x10 | (std::uint64_t{ 0x100 } << 32);  // classDescriptor 0x10, self 0x100
	CHECK(!dvb::mem::RttiClassName(Addr(&forged[0])).has_value());
	CHECK(!dvb::mem::RttiIsA(Addr(&forged[0]), "rtti_test::Base"));

	CHECK(!dvb::mem::RttiClassName(0).has_value());
	CHECK(!dvb::mem::RttiClassName(0x10).has_value());
	CHECK(!dvb::mem::RttiClassName(0x0000'7FFF'FFFF'0000).has_value());  // unmapped: SafeRead, not a crash

	// Misaligned by one byte into a real object: a wrong offset, off by a little.
	const auto leaf = std::make_unique<rtti_test::Leaf>();
	CHECK(!dvb::mem::RttiClassName(Addr(leaf.get()) + 1).has_value());
}
