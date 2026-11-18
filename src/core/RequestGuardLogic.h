#pragma once

#include <optional>
#include <string>
#include <string_view>

// Which HTTP requests the server answers. It listens on 127.0.0.1 with no auth, so a
// web page the user has open is the one remote caller that can still reach it: a
// cross-origin POST needs no preflight to land, and DNS rebinding makes an attacker's
// domain resolve to 127.0.0.1. Both carry a non-loopback Origin or Host header; local
// tools (the bridge, curl, PowerShell, Python) send a loopback Host and no Origin.

namespace dvb::RequestGuardLogic
{
	bool IsLoopbackHost(std::string_view a_host);

	bool IsLoopbackOrigin(std::string_view a_origin);

	std::string Refusal(std::optional<std::string_view> a_host, std::optional<std::string_view> a_origin);
}
