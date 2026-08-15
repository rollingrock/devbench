#include "core/tools/CommonTools.h"

#include "core/EventBus.h"
#include "core/Host.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/tools/Memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

// Tools that need nothing from the game. They register identically on every
// platform, so `memory` and `log` behave the same whether an agent is driving
// Skyrim, Fallout 4, or something else — which is the point: a debugging loop
// learned on one game transfers.
//
// NOTE ON SCOPE: `memory` can read and write arbitrary process memory. That is
// correct for a local dev bench (the whole reason the bench exists is to shorten a
// reverse-engineering loop), and it is no more privileged than the `console` tool
// already is. It is nevertheless gated: writes require the caller to opt in per
// request AND the config to allow them, because "I meant to read" is a mistake
// someone will make.

namespace dvb::tools
{
	namespace
	{
		bool g_allowWrites = false;

		std::string Hex(std::uint64_t a_v)
		{
			return std::format("0x{:x}", a_v);
		}

		// Format one scalar. NaN and infinity are emitted as STRINGS, never as JSON
		// null: a bench whose whole job is finding a NaN must not be the thing that
		// swallows it. (This is not hypothetical — a NaN in a light-accumulation
		// buffer survived five debugging sessions in the FO4VR investigation partly
		// because the instruments reported it as "nothing to see".)
		json Scalar(const std::string& a_type, const std::uint8_t* a_p)
		{
			if (a_type == "u8")
				return json(*a_p);
			if (a_type == "u16") {
				std::uint16_t v;
				std::memcpy(&v, a_p, sizeof(v));
				return json(v);
			}
			if (a_type == "u32") {
				std::uint32_t v;
				std::memcpy(&v, a_p, sizeof(v));
				return json(v);
			}
			if (a_type == "i32") {
				std::int32_t v;
				std::memcpy(&v, a_p, sizeof(v));
				return json(v);
			}
			if (a_type == "u64") {
				std::uint64_t v;
				std::memcpy(&v, a_p, sizeof(v));
				return json(v);
			}
			if (a_type == "i64") {
				std::int64_t v;
				std::memcpy(&v, a_p, sizeof(v));
				return json(v);
			}
			if (a_type == "ptr") {
				std::uint64_t v;
				std::memcpy(&v, a_p, sizeof(v));
				return json(Hex(v));
			}
			if (a_type == "f32") {
				float v;
				std::memcpy(&v, a_p, sizeof(v));
				if (std::isfinite(v))
					return json(v);
				return json(std::isnan(v) ? "NaN" : (v > 0 ? "Inf" : "-Inf"));
			}
			double v;
			std::memcpy(&v, a_p, sizeof(v));
			if (std::isfinite(v))
				return json(v);
			return json(std::isnan(v) ? "NaN" : (v > 0 ? "Inf" : "-Inf"));
		}

		std::size_t StrideOf(const std::string& a_type)
		{
			if (a_type == "u8" || a_type == "bytes" || a_type == "cstr")
				return 1;
			if (a_type == "u16")
				return 2;
			if (a_type == "u32" || a_type == "i32" || a_type == "f32")
				return 4;
			if (a_type == "u64" || a_type == "i64" || a_type == "f64" || a_type == "ptr")
				return 8;
			return 0;  // unknown
		}

		// Report the VA always, and the RVA only when the address is genuinely inside
		// the executable image. A heap object gets "heap": true instead — an RVA for it
		// would paste into Ghidra as a valid-looking lie.
		void AnnotateAddress(json& a_out, std::uint64_t a_va)
		{
			a_out["addr"] = Hex(a_va);
			if (mem::InImage(a_va))
				a_out["rva"] = Hex(a_va - mem::ImageBase());
			else
				a_out["heap"] = true;
		}

		json MemoryHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("read"));
			const std::string expr = a_args.value("addr", std::string{});
			if (expr.empty())
				throw ToolError(400, "memory: 'addr' is required (an address expression, e.g. base+0x1d98ff0)");

			std::uint64_t va = 0;
			std::string   err;
			if (!mem::ResolveAddress(expr, va, err))
				throw ToolError(400, "memory: " + err);

			json out = json::object();
			out["expr"] = expr;
			AnnotateAddress(out, va);
			out["base"] = Hex(mem::ImageBase());

			if (action == "resolve")
				return out;

			const std::string type = a_args.value("type", std::string("u32"));
			const std::size_t stride = StrideOf(type);
			if (stride == 0)
				throw ToolError(400, "memory: unknown type '" + type + "'");
			out["type"] = type;

