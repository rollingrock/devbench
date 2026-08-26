#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	// Provider side of the cross-plugin C-ABI (DevBenchAPI). Lets other script-extender
	// plugins register tools / emit events into this host's registry and event bus.
	//
	// GAME-AGNOSTIC as of the multigame split: this lives in the core and takes the three
	// message fields it actually uses, rather than an SKSE::MessagingInterface::Message*.
	// SKSE's and F4SE's Message structs carry the same {sender, type, dataLen, data} — but
	// they are unrelated types from unrelated headers, and naming either one here would
	// put a script-extender include in src/core/, which is the invariant the unit-test
	// target exists to protect (docs/MULTIGAME.md). Each platform's listener unpacks its
	// own struct: a three-line adapter per game instead of a per-game copy of the whole
	// provider.
	namespace HostApi
	{
		// Wire the host interface to the registry + bus. Call once before consumers
		// request the interface (Skyrim: at kPostLoad; Fallout: at kGameDataReady).
		//
		// a_buildNumber is MAJOR*10000 + MINOR*100 + PATCH, passed in rather than read
		// from the generated Version.h so the core stays independent of the build's
		// configured headers — the unit-test target links core files with no Version.h
		// on its include path.
		void Init(ToolRegistry& a_registry, EventBus& a_events, unsigned int a_buildNumber);

		// Load-order-proof alternative to the messaging handshake: the platform
		// exports a plain C function that returns this same GetApi pointer, so a
		// consumer can GetProcAddress its way to the interface when the message
		// route fails. Field-found 2026-08-26 (FO4VR): the extender's
		// RegisterListener de-dupes by listener handle, so the kPostLoad
		// re-register never reaches the slot of a consumer that loaded later -
		// the dispatch finds zero respondents forever.
		[[nodiscard]] void* GetApiEntry();

		// Handle a DevBenchMessage::kMessage_GetInterface request. Call from the
		// platform's message listener for every message — it no-ops unless it is the
		// request. a_sender may be null.
		void OnInterfaceRequest(std::uint32_t a_type, void* a_data, const char* a_sender);

		// A plugin that requested the C-ABI interface (one entry per GetInterface call — a
		// plugin that calls it more than once appears more than once, oldest first). NOTE: the
		// requested revision is NOT captured here — GetApiFunction is a pointer the consumer
		// calls itself, later, on its own; devbench never observes that call or its argument,
		// only that the pointer was handed out. Reporting a revision here would be a guess
		// dressed up as an observation.
		struct Consumer
		{
			std::string   name;  // a_message->sender, or "<?>" if unset
			long long     atEpoch;
			std::uint32_t atFrame;
		};

		// A successful RegisterTool/RegisterToolExtension call over the C-ABI.
		struct Registration
		{
			std::string   kind;  // "tool" | "extension"
			std::string   name;  // tool name, or "<baseTool>:<key>"
			long long     atEpoch;
			std::uint32_t atFrame;
			bool          replaced;  // true if this call overwrote an earlier registration
		};

		// Every GetInterface request seen so far, oldest first. Thread-safe.
		std::vector<Consumer> Consumers();

		// Every successful tool/extension registration seen so far, oldest first. Thread-safe.
		// There is no reliable per-registration caller identity (the C-ABI interface is one
		// shared singleton — see ROADMAP.md's "Event source tagging" item), so this cannot be
		// joined against Consumers() by plugin name; both lists are exposed side by side instead.
		std::vector<Registration> Registrations();
	}
}
