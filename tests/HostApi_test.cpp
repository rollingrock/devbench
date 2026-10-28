#include "test_framework.h"

#include "DevBenchAPI.h"
#include "core/EventBus.h"
#include "core/HostApi.h"
#include "core/ToolRegistry.h"
#include "test_platform.h"

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

// The main-thread service (DevBenchAPI.h, DevBenchMainThreadApi): its own message, the
// host's MainThread::Post queue behind it. PlatformStubs.cpp holds posted functions until
// DrainPosted(), the way a frame would.
TEST_CASE("the main-thread service comes from its own message and runs callbacks only when the queue drains")
{
	DevBenchAPI::DevBenchMainThreadMessage message;
	dvb::HostApi::OnInterfaceRequest(DevBenchAPI::DevBenchMainThreadMessage::kMessage_GetMainThread, &message, "SomeMod");
	CHECK(message.api != nullptr);
	if (!message.api)
		return;
	CHECK(message.api->version == 1);
	CHECK(dvb::HostApi::Consumers().back().route == "main-thread message");

	static int   hits = 0;
	static void* seen = nullptr;
	int          token = 0;
	hits = 0;
	dvb::test::DrainPosted();  // nothing left over from another case
	CHECK(message.api->RunOnMainThread(+[](void* a_ctx) { ++hits; seen = a_ctx; }, &token));
	CHECK(hits == 0);  // queued, not run inline
	CHECK(dvb::test::DrainPosted() == 1);
	CHECK(hits == 1);
	CHECK(seen == &token);
	CHECK(!message.api->RunOnMainThread(nullptr, &token));
	CHECK(dvb::test::DrainPosted() == 0);
}

TEST_CASE("the main-thread service reports confinement as the platform states it")
{
	DevBenchAPI::DevBenchMainThreadMessage message;
	dvb::HostApi::OnInterfaceRequest(DevBenchAPI::DevBenchMainThreadMessage::kMessage_GetMainThread, &message, "SomeMod");
	if (!message.api)
		return;
	dvb::test::g_confined = false;
	CHECK(!message.api->ConfinedToMainThread());
	dvb::test::g_confined = true;
	CHECK(message.api->ConfinedToMainThread());
	dvb::test::g_confined = false;
}

TEST_CASE("an unknown message leaves the main-thread service unset, and the export is the same service")
{
	// What a devbench without the service does with the request: nothing. The consumer
	// then sees a null api, not a call into something that is not there.
	DevBenchAPI::DevBenchMainThreadMessage message;
	dvb::HostApi::OnInterfaceRequest(0x12345678u, &message, "SomeMod");
	CHECK(message.api == nullptr);

	DevBenchAPI::DevBenchMainThreadMessage viaMessage;
	dvb::HostApi::OnInterfaceRequest(DevBenchAPI::DevBenchMainThreadMessage::kMessage_GetMainThread, &viaMessage, "SomeMod");
	const auto* viaExport = dvb::HostApi::GetMainThreadEntry(reinterpret_cast<const void*>(&MarkerInThisModule));
	CHECK(viaExport == viaMessage.api);
	CHECK(dvb::HostApi::Consumers().back().route == "main-thread export");
}
