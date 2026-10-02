#include "test_framework.h"

#include "DevBenchAPI.h"
#include "core/EventBus.h"
#include "core/HostApi.h"
#include "core/ToolRegistry.h"

#include <cmath>

namespace
{
	DevBenchAPI::IDevBenchInterface001* Interface()
	{
		auto getApi = reinterpret_cast<void* (*)(unsigned int)>(dvb::HostApi::GetApiEntry(nullptr));
		return static_cast<DevBenchAPI::IDevBenchInterface001*>(getApi(1));
	}

	void MarkerInThisModule() {}
}

TEST_CASE("HostApi records an export-route request as a consumer named by the calling module")
{
	// The case this exists for: FRIK reaches the C-ABI through the DLL export, not the
	// extender message, and `registrants` used to list its registrations with no consumer.
	const auto before = dvb::HostApi::Consumers().size();
	CHECK(dvb::HostApi::GetApiEntry(reinterpret_cast<const void*>(&MarkerInThisModule)) != nullptr);
	const auto after = dvb::HostApi::Consumers();
	CHECK(after.size() == before + 1);
	const auto& c = after.back();
	CHECK(c.route == "export");
	CHECK_MESSAGE(c.name == "devbench-tests.exe", "consumer named '" + c.name + "'");

	(void)dvb::HostApi::GetApiEntry(nullptr);
	CHECK(dvb::HostApi::Consumers().back().name == "<?>");
}

TEST_CASE("HostApi records a message-route request with its sender and route")
{
	DevBenchAPI::DevBenchMessage message;
	dvb::HostApi::OnInterfaceRequest(DevBenchAPI::DevBenchMessage::kMessage_GetInterface, &message, "SomeMod");
	CHECK(message.GetApiFunction != nullptr);
	const auto c = dvb::HostApi::Consumers().back();  // a copy: Consumers() returns by value
	CHECK(c.name == "SomeMod");
	CHECK(c.route == "message");
}

TEST_CASE("HostApi's self-test tool leaves the built-in ping in place")
{
	// Init runs after the built-in tools are registered. The self-test used to register
	// as "ping" and replace the core one, so every game answered { pong, echo }.
	dvb::ToolRegistry   registry;
	dvb::EventBus       events;
	dvb::ToolDescriptor ping;
	ping.name = "ping";
	ping.description = "built-in";
	registry.Register(std::move(ping), [](const dvb::json&, const dvb::ToolContext&) { return dvb::json{ { "ok", true } }; });

	dvb::HostApi::Init(registry, events, 12200);

	CHECK(registry.Describe("ping")->description == "built-in");
	const auto builtin = registry.Invoke("ping", dvb::json::object(), {});
	CHECK(builtin.value["ok"] == true);
	const auto selftest = registry.Invoke("devbench.selftest", dvb::json{ { "foo", 7 } }, {});
	CHECK(selftest.ok);
	CHECK(selftest.value["pong"] == true);
	CHECK(selftest.value["echo"]["foo"] == 7);
}

TEST_CASE("HostApi rejects time-scale requests when the platform has no implementation")
{
	dvb::ToolRegistry registry;
	dvb::EventBus     events;
	dvb::HostApi::Init(registry, events, 12200);
	auto* api = Interface();
	CHECK(api != nullptr);
	CHECK(api->GetBuildNumber() == 12200);
	CHECK(!api->SetTimeScale(2.0F, 1000, "test"));
	CHECK(std::isnan(api->GetTimeScale()));
}

TEST_CASE("HostApi forwards time-scale requests and clears callbacks on reinitialization")
{
	dvb::ToolRegistry registry;
	dvb::EventBus     events;
	dvb::HostApi::Init(registry, events, 12200, {
													.set = +[](float scale, std::uint32_t leaseMs, const char* owner) { return scale == 2.0F && leaseMs == 1234 && owner && std::string(owner) == "consumer"; },
													.get = +[]() { return 1.5F; },
												});
	auto* api = Interface();
	CHECK(api->SetTimeScale(2.0F, 1234, "consumer"));
	CHECK(!api->SetTimeScale(2.0F, 1234, nullptr));
	CHECK(api->GetTimeScale() == 1.5F);
	dvb::HostApi::Init(registry, events, 12200);
	CHECK(!api->SetTimeScale(2.0F, 1234, "consumer"));
	CHECK(std::isnan(api->GetTimeScale()));
}
