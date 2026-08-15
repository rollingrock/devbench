#include "Tools_Fallout4.h"

#include "core/EventBus.h"
#include "core/GameState.h"
#include "core/Json.h"
#include "core/MainThread.h"
#include "core/Server.h"
#include "core/ToolRegistry.h"

#include "Version.h"

// Fallout 4 / Fallout 4 VR game tools.
//
// These deliberately mirror the SHAPE of the Skyrim tools (same names, same action
// dispatch, same result keys where the concept exists in both games) so a client or
// agent script written against one game keeps working against the other. Where a
// concept genuinely differs, the key differs too rather than being faked — a caller
// must be able to tell "Fallout has no equivalent" from "the value is empty".
//
// Everything here runs through MainThread::RunAndWait: the engine's forms and UI are
// main-thread objects, and the listener thread reading them directly is a data race
// that happens to work until it doesn't.

namespace dvb
{
	namespace
	{
		// A form's display name, falling back to the editor ID and then to the form ID,
		// so a result never contains an empty string that a caller has to guess about.
		std::string FormLabel(RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			if (const auto name = RE::TESFullName::GetFullName(*a_form); !name.empty())
				return std::string{ name };
			if (const auto* ed = a_form->GetFormEditorID(); ed && *ed)
				return ed;
			return std::format("0x{:08X}", a_form->GetFormID());
		}

		json StateSnapshot()
		{
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const json identity = InstanceIdentity();
			json       out{
				      { "plugin", "devbench" },
				      { "version", DEVBENCH_VERSION_STRING },
				      { "playerLoaded", player != nullptr },
				      { "frame", game::CurrentFrame() },
			};
			// pid/port/exe/vr/game/extender — the identity block is shared with
			// GET /api/health so both answers can never disagree about who replied.
			for (const auto& [k, v] : identity.items())
				out[k] = v;
			return out;
		}

		json SceneSnapshot()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				throw ToolError(409, "inspect scene: no player (no save loaded yet)");

			json out = json::object();
			const auto pos = player->GetPosition();
			out["position"] = json{ { "x", pos.x }, { "y", pos.y }, { "z", pos.z } };

			if (auto* cell = player->GetParentCell()) {
				out["cell"] = FormLabel(cell);
				out["cellFormId"] = std::format("0x{:08X}", cell->GetFormID());
				out["interior"] = cell->IsInterior();
				// worldSpace shares a union with tempDataOffset and is only meaningful
				// for an exterior cell — reading it for an interior would report a small
				// integer as a pointer.
				if (!cell->IsInterior() && cell->worldSpace)
					out["worldspace"] = FormLabel(cell->worldSpace);
			}

			if (const auto* calendar = RE::Calendar::GetSingleton()) {
				if (calendar->gameHour)
					out["gameHour"] = calendar->gameHour->value;
				if (calendar->gameDaysPassed)
					out["daysPassed"] = calendar->gameDaysPassed->value;
			}
			return out;
		}

		json PlayerSnapshot()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				throw ToolError(409, "inspect player: no player (no save loaded yet)");

			json out{
				{ "formId", std::format("0x{:08X}", player->GetFormID()) },
				{ "level", player->GetLevel() },
			};
			if (const auto* name = player->GetDisplayFullName(); name && *name)
				out["name"] = name;
			return out;
		}

		json InspectHandler(const json& a_args, const ToolContext&)
		{
			const std::string kind = a_args.value("kind", std::string("state"));

			// 'health' is the ONLY kind answered without the main thread — that is what
			// makes it useful. A hung main thread is exactly when you most want an
			// answer, and every other kind would time out with a 504.
			if (kind == "health") {
				json out = InstanceIdentity();
				out["frame"] = game::CurrentFrame();
				out["lastTaskFrame"] = MainThread::LastCompletedFrame();
				out["pendingTasks"] = MainThread::PendingTasks();
				return out;
			}

			return MainThread::RunAndWait([kind]() -> json {
				if (kind == "state")
					return StateSnapshot();
				if (kind == "scene")
					return SceneSnapshot();
				if (kind == "player")
					return PlayerSnapshot();
				throw ToolError(400, "inspect: unknown kind '" + kind + "' (state|health|scene|player)");
			});
		}

		json ConsoleHandler(const json& a_args, const ToolContext&)
		{
			const std::string command = a_args.value("command", std::string{});
			if (command.empty())
				throw ToolError(400, "console: 'command' is required");

			// Fire-and-forget on purpose. Fallout 4's console has no ConsoleLog fencing
			// equivalent wired up here yet, so promising a captured result would be a
			// promise this cannot keep — say plainly that the output is not returned
			// rather than returning an empty 'lines' array that reads like "no output".
			MainThread::RunAndWait([command]() -> json {
				RE::Console::ExecuteCommand(command.c_str());
				return json{ { "queued", true } };
			});
			return json{
				{ "executed", true },
				{ "command", command },
				{ "note", "output is not captured on Fallout 4 yet — see the `log` tool, or read the in-game console" }
			};
		}
	}

	void RegisterGameTools(ToolRegistry& a_registry, EventBus&)
	{
		{
			ToolDescriptor inspect;
			inspect.name = "inspect";
			inspect.description =
				"Read live game state. Runs on the main thread and returns the value synchronously "
				"(a 504 means the main thread did not service the task — the message says whether "
				"it was busy or hung). kinds: "
				"'state' → { plugin, version, playerLoaded, frame, pid, port, exe, vr, game, extender } "
				"— the identity fields name the instance that answered, so a session attached to "
				"several running games (Skyrim on 8920, Fallout on 8930) can confirm which one it "
				"reached; 'health' → the same identity plus { frame, lastTaskFrame, pendingTasks }, "
				"and it is the ONLY kind answered WITHOUT the main thread, so it still replies while "
				"a busy or hung main thread would 504; 'scene' → { position, cell, cellFormId, "
				"interior, worldspace?, gameHour, daysPassed } (worldspace is absent, not empty, for "
				"an interior cell); 'player' → { formId, level, name }.";
			inspect.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "kind", json{ { "type", "string" }, { "enum", json::array({ "state", "health", "scene", "player" }) }, { "description", "default 'state'" } } } } }
			};
			inspect.readOnly = true;
			a_registry.Register(std::move(inspect), &InspectHandler);
		}

		{
			ToolDescriptor console;
			console.name = "console";
			console.description =
				"Run a Fallout 4 console command on the main thread (RE::Console::ExecuteCommand). "
				"Fire-and-forget: the command's console OUTPUT is not captured on Fallout 4 yet, so "
				"the result says { executed, command, note } and never a hollow empty line list. "
				"Use it for state changes (tgm, coc, player.additem, setgs); to read something back, "
				"prefer `inspect`, or `memory` if the value is only reachable in the engine.";
			console.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "command", json{ { "type", "string" }, { "description", "the console command, exactly as you would type it" } } } } },
				{ "required", json::array({ "command" }) }
			};
			a_registry.Register(std::move(console), &ConsoleHandler);
		}
	}
}
