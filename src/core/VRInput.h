#pragma once

#include "core/Json.h"

#include <string>
#include <string_view>

namespace dvb
{
	class EventBus;
	struct ToolContext;

	/// Lists the shared actions plus the platform's own (vrhost::PlatformActions).
	json VRInputCapabilities();
	json HandleVRInput(const json& a_args, const ToolContext& a_ctx);
	/// The owner a vrTrackedSet action acts as: 'owner', else the MCP session, else rest:anonymous.
	std::string ResolveVRInputOwner(const json& a_args, const ToolContext& a_ctx);
	bool        IsVRInputDevice(std::string_view a_device);
	/// Whether the running sequence belongs to a_owner under a_controlToken, so a sequence
	/// carrying that token replaces it in place.
	bool VRSequenceHeldBy(std::string_view a_owner, std::string_view a_controlToken);

	// Installs process-lifetime pass-through OpenVR interface hooks. They alter data only while
	// one owned atomic sequence is active; otherwise every call reaches the original runtime.
	// Returns true once installed; idempotent, so a platform may retry until OpenVR is up.
	bool MarkVRInputReady(EventBus& a_events);
	void ReleaseVRInputForLifecycle(const char* a_reason);
}
