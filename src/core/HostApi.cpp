#include "core/HostApi.h"

#include "DevBenchAPI.h"
#include "core/EventBus.h"
#include "core/GameState.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/ToolExtensions.h"
#include "core/ToolRegistry.h"

#include <Windows.h>

#include <ctime>
#include <limits>
#include <mutex>
#include <string_view>

namespace dvb::HostApi
{
	namespace
	{
		TimeScaleCallbacks g_timeScale;
		ToolRegistry*      g_registry = nullptr;
		EventBus*          g_events = nullptr;
		unsigned int       g_buildNumber = 0;  // supplied by the platform at Init

		// Registrant ledger: who asked for the C-ABI interface, and what they registered
		// through it. Both grow only (append-only, process lifetime) — a plugin unregistering
		// mid-session isn't a thing the C-ABI supports, so there's nothing to remove. One mutex
		// covers both since they're written from the same call sites and read together by
		// `inspect kind=registrants`.
		std::mutex                g_ledgerMutex;
		std::vector<Consumer>     g_consumers;
		std::vector<Registration> g_registrations;

		void NoteConsumer(std::string a_name, const char* a_route)
		{
			std::lock_guard<std::mutex> lock(g_ledgerMutex);
			g_consumers.push_back(Consumer{
				std::move(a_name),
				static_cast<long long>(std::time(nullptr)),
				static_cast<std::uint32_t>(game::CurrentFrame()),
				a_route,
			});
		}

