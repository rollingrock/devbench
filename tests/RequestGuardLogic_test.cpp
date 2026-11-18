#include "test_framework.h"

#include "core/RequestGuardLogic.h"

using namespace dvb::RequestGuardLogic;

TEST_CASE("a loopback host is localhost, 127.0.0.1 or [::1], with or without a port")
{
	CHECK(IsLoopbackHost("127.0.0.1"));
	CHECK(IsLoopbackHost("127.0.0.1:8931"));
	CHECK(IsLoopbackHost("localhost:8920"));
	CHECK(IsLoopbackHost("LocalHost"));
	CHECK(IsLoopbackHost("[::1]:8931"));
	CHECK(IsLoopbackHost("[::1]"));
	CHECK(!IsLoopbackHost("evil.example:8931"));
	CHECK(!IsLoopbackHost("localhost.evil.example"));
	CHECK(!IsLoopbackHost("127.0.0.1.evil.example"));
	CHECK(!IsLoopbackHost("127.0.0.1:"));
	CHECK(!IsLoopbackHost("127.0.0.1:89a"));
	CHECK(!IsLoopbackHost("[::1"));
	CHECK(!IsLoopbackHost("[::2]:8931"));
	CHECK(!IsLoopbackHost(""));
}

TEST_CASE("a loopback origin is a page served from loopback")
{
	CHECK(IsLoopbackOrigin("http://127.0.0.1:8931"));
	CHECK(IsLoopbackOrigin("http://localhost:3000"));
	CHECK(IsLoopbackOrigin("https://[::1]"));
	CHECK(!IsLoopbackOrigin("https://evil.example"));
	CHECK(!IsLoopbackOrigin("http://localhost.evil.example"));
	CHECK(!IsLoopbackOrigin("null"));
	CHECK(!IsLoopbackOrigin("file://"));
	CHECK(!IsLoopbackOrigin("http://"));
}

TEST_CASE("a request is refused for a foreign Origin or Host, and answered without either")
{
	CHECK(Refusal(std::nullopt, std::nullopt).empty());
	CHECK(Refusal("127.0.0.1:8931", std::nullopt).empty());
	CHECK(Refusal("127.0.0.1:8931", "http://localhost:3000").empty());
	CHECK(!Refusal("127.0.0.1:8931", "https://evil.example").empty());
	CHECK(!Refusal("127.0.0.1:8931", "null").empty());
	CHECK(!Refusal("rebound.evil.example:8931", std::nullopt).empty());
}