			if (action == "write") {
				if (!g_allowWrites)
					throw ToolError(403, "memory: writes are disabled — set allowMemoryWrites=true in config.json");
				if (!a_args.value("confirm", false))
					throw ToolError(400, "memory: a write needs confirm=true (guards against a typo'd read)");
				if (type == "cstr" || type == "bytes")
					throw ToolError(400, "memory: write takes one scalar (u8/u16/u32/u64/i32/i64/f32/f64)");
				if (!a_args.contains("value"))
					throw ToolError(400, "memory: 'value' is required for a write");

				std::uint8_t bytes[8]{};
				if (type == "f32") {
					const auto f = a_args["value"].get<float>();
					std::memcpy(bytes, &f, 4);
				} else if (type == "f64") {
					const auto d = a_args["value"].get<double>();
					std::memcpy(bytes, &d, 8);
				} else {
					// Accept a decimal number or a "0x..." string, so a value copied out
					// of a disassembler pastes in unchanged.
					std::uint64_t v = 0;
					if (a_args["value"].is_string())
						v = std::strtoull(a_args["value"].get<std::string>().c_str(), nullptr, 0);
					else
						v = a_args["value"].get<std::uint64_t>();
					std::memcpy(bytes, &v, stride);
				}

				std::uint8_t before[8]{};
				if (!mem::SafeRead(reinterpret_cast<const void*>(va), before, stride))
					throw ToolError(400, "memory: read-back of " + Hex(va) + " faulted; refusing to write");
				if (!mem::SafeWrite(reinterpret_cast<void*>(va), bytes, stride))
					throw ToolError(400, "memory: write to " + Hex(va) + " faulted (page not writable?)");
				std::uint8_t after[8]{};
				mem::SafeRead(reinterpret_cast<const void*>(va), after, stride);

				// Echo before AND after. "the write succeeded" and "the value changed"
				// are different claims: a write to a page the engine rewrites every
				// frame looks identical to a no-op unless both are printed. `held`
				// says which one happened, and `before` is the caller's undo.
				out["before"] = Scalar(type == "ptr" ? "u64" : type, before);
				out["after"] = Scalar(type == "ptr" ? "u64" : type, after);
				out["held"] = std::memcmp(after, bytes, stride) == 0;
				dlog::info("memory: wrote {} at {} ({} -> {})", type, Hex(va), out["before"].dump(), out["after"].dump());
				return out;
			}

			if (action != "read")
				throw ToolError(400, "memory: unknown action '" + action + "' (resolve|read|write)");

			// A bench read that can allocate hundreds of MB is a footgun, not a
			// feature. Cap it and SAY the cap was hit rather than truncating quietly.
			std::int64_t           count = a_args.value("count", 1);
			constexpr std::int64_t kMaxCount = 4096;
			if (count > kMaxCount) {
				count = kMaxCount;
				out["capped"] = true;
			}
			if (count < 1)
				count = 1;
			out["count"] = count;