		// File name of the module that contains a_address ("FRIK.dll"), or "<?>". The
		// address is a return address into the caller, so this names whoever called.
		std::string ModuleNameAt(const void* a_address)
		{
			HMODULE module = nullptr;
			if (!a_address ||
				!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCWSTR>(a_address), &module))
				return "<?>";
			wchar_t     path[MAX_PATH]{};
			const DWORD length = ::GetModuleFileNameW(module, path, MAX_PATH);
			if (length == 0)
				return "<?>";
			std::wstring_view full(path, length);
			const auto        slash = full.find_last_of(L"\\/");
			const auto        file = slash == std::wstring_view::npos ? full : full.substr(slash + 1);
			const int         bytes = ::WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), nullptr, 0, nullptr, nullptr);
			std::string       out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
			if (bytes <= 0 ||
				::WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), out.data(), bytes, nullptr, nullptr) != bytes)
				return "<?>";
			return out;
		}

		void NoteRegistration(std::string a_kind, std::string a_name, bool a_replacedExisting)
		{
			std::lock_guard<std::mutex> lock(g_ledgerMutex);
			g_registrations.push_back(Registration{
				std::move(a_kind),
				std::move(a_name),
				static_cast<long long>(std::time(nullptr)),
				static_cast<std::uint32_t>(game::CurrentFrame()),
				a_replacedExisting,
			});
		}

		// Wrap a consumer's C callback (fn + ctx) as a ToolHandler: args in as a JSON string, result
		// collected via our host-owned sink. Nothing C++ crosses the DLL boundary — only const char*
		// and function pointers. Shared by RegisterTool and RegisterMenuHandler.
		ToolHandler MakeHandler(DevBenchAPI::ToolFn a_handler, void* a_ctx)
		{
			return [a_handler, a_ctx](const json& a_args, const ToolContext&) -> json {
				std::string          result;
				DevBenchAPI::WriteFn write = +[](void* a_sink, const char* a_json) {
					*static_cast<std::string*>(a_sink) = a_json ? a_json : "";
				};
				a_handler(a_ctx, a_args.dump().c_str(), &result, write);
				if (result.empty())
					return json::object();
				try {
					return json::parse(result);
				} catch (...) {
					return json{ { "raw", result } };
				}
			};
		}

		// Concrete implementation of the published C-ABI interface, wired to the
		// host's registry + bus.
		struct Interface : DevBenchAPI::IDevBenchInterface001
		{
			unsigned int GetBuildNumber() override
			{
				return g_buildNumber;
			}

			bool RegisterTool(const char* a_name, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				if (!g_registry || !a_name || !a_handler)
					return false;

				json desc = json::object();
				if (a_descriptorJson) {
					try {
						desc = json::parse(a_descriptorJson);
					} catch (...) {
					}
				}
				ToolDescriptor d;
				d.name = a_name;
				d.description = desc.value("description", std::string{});
				d.inputSchema = desc.value("inputSchema", DefaultInputSchema());
				d.readOnly = desc.value("readOnly", false);

				const bool isNew = g_registry->Register(std::move(d), MakeHandler(a_handler, a_ctx));
				NoteRegistration("tool", a_name, !isNew);
				return isNew;
			}

			bool RegisterMenuHandler(const char* a_menuName, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				// Menus are just the first base tool that accepts extensions — delegate.
				return RegisterToolExtension("menu", a_menuName, a_descriptorJson, a_handler, a_ctx);
			}

			bool RegisterToolExtension(const char* a_baseTool, const char* a_key, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				if (!a_baseTool || !*a_baseTool || !a_key || !*a_key || !a_handler)
					return false;
				json desc = json::object();
				if (a_descriptorJson) {
					try {
						desc = json::parse(a_descriptorJson);
					} catch (...) {
					}
				}
				if (!desc.is_object())  // a scalar/array descriptor would break the descriptor object contract
					desc = json::object();
				const bool isNew = ToolExtensions::Register(a_baseTool, a_key, std::move(desc), MakeHandler(a_handler, a_ctx));
				NoteRegistration("extension", std::string(a_baseTool) + ":" + a_key, !isNew);
				return isNew;
			}

			void EmitEvent(const char* a_topic, const char* a_payloadJson) override
			{
				if (!g_events || !a_topic)
					return;
				json payload = json::object();
				if (a_payloadJson) {
					try {
						payload = json::parse(a_payloadJson);
					} catch (...) {
					}
				}
				g_events->Publish(a_topic, std::move(payload));
			}

			bool SetTimeScale(float a_scale, std::uint32_t a_leaseMs, const char* a_owner) override
			{
				return g_timeScale.set && g_timeScale.set(a_scale, a_leaseMs, a_owner);
			}

			float GetTimeScale() override
			{
				return g_timeScale.get ? g_timeScale.get() : std::numeric_limits<float>::quiet_NaN();
			}
		};

		Interface g_interface;

		void* GetApi(unsigned int a_revision)
		{
			// Only revision 1 exists; future revisions return a derived interface.
			return a_revision >= 1 ? static_cast<DevBenchAPI::IDevBenchInterface001*>(&g_interface) : nullptr;
		}

		// Self-test: register a trivial tool THROUGH the public interface, proving the
		// C-callback + JSON round-trip path end to end without a separate consumer.
		// Its own dotted name, like any mod tool: Init runs after the built-in tools are
		// registered and Register replaces by name, so this used to be called "ping" and
		// silently took the core `ping` ({ ok, game, exe, vr }) over on every game.
		void RegisterSelfTest()
		{
			static constexpr const char* desc =
				R"({"description":"devbench C-ABI self-test; echoes its args.","inputSchema":{"type":"object"},"readOnly":true})";
			g_interface.RegisterTool("devbench.selftest", desc, +[](void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write) {
					const std::string args = (a_argsJson && *a_argsJson) ? a_argsJson : "{}";
					const std::string out = R"({"pong":true,"echo":)" + args + "}";
					a_write(a_sink, out.c_str()); }, nullptr);
		}

		// Shared echo handler for the extension self-tests (the ping pattern, one level down).
		void EchoExtension(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			const std::string args = (a_argsJson && *a_argsJson) ? a_argsJson : "{}";
			const std::string out = R"({"invoked":true,"echo":)" + args + "}";
			a_write(a_sink, out.c_str());
		}

		// Self-test the extension path THROUGH the public interface under two base tools — `menu`
		// (via the RegisterMenuHandler alias) and `inspect` (via the general RegisterToolExtension) —
		// so `menu invoke name=devbench.selftest` and `inspect kind=devbench.selftest` round-trip the
		// C-callback without a separate consumer mod, proving the mechanism generalizes.
		void RegisterExtensionSelfTests()
		{
			g_interface.RegisterMenuHandler("devbench.selftest",
				R"({"description":"devbench menu-extension self-test; echoes its args."})", &EchoExtension, nullptr);
			g_interface.RegisterToolExtension("inspect", "devbench.selftest",
				R"({"description":"devbench inspect-extension self-test; echoes its args."})", &EchoExtension, nullptr);
		}
	}

	void Init(ToolRegistry& a_registry, EventBus& a_events, unsigned int a_buildNumber, TimeScaleCallbacks a_timeScale)
	{
		g_registry = &a_registry;
		g_events = &a_events;
		g_buildNumber = a_buildNumber;
		g_timeScale = a_timeScale;
		RegisterSelfTest();
		RegisterExtensionSelfTests();
	}

	void* GetApiEntry(const void* a_callerAddress)
	{
		auto name = ModuleNameAt(a_callerAddress);
		dlog::info("devbench: provided plugin interface to {} (export)", name);
		NoteConsumer(std::move(name), "export");
		return reinterpret_cast<void*>(&GetApi);
	}

	void OnInterfaceRequest(std::uint32_t a_type, void* a_data, const char* a_sender)
	{
		if (a_type == DevBenchAPI::DevBenchMessage::kMessage_GetInterface && a_data) {
			static_cast<DevBenchAPI::DevBenchMessage*>(a_data)->GetApiFunction = GetApi;
			NoteConsumer(a_sender ? std::string(a_sender) : std::string("<?>"), "message");
			dlog::info("devbench: provided plugin interface to {}", a_sender ? a_sender : "<?>");
		}
	}

	std::vector<Consumer> Consumers()
	{
		std::lock_guard<std::mutex> lock(g_ledgerMutex);
		return g_consumers;
	}

	std::vector<Registration> Registrations()
	{
		std::lock_guard<std::mutex> lock(g_ledgerMutex);
		return g_registrations;
	}
}
