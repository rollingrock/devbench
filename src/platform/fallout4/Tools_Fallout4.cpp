#include "Tools_Fallout4.h"

#include "GameEvents_Fallout4.h"

#include "core/EventBus.h"
#include "core/GameState.h"
#include "core/Json.h"
#include "core/MainThread.h"
#include "core/Server.h"
#include "core/ToolRegistry.h"
#include "core/tools/Memory.h"

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
		// ---- guarded reads for MessageBoxData -------------------------------------
		//
		// CommonLibF4's MessageBoxData/MessageBoxMenu layouts are reverse engineered
		// from FLAT-RIM Fallout 4. This is Fallout 4 VR, where struct layouts are known
		// to differ, and the first version of `describe` trusted those offsets and took
		// the game down (EXCEPTION_ACCESS_VIOLATION reading 0x27 — the "buttonText
		// BSTArray" at +0x38 held 0x27, i.e. not an array pointer at all).
		//
		// So nothing below dereferences an offset it has not first validated. This is
		// the same rule core/gfx/Validate.h states for the render-target seam, for the
		// same reason: at a wrong offset the failure mode is not a wrong answer, it is a
		// crash, because plausible-looking qwords are exactly what a wrong offset yields.
		// Where validation fails we report the raw bytes, so the NEXT session derives
		// the real layout from data instead of guessing at it again.

		bool ReadQWord(std::uintptr_t a_at, std::uintptr_t& a_out)
		{
			return mem::SafeRead(reinterpret_cast<const void*>(a_at), &a_out, sizeof(a_out));
		}

		// Structural filter for a polymorphic engine object: aligned, first qword is a
		// readable vtable, and that vtable's first slot is a plausible code pointer.
		bool LooksLikeVTableObject(std::uintptr_t a_va)
		{
			if (a_va < 0x10000 || (a_va & 7) != 0)
				return false;
			std::uintptr_t vtable = 0;
			if (!ReadQWord(a_va, vtable) || vtable < 0x10000 || (vtable & 7) != 0)
				return false;
			std::uintptr_t firstMethod = 0;
			return ReadQWord(vtable, firstMethod) && firstMethod >= 0x10000;
		}

		// BSStringT<char> is { char* data; uint16 size; uint16 capacity }. Returns the
		// text only if every part of that reading holds up: readable pointer, sane
		// length, and bytes that are actually text. A slot that fails any of these is
		// reported as "not a string here" rather than guessed at.
		bool TryReadBSString(std::uintptr_t a_at, std::string& a_out)
		{
			std::uintptr_t ptr = 0;
			if (!ReadQWord(a_at, ptr) || ptr < 0x10000)
				return false;
			std::uint16_t size = 0;
			std::uint16_t capacity = 0;
			if (!mem::SafeRead(reinterpret_cast<const void*>(a_at + 8), &size, sizeof(size)) ||
				!mem::SafeRead(reinterpret_cast<const void*>(a_at + 10), &capacity, sizeof(capacity)))
				return false;
			if (size == 0 || size > 4096 || capacity < size)
				return false;
			std::string buf(size, '\0');
			if (!mem::SafeRead(reinterpret_cast<const void*>(ptr), buf.data(), size))
				return false;
			// Text, not merely readable memory: a wrong offset lands on pointers and
			// counters far more often than on prose, and those read as control bytes.
			for (const unsigned char c : buf) {
				if (c != '\t' && c != '\n' && c != '\r' && (c < 0x20 || c > 0x7E))
					return false;
			}
			a_out = std::move(buf);
			return true;
		}

		// Every 8-byte-aligned slot in [base, base+len) that parses as a BSStringT.
		// This is how the VR layout gets DERIVED rather than assumed: run it against a
		// live message box and the header/body offsets identify themselves.
		json ScanForStrings(std::uintptr_t a_base, std::size_t a_len)
		{
			json found = json::array();
			for (std::size_t off = 0; off + 0x10 <= a_len; off += 8) {
				std::string s;
				if (TryReadBSString(a_base + off, s))
					found.push_back(json{ { "offset", std::format("0x{:X}", off) }, { "text", std::move(s) } });
			}
			return found;
		}

		// MessageBoxMenu::currentMessage. CommonLibF4 says +0xE8, reverse engineered on
		// flat-rim; on Fallout 4 VR it is measured at +0xF8 (one 0x10 base-class shift),
		// while MessageBoxData's OWN layout is byte-for-byte the same on both. Both are
		// tried and the winner is validated, rather than compiling in one and hoping:
		// the offset that is wrong points at a block of small integers with no vtable,
		// and dereferencing it is what crashed the first build.
		constexpr std::uintptr_t kCurrentMessageOffsets[]{ 0xF8, 0xE8 };

		// A MessageBoxData is identified, not assumed: a vtable AND a bodyText that
		// reads as text. The vtable check alone already rejects the wrong offset here,
		// but "it has a vtable" is a weak claim to hang an indirect call on.
		bool FindMessageBoxData(std::uintptr_t a_menuVA, std::uintptr_t& a_dataVA, std::uintptr_t& a_offset)
		{
			for (const auto off : kCurrentMessageOffsets) {
				std::uintptr_t candidate = 0;
				if (!ReadQWord(a_menuVA + off, candidate) || !LooksLikeVTableObject(candidate))
					continue;
				std::string body;
				if (!TryReadBSString(candidate + 0x28, body))
					continue;
				a_dataVA = candidate;
				a_offset = off;
				return true;
			}
			return false;
		}

		json HexDump(std::uintptr_t a_base, std::size_t a_len)
		{
			std::vector<std::uint8_t> bytes(a_len, 0);
			if (!mem::SafeRead(reinterpret_cast<const void*>(a_base), bytes.data(), a_len))
				return json(nullptr);
			std::string hex;
			hex.reserve(a_len * 3);
			for (std::size_t i = 0; i < a_len; ++i)
				hex += std::format("{:02X}{}", bytes[i], (i % 16 == 15) ? "\n" : " ");
			return hex;
		}

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
				// A modal (MessageBoxMenu) blocks the state machine — `coc` and other
				// commands silently no-op behind one — while every OTHER signal here
				// (frame advancing, pendingTasks 0) still reads healthy. Read from
				// RE::UI::menuMap under the engine's read lock, no main-thread hop, so it
				// stays correct on exactly the same terms as the rest of `health`: still
				// answers when the main thread would 504. See DEVBENCH_REQ_MENU_STATE.md.
				out["blocking"] = IsMessageBoxOpen();
				return out;
			}

			// 'ui' is answered without the main thread, like 'health' — for the same
			// reason: it needs to be trustworthy precisely when the game is stuck behind
			// a modal, not just when it's healthy.
			if (kind == "ui") {
				json open = json::array();
				for (auto& m : GetOpenMenus())
					open.push_back(std::move(m));
				const bool messageBox = IsMessageBoxOpen();
				return json{
					{ "openMenus", std::move(open) },
					{ "messageBoxOpen", messageBox },
					// Only messageBox is treated as blocking today — it's the one open
					// menu type known to gate the state machine (see the requirement
					// doc). Other menus can be open (inventory, pause) without blocking.
					{ "blocking", messageBox },
					// Which source answered. "menuMap" = the engine's own map (ground
					// truth); "events" = the sink's set, which only knows about menus
					// that opened after we subscribed; "none" = nothing is tracking, and
					// the false above means NOTHING. Reported because the first field
					// test produced a confident empty set from a sink that was never
					// installed — a caller must be able to tell those apart.
					{ "source", MenuStateSource() },
					{ "menuEvents", MenuEventsInstalled() },
				};
			}

			return MainThread::RunAndWait([kind]() -> json {
				if (kind == "state")
					return StateSnapshot();
				if (kind == "scene")
					return SceneSnapshot();
				if (kind == "player")
					return PlayerSnapshot();
				throw ToolError(400, "inspect: unknown kind '" + kind + "' (state|health|ui|scene|player)");
			});
		}

		json ConsoleHandler(const json& a_args, const ToolContext&)
		{
			const std::string command = a_args.value("command", std::string{});
			if (command.empty())
				throw ToolError(400, "console: 'command' is required");

			// Checked BEFORE running: a modal open at submission time is what makes a
			// state-machine command (coc, ...) silently no-op, and fire-and-forget
			// means this is the only chance to say so.
			const bool blocked = IsMessageBoxOpen();

			// Fire-and-forget on purpose. Fallout 4's console has no ConsoleLog fencing
			// equivalent wired up here yet, so promising a captured result would be a
			// promise this cannot keep — say plainly that the output is not returned
			// rather than returning an empty 'lines' array that reads like "no output".
			MainThread::RunAndWait([command]() -> json {
				RE::Console::ExecuteCommand(command.c_str());
				return json{ { "queued", true } };
			});
			json out{
				{ "executed", true },
				{ "command", command },
				{ "note", "output is not captured on Fallout 4 yet — see the `log` tool, or read the in-game console" },
				{ "blocked", blocked },
			};
			if (blocked) {
				out["blockedNote"] =
					"a modal (MessageBoxMenu) was open when this command was submitted -- many console "
					"commands (e.g. coc) silently no-op behind one rather than erroring. The command above "
					"still ran (fire-and-forget), so it may have done nothing. Use inspect kind='ui' or "
					"menu action='describe' to read the modal, and menu action='accept' to answer it.";
			}
			return out;
		}

		// menu: detect/answer menus (mirrors Skyrim's `menu` tool's shape — same action
		// names, same result keys — where Fallout has the concept). 'list' is read off
		// the same main-thread-independent mutex as `inspect kind='ui'` (they share
		// GetOpenMenus()/IsMessageBoxOpen() — one source of truth, not two that can
		// drift). 'open'/'close' queue a UI message (kShow/kHide); 'describe'/'accept'
		// read/answer the active MessageBoxMenu on the main thread.
		json MenuHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("list"));

			if (action == "list") {
				json open = json::array();
				for (auto& m : GetOpenMenus())
					open.push_back(std::move(m));
				return json{
					{ "openMenus", std::move(open) },
					{ "messageBoxOpen", IsMessageBoxOpen() },
					{ "source", MenuStateSource() },
					{ "menuEvents", MenuEventsInstalled() },
				};
			}

			if (action == "open" || action == "close") {
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, std::format("action '{}' requires a 'name' (menu to {})", action, action == "open" ? "show" : "hide"));
				auto* task = F4SE::GetTaskInterface();
				if (!task)
					throw ToolError(500, "F4SE TaskInterface unavailable");
				const auto type = (action == "open") ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide;
				task->AddTask([name, type]() {
					if (auto* q = RE::UIMessageQueue::GetSingleton())
						q->AddMessage(name.c_str(), type);
				});
				return json{ { "queued", true }, { "action", action }, { "name", name } };
			}

			if (action == "describe") {
				// Read the active MessageBoxMenu's data. CommonLibF4 has no
				// GetCurrentMessageBoxData() convenience wrapper (unlike CommonLibSSE-NG's
				// Skyrim build), so this goes at `currentMessage` directly — but through
				// guarded reads, because those offsets are flat-rim's and this is VR. See
				// the block comment on the read helpers above: the naive version crashed.
				const bool raw = a_args.value("raw", false);
				return MainThread::RunAndWait([raw]() -> json {
					auto* ui = RE::UI::GetSingleton();
					auto  mbm = ui ? ui->GetMenu<RE::MessageBoxMenu>() : nullptr;
					if (!mbm)
						return json{ { "messageBoxOpen", false } };

					const auto     menuVA = reinterpret_cast<std::uintptr_t>(mbm.get());
					std::uintptr_t dataVA = 0;
					std::uintptr_t msgOffset = 0;
					const bool     found = FindMessageBoxData(menuVA, dataVA, msgOffset);

					json out{
						{ "messageBoxOpen", true },
						{ "menuAddress", std::format("0x{:X}", menuVA) },
						{ "layoutValidated", found },
					};
					if (!found) {
						out["note"] =
							"no slot on the menu (+0xF8 VR, +0xE8 flat-rim) points at something that reads as a "
							"MessageBoxData -- vtable plus a bodyText that is actually text. Nothing was "
							"dereferenced. `menuBytes` is the menu object's memory: find the slot pointing at an "
							"object whose +0x28 holds the dialog text, and add that offset to "
							"kCurrentMessageOffsets.";
						out["menuBytes"] = HexDump(menuVA, 0x140);
						return out;
					}
					out["currentMessageOffset"] = std::format("0x{:X}", msgOffset);
					out["currentMessage"] = std::format("0x{:X}", dataVA);

					std::string s;
					if (TryReadBSString(dataVA + 0x18, s))
						out["headerText"] = s;
					else
						out["headerText"] = "";  // empty is normal: this dialog has no header
					if (TryReadBSString(dataVA + 0x28, s))
						out["bodyText"] = s;

					// buttonText is a BSTArray<BSStringT<char>>: { void* data; u32 capacity;
					// <pad>; u32 size } -- size at +0x10, NOT +0x0C, because the allocator's
					// capacity is padded out to 8. sizeof is 0x18, which is exactly why the
					// next member (warningContext) sits at +0x50. Getting this wrong is what
					// made the count read as 1967652873.
					std::uintptr_t bdata = 0;
					std::uint32_t  bcap = 0, bsize = 0;
					if (ReadQWord(dataVA + 0x38, bdata) &&
						mem::SafeRead(reinterpret_cast<const void*>(dataVA + 0x40), &bcap, sizeof(bcap)) &&
						mem::SafeRead(reinterpret_cast<const void*>(dataVA + 0x48), &bsize, sizeof(bsize)) &&
						bdata >= 0x10000 && bsize <= 16 && bcap <= 16 && bcap >= bsize) {
						json buttons = json::array();
						for (std::uint32_t i = 0; i < bsize; ++i) {
							std::string b;
							buttons.push_back(TryReadBSString(bdata + i * 0x10, b) ? b : std::string{});
						}
						out["buttons"] = std::move(buttons);
					} else {
						out["buttons"] = json::array();
						out["buttonsNote"] = std::format(
							"buttonText at +0x38 did not read as a BSTArray (data=0x{:X} size={} cap={}). Button "
							"INDEXES still work for `accept`; only their labels are unavailable.",
							bdata, bsize, bcap);
					}

					std::uint8_t modal = 0;
					if (mem::SafeRead(reinterpret_cast<const void*>(dataVA + 0x64), &modal, sizeof(modal)))
						out["modal"] = modal != 0;
					if (raw) {
						out["bytes"] = HexDump(dataVA, 0x68);
						out["strings"] = ScanForStrings(dataVA, 0x68);
					}
					return out;
				});
			}

			if (action == "accept") {
				// Answer the active MessageBoxMenu by button index (default 0): invokes
				// its IMessageBoxCallback, which is what actually unblocks whatever was
				// waiting on the choice (e.g. "Continue Loading?"), and queues kHide to
				// close the menu. No queue-pop API is exposed here to mirror exactly
				// (unlike CommonLibSSE-NG's MessageBoxMenu::SelectOption on Skyrim), so
				// the callback is kept alive via its own smart pointer across the kHide.
				//
				// This makes an INDIRECT CALL through an offset reverse engineered on
				// flat-rim (`callback` at +0x58), on a VR binary where the sibling field
				// at +0x38 is already known not to hold what that layout claims. A wrong
				// function pointer here does not misbehave, it executes garbage — so the
				// call is gated on the whole chain validating first, and refuses with an
				// explanation rather than rolling the dice. Run `describe` to see the
				// evidence behind a refusal.
				const int index = a_args.value("index", 0);
				if (index < 0 || index > 255)
					throw ToolError(400, std::format("invalid index '{}' (button index is a uint8)", index));

				return MainThread::RunAndWait([index]() -> json {
					auto* ui = RE::UI::GetSingleton();
					auto  mbm = ui ? ui->GetMenu<RE::MessageBoxMenu>() : nullptr;
					if (!mbm)
						return json{ { "accepted", false }, { "reason", "no MessageBoxMenu is open" } };

					const auto     menuVA = reinterpret_cast<std::uintptr_t>(mbm.get());
					std::uintptr_t dataVA = 0;
					std::uintptr_t msgOffset = 0;
					if (!FindMessageBoxData(menuVA, dataVA, msgOffset))
						return json{
							{ "accepted", false },
							{ "reason", "could not identify the MessageBoxData on this menu (tried +0xF8 and +0xE8) "
										"-- refusing to call through an unverified layout. See `describe`." },
						};

					std::uintptr_t cbVA = 0;
					if (!ReadQWord(dataVA + 0x58, cbVA) || !LooksLikeVTableObject(cbVA))
						return json{
							{ "accepted", false },
							{ "reason", std::format("callback (+0x58) = 0x{:X} is not a polymorphic object -- refusing "
													"to call it. This dialog may have no callback at all (not every "
													"message box has one). See `describe`.",
										   cbVA) },
						};

					// Validated: vtable slot 1 is operator()(uint8). Keep the callback
					// alive across the kHide, which can drop the menu's own reference.
					auto* cb = reinterpret_cast<RE::IMessageBoxCallback*>(cbVA);
					RE::BSTSmartPointer<RE::IMessageBoxCallback> keepAlive{ cb };
					if (auto* q = RE::UIMessageQueue::GetSingleton())
						q->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
					(*keepAlive)(static_cast<std::uint8_t>(index));
					return json{
						{ "accepted", true },
						{ "index", index },
						{ "currentMessageOffset", std::format("0x{:X}", msgOffset) },
					};
				});
			}

			throw ToolError(400, std::format("unknown action '{}' (list|open|close|describe|accept)", action));
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
				"a busy or hung main thread would 504, plus { blocking } — is a modal open right now "
				"(see 'ui'); 'ui' → { openMenus, messageBoxOpen, blocking, source, menuEvents }, ALSO "
				"answered without the main thread (same reason as 'health' — it has to be trustworthy "
				"exactly when the game is stuck behind a modal). `source` is 'menuMap' when the answer "
				"came from the engine's own menu map (ground truth) and 'none' when nothing is tracking "
				"— in which case `blocking:false` means nothing, so check it before believing a false; "
				"'scene' → { position, cell, cellFormId, "
				"interior, worldspace?, gameHour, daysPassed } (worldspace is absent, not empty, for "
				"an interior cell); 'player' → { formId, level, name }.";
			inspect.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "kind", json{ { "type", "string" }, { "enum", json::array({ "state", "health", "ui", "scene", "player" }) }, { "description", "default 'state'" } } } } }
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
				"the result says { executed, command, note, blocked } and never a hollow empty line "
				"list. `blocked` is true if a modal (MessageBoxMenu) was open at submission time — "
				"many state-machine commands (coc, ...) silently no-op behind one rather than erroring, "
				"so this is the one warning available; see `menu` to describe/dismiss it. "
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

		{
			ToolDescriptor menu;
			menu.name = "menu";
			menu.description =
				"Detect and answer in-game menus — mirrors the shape of Skyrim's `menu` tool "
				"(same action names, same result keys) where the concept exists on Fallout. "
				"actions: 'list' (default) → { openMenus, messageBoxOpen, source, menuEvents }, answered "
				"off the same main-thread-independent signal as `inspect kind='ui'` (RE::UI::menuMap "
				"under the engine's read lock — `source` says so, and says when nothing is tracking); "
				"'describe' → the active "
				"MessageBoxMenu's { headerText, bodyText, buttons, modal } or { messageBoxOpen: false }; "
				"'accept' (name='index', default 0) → answers the active MessageBoxMenu by button index "
				"and closes it — the way to recover from a blocking modal (e.g. the \"this save relies "
				"on content that is no longer present\" load warning) without a headset; 'open'/'close' "
				"(requires 'name') → show/hide an engine menu by name via the UI message queue "
				"(kShow/kHide).";
			menu.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "open", "close", "describe", "accept" }) }, { "description", "default 'list'" } } },
									{ "name", json{ { "type", "string" }, { "description", "menu name — required for 'open'/'close'" } } },
									{ "index", json{ { "type", "integer" }, { "description", "button index for 'accept', default 0" } } },
									{ "raw", json{ { "type", "boolean" }, { "description", "'describe' only: also return a hex dump of MessageBoxData, for deriving the VR struct layout" } } },
								} }
			};
			a_registry.Register(std::move(menu), &MenuHandler);
		}
	}
}
