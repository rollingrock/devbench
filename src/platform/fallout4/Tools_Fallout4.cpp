#include "Tools_Fallout4.h"

#include "GameEvents_Fallout4.h"

#include "core/ConsoleCaptureLogic.h"
#include "core/EventBus.h"
#include "core/GameState.h"
#include "core/HostApi.h"
#include "core/Json.h"
#include "core/MainThread.h"
#include "core/Server.h"
#include "core/ToolExtensions.h"
#include "core/ToolRegistry.h"
#include "core/tools/Memory.h"

#include "Version.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>

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

			json       out = json::object();
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

		json InspectHandler(const json& a_args, const ToolContext& a_ctx)
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

			if (kind == "state" || kind == "scene" || kind == "player") {
				return MainThread::RunAndWait([kind]() -> json {
					if (kind == "state")
						return StateSnapshot();
					if (kind == "scene")
						return SceneSnapshot();
					return PlayerSnapshot();
				});
			}

			// 'registrants': who requested the C-ABI interface and what they registered
			// through it — the same ledger and the same shape as the Skyrim kind. The two
			// lists are side by side, not joined: the C-ABI has no per-call caller
			// identity, so pairing them by plugin name would be a guess.
			if (kind == "registrants") {
				json consumers = json::array();
				for (const auto& c : HostApi::Consumers())
					consumers.push_back(json{ { "name", c.name }, { "route", c.route }, { "atEpoch", c.atEpoch }, { "atFrame", c.atFrame } });

				json registrations = json::array();
				for (const auto& r : HostApi::Registrations())
					registrations.push_back(json{
						{ "kind", r.kind }, { "name", r.name },
						{ "atEpoch", r.atEpoch }, { "atFrame", r.atFrame }, { "replaced", r.replaced } });

				json capabilities = json::object();
				for (const char* base : { "capture", "inspect", "menu" }) {
					json keys = json::array();
					for (const auto& k : ToolExtensions::Keys(base))
						keys.push_back(k);
					capabilities[base] = std::move(keys);
				}
				return json{
					{ "consumers", std::move(consumers) },
					{ "registrations", std::move(registrations) },
					{ "capabilities", std::move(capabilities) },
				};
			}

			if (kind == "extensions") {
				json out = json::array();
				for (const auto& k : ToolExtensions::Keys("inspect")) {
					json e{ { "kind", k } };
					if (auto entry = ToolExtensions::Find("inspect", k))
						e["descriptor"] = entry->descriptor;
					out.push_back(std::move(e));
				}
				return json{ { "extensions", std::move(out) } };
			}

			// A kind another plugin registered (C-ABI RegisterToolExtension "inspect").
			// Its handler runs here, on the listener thread — the C-ABI contract is that
			// a consumer marshals to the main thread itself, same as for a whole tool.
			if (auto entry = ToolExtensions::Find("inspect", kind))
				return entry->handler(a_args, a_ctx);

			throw ToolError(400, "inspect: unknown kind '" + kind +
									 "' (state|health|ui|scene|player|registrants|extensions, or a registered kind -- see kind='extensions')");
		}

		// " Registered (mod) kinds: a — desc; b." for the descriptor, so a kind another
		// plugin added is discoverable from tools/list and not only from kind='extensions'.
		std::string RegisteredInspectKinds()
		{
			const auto keys = ToolExtensions::Keys("inspect");
			if (keys.empty())
				return {};
			std::string s = " Registered (mod) kinds: ";
			for (std::size_t i = 0; i < keys.size(); ++i) {
				std::string desc;
				if (auto e = ToolExtensions::Find("inspect", keys[i]))
					desc = e->descriptor.value("description", std::string{});
				s += keys[i];
				if (!desc.empty())
					s += " — " + desc;
				s += (i + 1 < keys.size()) ? "; " : ".";
			}
			return s;
		}

		// Rebuilt, not frozen at startup, so a registered kind lands in the enum and the
		// description — the change listener in RegisterGameTools re-registers it.
		ToolDescriptor BuildInspectDescriptor()
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
				"an interior cell); 'player' → { formId, level, name }; "
				"'registrants' → who has requested the C-ABI interface and what they registered "
				"through it { consumers:[{name,route,atEpoch,atFrame}] (route: message = extender "
				"handshake, name is the sender; export = DevBench_GetApiFunction, name is the calling "
				"DLL), registrations:[{kind,name,atEpoch,"
				"atFrame,replaced}], capabilities:{capture,inspect,menu → [registered keys]} } — side "
				"by side, not joined, since the C-ABI has no per-call caller identity; "
				"'extensions' → the kinds other plugins added via the C-ABI RegisterToolExtension, "
				"with their descriptors, and kind=<registered> dispatches to that plugin's handler.";
			inspect.description += RegisteredInspectKinds();
			json kinds = json::array({ "state", "health", "ui", "scene", "player", "registrants", "extensions" });
			for (const auto& k : ToolExtensions::Keys("inspect"))
				kinds.push_back(k);
			inspect.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "kind", json{ { "type", "string" }, { "enum", std::move(kinds) }, { "description", "default 'state'; or a registered mod kind (see kind='extensions')" } } } } }
			};
			inspect.readOnly = true;
			return inspect;
		}

		// ---- console output --------------------------------------------------------
		//
		// Every console print ends in ConsoleLog::AddString, which appends to
		// ConsoleLog::buffer under the log's lock. When the buffer goes from empty to
		// non-empty it also queues a UI message, and the Console menu, once it has ever
		// been created, drains the buffer into its history when that message is
		// processed. That happens in the UI pass, never inside a task.
		//
		// Console::ExecuteCommand does not run the command: it echoes it, then queues it
		// for the engine to run on a later frame, by which time a created Console menu may
		// already have drained the output. So a capture compiles and runs the command
		// itself (Script::CompileAndRun with the console's compiler, against the selected
		// reference, as MentatsF4SE's ExecuteCommand does), and reads the buffer before and
		// after in ONE main-thread task. Its output is then in the buffer whether or not the
		// console exists: no hook, no marker commands, no polling.
		//
		// Measured live on FO4VR (2026-09-28): before the console was first opened the
		// buffer held every line printed since load; opening it dropped the size to 0;
		// with the menu created, open or closed, it read 0 again right after a command.
		// A command through ExecuteCommand put only its echo in the buffer within the
		// task; its output arrived later. VR addresses, all from the VR address library
		// and disassembled live: AddString 0x12E3F10 (also Modding-Reference F4VR/
		// Analysis/gold/f4sevr_0_6_21_RE_REFERENCE.md, Console) appends to [this+0x08]
		// and drops any string that would take the buffer to 0xFFFE bytes; ConsoleLog's
		// singleton is 0x59429C8, F4SEVR's g_console (Analysis/silver/
		// FO4VRTools_RE_REFERENCE.md); ExecuteCommand 0x12DC460 copies each command into
		// a queue slot; CompileAndRun is 0x4CBA50.

		constexpr std::size_t kConsoleBufferLimit = 0xFFFE;
		// Past this, a long line may already be dropped by AddString's limit.
		constexpr std::size_t kConsoleBufferNearlyFull = 0xF000;
		constexpr std::size_t kConsoleDefaultLines = 200;
		constexpr std::size_t kConsoleMaxLines = 5000;

		// The console buffer's text, or nullopt if it cannot be read. Main thread. Read
		// through SafeRead since the log's lock is not taken: a print from another thread
		// can race this, and a torn read must be a failed capture, not a crash.
		std::optional<std::string> ReadConsoleBuffer()
		{
			auto* log = RE::ConsoleLog::GetSingleton();
			if (!log)
				return std::nullopt;
			// BSStringT<char> is { char* data; uint16 size; uint16 capacity }, and a size of
			// 0xFFFF means "not tracked, use strlen".
			const auto     at = reinterpret_cast<std::uintptr_t>(&log->buffer);
			std::uintptr_t data = 0;
			std::uint16_t  size = 0;
			std::uint16_t  capacity = 0;
			if (!ReadQWord(at, data) ||
				!mem::SafeRead(reinterpret_cast<const void*>(at + 8), &size, sizeof(size)) ||
				!mem::SafeRead(reinterpret_cast<const void*>(at + 10), &capacity, sizeof(capacity)))
				return std::nullopt;
			if (!data || size == 0)
				return std::string{};  // never allocated, or drained
			const std::size_t length = size == 0xFFFF ? capacity : size;
			std::string       text(length, '\0');
			if (!mem::SafeRead(reinterpret_cast<const void*>(data), text.data(), length))
				return std::nullopt;
			if (size == 0xFFFF)
				text.resize(::strnlen(text.data(), length));
			return text;
		}

		// The console's selected reference (prid, or a click), which a command without an
		// explicit `ref.` runs against. CommonLibF4 has only its flat-OG id, 170742, which
		// the VR address library also maps (0x5B3CC50); next-gen has no such id and
		// resolving a missing one is a CTD, so there it is left unset.
		RE::NiPointer<RE::TESObjectREFR> ConsolePickRef()
		{
			if (REL::Module::IsNG())
				return nullptr;
			return RE::Console::GetPickRef().get();
		}

		// Compiles and runs one console command now instead of on a later frame. Main
		// thread. False if it did not compile (the reason is printed).
		bool RunConsoleCommandNow(const std::string& a_command)
		{
			auto* factory = RE::ConcreteFormFactory<RE::Script>::GetFormFactory();
			auto* script = factory ? factory->Create() : nullptr;
			if (!script)
				throw ToolError(500, "console: the engine did not create a Script form to run the command");
			const auto         target = ConsolePickRef();
			RE::ScriptCompiler compiler;
			script->SetText(a_command);
			script->CompileAndRun(&compiler, RE::COMPILER_NAME::kSystemWindow, target.get());
			const bool compiled = script->header.isCompiled;
			delete script;  // the engine's own deleting destructor, through the vtable
			return compiled;
		}

		json ConsoleHandler(const json& a_args, const ToolContext&)
		{
			const std::string command = a_args.value("command", std::string{});
			if (command.empty())
				throw ToolError(400, "console: 'command' is required");
			const bool capture = a_args.value("capture", true);
			const auto maxLines = static_cast<std::size_t>(std::clamp<std::int64_t>(
				a_args.value("maxLines", static_cast<std::int64_t>(kConsoleDefaultLines)), 1, kConsoleMaxLines));

			// Checked BEFORE running: a modal open at submission time is what makes a
			// state-machine command (coc, ...) silently no-op without printing anything.
			const bool blocked = IsMessageBoxOpen();

			json out;
			if (!capture) {
				MainThread::RunAndWait([command]() -> json {
					RE::Console::ExecuteCommand(command.c_str());
					return true;
				});
				out = json{
					{ "executed", true },
					{ "command", command },
					{ "captured", false },
					{ "note", "capture=false: queued exactly as if typed, so it runs on a later frame and its output goes to the in-game console" },
				};
			} else {
				// The console runs a ForEachRef[...] block itself, before any script sees it.
				constexpr std::string_view kForEachRef = "foreachref[";
				if (command.size() >= kForEachRef.size() &&
					std::ranges::equal(std::string_view(command).substr(0, kForEachRef.size()), kForEachRef,
						[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }))
					throw ToolError(400, "console: a ForEachRef[...] block only runs through the console's queue; pass capture=false");
				const auto commands = ConsoleLogCapture::SplitConsoleCommands(command);
				if (commands.empty())
					throw ToolError(400, "console: 'command' has no command in it");

				// One task for the baseline, the commands and the read: the Console menu can
				// only drain the buffer after this task returns.
				out = MainThread::RunAndWait([command, commands, maxLines]() -> json {
					// The echo ExecuteCommand would print, so the console history still shows
					// what ran. Before the baseline, so it is not part of the output.
					if (auto* log = RE::ConsoleLog::GetSingleton())
						log->AddString((command + "\n").c_str());
					const auto before = ReadConsoleBuffer();
					bool       compiled = true;
					for (const auto& one : commands)
						compiled = RunConsoleCommandNow(one) && compiled;
					const auto after = ReadConsoleBuffer();

					json r{ { "executed", true }, { "command", command }, { "compiled", compiled } };
					if (!before || !after) {
						r["captured"] = false;
						r["note"] = "the console buffer could not be read, so the output was not captured; read the in-game console";
						return r;
					}
					const auto appended = ConsoleLogCapture::AppendedLines(*after, before->size(), maxLines);
					r["captured"] = true;
					r["count"] = appended.lines.size();
					r["lines"] = appended.lines;
					if (appended.omitted)
						r["omitted"] = appended.omitted;
					if (appended.drained)
						r["drained"] = true;
					if (after->size() >= kConsoleBufferNearlyFull) {
						r["bufferNearlyFull"] = true;
						r["note"] = std::format(
							"the console buffer holds {} of its {} bytes, and the engine silently drops any line that "
							"would pass the limit, so output may be missing. It fills only until the Console menu is "
							"first created: menu action='open' then 'close' with name='Console' drains it for good.",
							after->size(), kConsoleBufferLimit);
					}
					return r;
				});
			}
			out["blocked"] = blocked;
			if (blocked) {
				out["blockedNote"] =
					"a modal (MessageBoxMenu) was open when this command was submitted -- many console "
					"commands (e.g. coc) silently no-op behind one rather than erroring, so it may have "
					"done nothing. Use inspect kind='ui' or menu action='describe' to read the modal, and "
					"menu action='accept' to answer it.";
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
							{ "reason",
								"could not identify the MessageBoxData on this menu (tried +0xF8 and +0xE8) "
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
					auto*                                        cb = reinterpret_cast<RE::IMessageBoxCallback*>(cbVA);
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
		a_registry.Register(BuildInspectDescriptor(), &InspectHandler);

		// When a plugin registers an inspect kind, rebuild the descriptor so the kind is in
		// tools/list from the next call (re-registering also fires tools/list_changed).
		// Only `inspect` routes extensions on Fallout so far; `menu` and `capture` keys
		// are still recorded (and listed by kind='registrants') but not dispatched.
		ToolExtensions::SetChangeListener([reg = &a_registry](const std::string& a_baseTool) {
			std::string base = a_baseTool;
			std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (base == "inspect")
				reg->Register(BuildInspectDescriptor(), &InspectHandler);
		});

		{
			ToolDescriptor console;
			console.name = "console";
			console.description =
				"Run a Fallout 4 console command and return what it printed: { executed, command, "
				"compiled, captured, count, lines, blocked }. By default (capture=true) the command is "
				"compiled and run right away on the main thread, against the console's selected "
				"reference, and its output is read from the engine's console buffer in that same task, "
				"so `lines` holds this command's output only, complete even when it prints many lines "
				"at once (player.showinventory, sqv), with blank lines dropped; one call, no separate "
				"read. At most "
				"maxLines (default 200, max 5000), the most recent; `omitted` counts the rest. "
				"compiled=false means the command did not compile (the reason is in `lines`); "
				"captured=true with no lines means it printed nothing; captured=false means the buffer "
				"could not be read. `bufferNearlyFull` means lines may have been dropped by the engine's "
				"64 KB limit, which only applies before the Console menu is first opened (the note says "
				"how to clear it). Several commands separated by ';' run in order, as in the console; "
				"`compiled` is false if any did not compile. A ForEachRef[...] block needs capture=false, "
				"which queues the line exactly as if typed; it then runs on a later frame and returns no "
				"output. `blocked` is true if a "
				"modal (MessageBoxMenu) was open at submission time — many state-machine commands (coc, "
				"...) silently no-op behind one rather than erroring; see `menu` to describe/dismiss it. "
				"Non-UTF-8 bytes come back escaped as \\xNN.";
			console.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "command", json{ { "type", "string" }, { "description", "the console command, exactly as you would type it" } } },
									{ "capture", json{ { "type", "boolean" }, { "description", "default true: run it now and return its output; false queues it as if typed, with no output" } } },
									{ "maxLines", json{ { "type", "integer" }, { "description", "most recent output lines to return, default 200, max 5000" } } } } },
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

		RegisterNodeTools(a_registry);
	}
}