			if (type == "cstr") {
				char        buf[512]{};
				std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(count), sizeof(buf) - 1);
				if (!mem::SafeRead(reinterpret_cast<const void*>(va), buf, n))
					throw ToolError(400, "memory: read faulted at " + Hex(va));
				buf[n] = '\0';
				out["value"] = std::string(buf);
				return out;
			}

			std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * stride);
			if (!mem::SafeRead(reinterpret_cast<const void*>(va), raw.data(), raw.size())) {
				// Name the faulting address. A probe whose only outcomes are "the
				// answer" and "failed" cannot tell a wrong pointer from an absent one.
				throw ToolError(400, std::format("memory: read of {} bytes faulted at {}", raw.size(), Hex(va)));
			}

			json values = json::array();
			for (std::int64_t k = 0; k < count; ++k) {
				const std::uint8_t* p = raw.data() + static_cast<std::size_t>(k) * stride;
				values.push_back(type == "bytes" ? json(*p) : Scalar(type, p));
			}
			out["values"] = std::move(values);
			return out;
		}

		json LogHandler(const json& a_args, const ToolContext&)
		{
			const auto dir = host::LogDir();
			if (dir.empty())
				throw ToolError(500, "log: the script extender did not resolve a log directory");

			const std::string action = a_args.value("action", std::string("tail"));
			std::error_code   ec;

			if (action == "list") {
				json files = json::array();
				for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
					if (!e.is_regular_file(ec) || e.path().extension() != ".log")
						continue;
					files.push_back(json{
						{ "file", e.path().filename().string() },
						{ "bytes", static_cast<std::uint64_t>(e.file_size(ec)) } });
				}
				return json{ { "dir", dir.string() }, { "count", files.size() }, { "logs", std::move(files) } };
			}

			if (action != "tail")
				throw ToolError(400, "log: unknown action '" + action + "' (tail|list)");

			// Default to devbench's own log — the overwhelmingly common ask — but any
			// plugin's log in the same directory is fair game, which is what makes this
			// useful for debugging a mod that is NOT devbench.
			std::string name = a_args.value("file", std::string("devbench.log"));
			if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
				name.find("..") != std::string::npos)
				throw ToolError(400, "log: 'file' must be a bare filename in the extender's log directory");

			const auto path = dir / name;
			std::ifstream in(path);
			if (!in)
				throw ToolError(404, "log: cannot open " + path.string());

			std::int64_t tail = std::clamp<std::int64_t>(a_args.value("tail", 80), 1, 5000);
			const std::string needle = a_args.value("grep", std::string{});

			std::vector<std::string> keep;
			std::string              line;
			std::uint64_t            scanned = 0;
			while (std::getline(in, line)) {
				++scanned;
				if (!needle.empty() && line.find(needle) == std::string::npos)
					continue;
				keep.push_back(line);
				if (static_cast<std::int64_t>(keep.size()) > tail)
					keep.erase(keep.begin());
			}

			return json{
				{ "file", path.string() },
				{ "linesScanned", scanned },
				{ "returned", keep.size() },
				{ "lines", keep }
			};
		}
	}

	void SetAllowMemoryWrites(bool a_allow)
	{
		g_allowWrites = a_allow;
	}

	void RegisterCommonTools(ToolRegistry& a_registry, EventBus&)
	{
		{
			ToolDescriptor ping;
			ping.name = "ping";
			ping.description = "Self-test. Returns { ok: true } plus which game answered.";
			ping.inputSchema = json{ { "type", "object" }, { "properties", json::object() } };
			ping.readOnly = true;
			a_registry.Register(std::move(ping), [](const json&, const ToolContext&) {
				const auto& id = host::Get();
				return json{ { "ok", true }, { "game", id.game }, { "exe", id.exe }, { "vr", id.vr } };
			});
		}

		{
			ToolDescriptor memory;
			memory.name = "memory";
			memory.description =
				"Read (and, when enabled, write) the game process's memory by ADDRESS EXPRESSION — "
				"the reverse-engineering half of the bench, available on every game. "
				"Grammar: expr := term (('+'|'-') term)* ; term := '[' expr ']' | 'base' | 0xHEX | DEC. "
				"'base' is the executable's load base and '[x]' dereferences a qword, so an RVA from a "
				"Ghidra session pastes in verbatim: addr='base+0x1d98ff0', or "
				"addr='[base+0x6239340]+4' to reach a field through a singleton pointer. "
				"action='resolve' returns { addr, rva|heap, base } without touching the target — an "
				"RVA is reported ONLY for addresses inside the image, so a heap pointer cannot be "
				"mistaken for one. action='read' (default) returns 'count' values of 'type' "
				"(u8|u16|u32|u64|i32|i64|f32|f64|ptr|bytes|cstr; count capped at 4096, and 'capped' "
				"says so). NaN/Inf come back as the strings \"NaN\"/\"Inf\", never as null. "
				"action='write' needs confirm=true AND allowMemoryWrites=true in config.json, and "
				"echoes { before, after, held } so you have an undo and can tell a write that stuck "
				"from one the engine overwrote. Every access is SEH-guarded: a bad address is a 400 "
				"naming the faulting address, never a crash.";
			memory.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "resolve", "read", "write" }) }, { "description", "default 'read'" } } },
									{ "addr", json{ { "type", "string" }, { "description", "address expression, e.g. 'base+0x1d98ff0' or '[base+0x6239340]+4'" } } },
									{ "type", json{ { "type", "string" }, { "enum", json::array({ "u8", "u16", "u32", "u64", "i32", "i64", "f32", "f64", "ptr", "bytes", "cstr" }) }, { "description", "default 'u32'" } } },
									{ "count", json{ { "type", "integer" }, { "description", "read: element count (cstr: byte length). Capped at 4096." } } },
									{ "value", json{ { "description", "write: the value. A string is parsed with strtoull, so \"0x41\" works." } } },
									{ "confirm", json{ { "type", "boolean" }, { "description", "write: must be true" } } } } },
				{ "required", json::array({ "addr" }) }
			};
			a_registry.Register(std::move(memory), &MemoryHandler);
		}

		{
			ToolDescriptor log;
			log.name = "log";
			log.description =
				"Read a script-extender plugin log from Documents/My Games/<game>/<extender>/. "
				"action='tail' (default) returns the last 'tail' lines of 'file' (default "
				"devbench.log), optionally filtered by 'grep' (a plain substring, applied BEFORE "
				"the tail — so grep+tail gives the last N matching lines, not the matches within "
				"the last N lines). action='list' enumerates the .log files present. Any plugin's "
                "log is readable, which is the point: the mod being debugged is usually not devbench.";
			log.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "tail", "list" }) } } },
									{ "file", json{ { "type", "string" }, { "description", "bare filename, default 'devbench.log'" } } },
									{ "tail", json{ { "type", "integer" }, { "description", "line count, 1..5000, default 80" } } },
									{ "grep", json{ { "type", "string" }, { "description", "substring filter applied before the tail" } } } } }
			};
			log.readOnly = true;
			a_registry.Register(std::move(log), &LogHandler);
		}
	}
}
