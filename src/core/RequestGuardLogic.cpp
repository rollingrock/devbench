#include "core/RequestGuardLogic.h"

#include <algorithm>
#include <cctype>

namespace dvb::RequestGuardLogic
{
	namespace
	{
		bool EqualsNoCase(std::string_view a_left, std::string_view a_right)
		{
			return std::ranges::equal(a_left, a_right, [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
		}

		/// Whether the text after a host is empty or ":<digits>".
		bool IsPortSuffix(std::string_view a_rest)
		{
			if (a_rest.empty())
				return true;
			if (a_rest.front() != ':' || a_rest.size() == 1)
				return false;
			return std::ranges::all_of(a_rest.substr(1), [](char c) { return c >= '0' && c <= '9'; });
		}
	}

	/// Whether a Host header ("host[:port]") names this machine's loopback: localhost,
	/// 127.0.0.1 or [::1]. Only those three; another 127.x address or a hostname is refused.
	bool IsLoopbackHost(std::string_view a_host)
	{
		if (a_host.starts_with('[')) {
			const auto close = a_host.find(']');
			return close != std::string_view::npos && a_host.substr(1, close - 1) == "::1" && IsPortSuffix(a_host.substr(close + 1));
		}
		const auto colon = a_host.find(':');
		const auto name = a_host.substr(0, colon);
		const auto rest = colon == std::string_view::npos ? std::string_view{} : a_host.substr(colon);
		return (EqualsNoCase(name, "localhost") || name == "127.0.0.1") && IsPortSuffix(rest);
	}

	/// Whether an Origin header ("scheme://host[:port]") is a page served from loopback.
	/// "null" (a sandboxed frame, a file:// page) is not.
	bool IsLoopbackOrigin(std::string_view a_origin)
	{
		for (const std::string_view scheme : { "http://", "https://" })
			if (a_origin.size() > scheme.size() && EqualsNoCase(a_origin.substr(0, scheme.size()), scheme))
				return IsLoopbackHost(a_origin.substr(scheme.size()));
		return false;
	}

	/// Why a request is refused, or empty when it is answered. A missing header passes:
	/// browsers always send Host, and send Origin on every request that could change state.
	std::string Refusal(std::optional<std::string_view> a_host, std::optional<std::string_view> a_origin)
	{
		if (a_origin && !IsLoopbackOrigin(*a_origin))
			return "devbench answers no request from a web page that is not served from loopback (Origin: " + std::string(*a_origin) + ")";
		if (a_host && !IsLoopbackHost(*a_host))
			return "devbench answers only requests addressed to 127.0.0.1, localhost or [::1] (Host: " + std::string(*a_host) + ")";
		return {};
	}
}
