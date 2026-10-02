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
		auto getApi = reinterpret_cast<void* (*)(unsigned int)>(dvb::HostApi::GetApiEntry());
		return static_cast<DevBenchAPI::IDevBenchInterface001*>(getApi(1));
	}
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
