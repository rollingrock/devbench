#include "core/tools/CommonTools.h"

#include "core/EventBus.h"
#include "core/FilesLogic.h"
#include "core/Host.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/tools/Memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
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

				// Stop at the terminator, then ESCAPE anything that is not printable
				// ASCII. Handing raw bytes straight to json made this type unusable for
				// its entire purpose: nlohmann refuses to serialise invalid UTF-8 and
				// throws, so probing an address that turned out NOT to be a string
				// answered 500 — the one answer that cannot be distinguished from the
				// server being broken. A probe must be able to say "that is not text".
				const std::string_view raw{ buf, std::strlen(buf) };
				std::string            value;
				value.reserve(raw.size());
				std::size_t printable = 0;
				for (const unsigned char c : raw) {
					if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c <= 0x7E)) {
						value += static_cast<char>(c);
						++printable;
					} else {
						value += std::format("\\x{:02X}", c);
					}
				}
				out["value"] = value;
				out["length"] = raw.size();
				// Lets a caller filter on "is this actually text" without re-deriving it
				// from escapes, which is the check every scan over unknown memory wants.
				out["printable"] = raw.empty() ? false : (printable == raw.size());
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

			const auto    path = dir / name;
			std::ifstream in(path);
			if (!in)
				throw ToolError(404, "log: cannot open " + path.string());

			std::int64_t      tail = std::clamp<std::int64_t>(a_args.value("tail", 80), 1, 5000);
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

		constexpr std::uintmax_t kMaxFileBytes = 16u * 1024 * 1024;
		constexpr std::size_t    kMaxListed = 10000;

		/// The folder a root names, absolute: the game's Data folder as the game sees it, or
		/// Documents/My Games/<game>, the folder above the extender's log folder.
		std::filesystem::path RootDir(FilesLogic::Root a_root)
		{
			if (a_root == FilesLogic::Root::Data) {
				std::error_code ec;
				auto            dir = std::filesystem::absolute("Data", ec);
				if (ec)
					throw ToolError(500, "files: cannot resolve the game's Data folder: " + ec.message());
				return dir;
			}
			const auto logDir = host::LogDir();
			if (logDir.empty())
				throw ToolError(500, "files: the script extender did not resolve Documents/My Games/<game>");
			return logDir.parent_path();
		}

		/// The first a_count segments of a relative path, under a_root. The segments are UTF-8.
		std::filesystem::path UnderRoot(const std::filesystem::path& a_root, const std::vector<std::string>& a_segments, std::size_t a_count)
		{
			auto path = a_root;
			for (std::size_t i = 0; i < std::min(a_count, a_segments.size()); ++i)
				path /= std::u8string_view(reinterpret_cast<const char8_t*>(a_segments[i].data()), a_segments[i].size());
			return path;
		}

		/// A file's last write time in milliseconds since the Unix epoch, 0 when it can't be read.
		std::int64_t ModifiedMs(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			const auto      time = std::filesystem::last_write_time(a_path, ec);
			if (ec)
				return 0;
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::clock_cast<std::chrono::system_clock>(time).time_since_epoch()).count();
		}

		/// The segments of the 'path' argument, refused with a 400 that says why.
		std::vector<std::string> PathArgument(const json& a_args, bool a_allowWildcards)
		{
			const auto path = a_args.value("path", std::string{});
			auto       split = FilesLogic::SplitRelativePath(path, a_allowWildcards);
			if (!split.error.empty())
				throw ToolError(400, "files: " + split.error);
			return std::move(split.segments);
		}

		/// Every file under the root whose path matches the glob, sorted by path. The walk
		/// starts at the glob's folders with no wildcard, and goes only as deep as the glob
		/// reaches unless it has a "**". A walk that fails partway returns what it found with
		/// 'incomplete' and the reason; a file whose size can't be read has null 'bytes'.
		json ListFiles(const std::filesystem::path& a_root, const std::vector<std::string>& a_pattern)
		{
			struct Listed
			{
				std::string                   path;
				std::optional<std::uintmax_t> bytes;
				std::int64_t                  modifiedMs;
			};

			const auto prefix = FilesLogic::LiteralPrefixLength(a_pattern);
			const auto base = UnderRoot(a_root, a_pattern, prefix);
			const bool anyDepth = std::ranges::find(a_pattern, std::string("**")) != a_pattern.end();
			const auto depth = static_cast<int>(a_pattern.size() - prefix);

			std::vector<Listed> listed;
			bool                capped = false;
			std::error_code     ec;
			const bool          isDirectory = std::filesystem::is_directory(base, ec);
			// MSVC reports a missing folder as an error; it is a glob that matches nothing.
			if (ec == std::errc::no_such_file_or_directory)
				ec.clear();
			if (isDirectory) {
				for (auto it = std::filesystem::recursive_directory_iterator(base, std::filesystem::directory_options::skip_permission_denied, ec);
					!ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
					if (!anyDepth && it.depth() + 1 >= depth)
						it.disable_recursion_pending();
					std::error_code fileEc;
					if (!it->is_regular_file(fileEc))
						continue;
					std::vector<std::string> segments(a_pattern.begin(), a_pattern.begin() + static_cast<std::ptrdiff_t>(prefix));
					for (const auto& part : it->path().lexically_relative(base))
						segments.push_back(Utf8(part));
					if (!FilesLogic::GlobMatch(a_pattern, segments))
						continue;
					if (listed.size() == kMaxListed) {
						capped = true;
						break;
					}
					const auto bytes = it->file_size(fileEc);
					listed.push_back({ FilesLogic::JoinSegments(segments, segments.size()), fileEc ? std::nullopt : std::optional(bytes), ModifiedMs(it->path()) });
				}
			}
			std::ranges::sort(listed, {}, &Listed::path);

			json files = json::array();
			for (const auto& file : listed)
				files.push_back(json{ { "path", file.path }, { "bytes", file.bytes ? json(static_cast<std::uint64_t>(*file.bytes)) : json(nullptr) }, { "modifiedMs", file.modifiedMs } });
			json out{ { "count", files.size() }, { "files", std::move(files) } };
			if (capped)
				out["capped"] = true;
			if (ec) {
				out["incomplete"] = true;
				out["reason"] = ec.message();
			}
			return out;
		}

		json FilesHandler(const json& a_args, const ToolContext&)
		{
			const auto action = a_args.value("action", std::string{});
			const auto rootName = a_args.value("root", std::string{});
			const auto root = FilesLogic::ParseRoot(rootName);
			if (!root)
				throw ToolError(400, "files: 'root' must be 'data' or 'mygames'");
			const auto rootDir = RootDir(*root);

			if (action == "list") {
				auto out = ListFiles(rootDir, PathArgument(a_args, true));
				out["root"] = rootName;
				out["rootDir"] = Utf8(rootDir);
				return out;
			}

			const auto segments = PathArgument(a_args, false);
			const auto relative = FilesLogic::JoinSegments(segments, segments.size());
			const auto file = UnderRoot(rootDir, segments, segments.size());
			const auto encoding = a_args.value("encoding", std::string("text"));
			if (encoding != "text" && encoding != "base64")
				throw ToolError(400, "files: 'encoding' must be 'text' or 'base64'");
			json            out{ { "root", rootName }, { "path", relative } };
			std::error_code ec;

			if (action == "read") {
				if (!std::filesystem::is_regular_file(file, ec))
					throw ToolError(404, "files: no file '" + relative + "' under " + rootName);
				const auto bytes = std::filesystem::file_size(file, ec);
				if (ec)
					throw ToolError(500, "files: cannot read the size of '" + relative + "': " + ec.message());
				if (bytes > kMaxFileBytes)
					throw ToolError(422, std::format("files: '{}' has {} bytes, more than the {} a read returns", relative, bytes, kMaxFileBytes));
				std::ifstream in(file, std::ios::binary);
				if (!in)
					throw ToolError(500, "files: cannot open '" + relative + "'");
				std::string content(static_cast<std::size_t>(bytes), '\0');
				in.read(content.data(), static_cast<std::streamsize>(content.size()));
				content.resize(static_cast<std::size_t>(in.gcount()));
				if (encoding == "text" && !FilesLogic::IsUtf8(content))
					throw ToolError(422, "files: '" + relative + "' is not UTF-8 text; read it with encoding='base64'");
				out["bytes"] = content.size();
				out["modifiedMs"] = ModifiedMs(file);
				out["encoding"] = encoding;
				out["content"] = encoding == "base64" ? FilesLogic::EncodeBase64(content) : std::move(content);
				return out;
			}

			if (action == "write") {
				if (!a_args.contains("content") || !a_args["content"].is_string())
					throw ToolError(400, "files: a write needs 'content', a string");
				const auto& text = a_args["content"].get_ref<const std::string&>();
				std::string content;
				if (encoding == "base64") {
					auto decoded = FilesLogic::DecodeBase64(text);
					if (!decoded)
						throw ToolError(400, "files: 'content' is not base64");
					content = std::move(*decoded);
				} else {
					content = text;
				}
				if (const auto refusal = FilesLogic::ExecutableRefusal(segments.back(), content); !refusal.empty())
					throw ToolError(403, refusal);
				if (content.size() > kMaxFileBytes)
					throw ToolError(422, std::format("files: {} bytes is more than the {} a write takes", content.size(), kMaxFileBytes));
				if (std::filesystem::is_directory(file, ec))
					throw ToolError(409, "files: '" + relative + "' is a folder");
				const bool existed = std::filesystem::exists(file, ec);
				std::filesystem::create_directories(file.parent_path(), ec);
				if (ec)
					throw ToolError(500, "files: cannot create the folder of '" + relative + "': " + ec.message());
				{
					std::ofstream outFile(file, std::ios::binary | std::ios::trunc);
					if (!outFile || !outFile.write(content.data(), static_cast<std::streamsize>(content.size())))
						throw ToolError(500, "files: cannot write '" + relative + "'");
				}
				dlog::info("files: wrote {} bytes to {} '{}'", content.size(), rootName, relative);
				out["bytes"] = content.size();
				out["created"] = !existed;
				out["modifiedMs"] = ModifiedMs(file);
				return out;
			}

			if (action == "delete") {
				if (!std::filesystem::exists(file, ec)) {
					out["deleted"] = false;
					return out;
				}
				if (!std::filesystem::is_regular_file(file, ec))
					throw ToolError(409, "files: '" + relative + "' is not a file; only a file is deleted");
				if (!std::filesystem::remove(file, ec) || ec)
					throw ToolError(500, "files: cannot delete '" + relative + "': " + ec.message());
				dlog::info("files: deleted {} '{}'", rootName, relative);
				out["deleted"] = true;
				return out;
			}

			throw ToolError(400, "files: unknown action '" + action + "' (list|read|write|delete)");
		}

		// the longest quit watchdog: ten minutes
		constexpr std::int64_t kMaxKillAfterMs = 600000;

		/**
		 * Asks the game to quit on the main thread and answers before it goes down. With
		 * killAfterMs, a detached thread terminates the process if it still runs by then:
		 * a shutdown that hangs has usually stopped this listener, so no later call could.
		 */
		json QuitHandler(const json& a_args, const ToolContext&)
		{
			const json& raw = a_args.contains("killAfterMs") ? a_args["killAfterMs"] : json(0);
			if (!raw.is_number_integer() || raw.get<std::int64_t>() < 0 || raw.get<std::int64_t>() > kMaxKillAfterMs)
				throw ToolError(400, std::format("quit: 'killAfterMs' must be an integer from 0 to {}", kMaxKillAfterMs));
			const auto killAfterMs = raw.get<std::int64_t>();

			const auto refused = MainThread::RunAndWait([]() -> json { return host::RequestQuit(); }).get<std::string>();
			if (!refused.empty())
				throw ToolError(500, "quit: " + refused);

			if (killAfterMs > 0) {
				std::thread([killAfterMs] {
					std::this_thread::sleep_for(std::chrono::milliseconds(killAfterMs));
					dlog::warn("quit: still running {} ms after the quit; terminating the process", killAfterMs);
					::TerminateProcess(::GetCurrentProcess(), 1);
				}).detach();
			}
			const auto pid = static_cast<int>(::GetCurrentProcessId());
			dlog::info("quit: requested (pid {}, killAfterMs {})", pid, killAfterMs);
			return json{ { "pid", pid }, { "quitting", true }, { "killAfterMs", killAfterMs } };
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
				"says so). NaN/Inf come back as the strings \"NaN\"/\"Inf\", never as null. 'cstr' "
				"returns { value, length, printable } with non-printable bytes escaped as \\xNN, so "
				"probing an address that turns out NOT to hold text answers 'printable: false' "
				"rather than failing — a scan over unknown memory needs that to be a readable answer. "
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

		{
			ToolDescriptor files;
			files.name = "files";
			files.description =
				"List, read, write and delete files under two roots: 'data', the game's Data folder as "
				"the game sees it, so a mod manager's virtual files (MO2's mods and overwrite) are in "
				"it and a write lands where the game's own write would; and 'mygames', "
				"Documents/My Games/<game>. 'path' is relative to the root, '/' or '\\' separated; "
				"one that leaves it (absolute, a drive, '..') is refused. action='list' takes a glob "
				"('*' and '?' in a name, '**' for any folders, case ignored) and returns { count, files: "
				"[{ path, bytes, modifiedMs }], capped } sorted by path, at most 10000; a walk that "
				"fails partway adds 'incomplete' and 'reason'. 'read' returns "
				"{ path, bytes, modifiedMs, encoding, content }: 'text' (default) for UTF-8, 'base64' "
				"for any bytes, so a backup round-trips exactly. 'write' creates or replaces the file "
				"and its folders from 'content' in 'encoding', and says whether it 'created' it; it "
				"never writes native code (.dll, .exe, .asi, .bat, .cmd, .ps1, or bytes starting 'MZ'). "
				"'delete' removes one file; 'deleted' is false when there was none. Files up to 16 MB. "
				"Runs on the listener thread, so it answers at the main menu and while the game is "
				"stalled.";
			files.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "read", "write", "delete" }) } } },
									{ "root", json{ { "type", "string" }, { "enum", json::array({ "data", "mygames" }) } } },
									{ "path", json{ { "type", "string" }, { "description", "relative to the root; list: a glob, e.g. 'MCM/Settings/*.ini' or 'Mods_Config/**'" } } },
									{ "content", json{ { "type", "string" }, { "description", "write: the file's content, in 'encoding'" } } },
									{ "encoding", json{ { "type", "string" }, { "enum", json::array({ "text", "base64" }) }, { "description", "read, write: default 'text' (UTF-8)" } } } } },
				{ "required", json::array({ "action", "root", "path" }) }
			};
			a_registry.Register(std::move(files), &FilesHandler);
		}

		{
			ToolDescriptor quit;
			quit.name = "quit";
			quit.description =
				"Quit the game to the desktop through the engine's own shutdown, the path the console's qqq "
				"takes: no save, no prompt. Answers { pid, quitting: true, killAfterMs } at once; the game goes "
				"down on a later frame, so this port stops answering soon after. To know it is gone, wait for "
				"process 'pid' to exit, not for the port: shutdown takes seconds and can hang. killAfterMs > 0 "
				"arms a watchdog that terminates the process if it still runs that many ms after the call, for "
				"a shutdown that hangs (it skips whatever cleanup was left); 0 (default) never kills.";
			quit.inputSchema = json{
				{ "type", "object" },
				{ "properties", json{
									{ "killAfterMs", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", kMaxKillAfterMs }, { "description", "terminate the process if it still runs this long after the call; default 0, never" } } } } }
			};
			a_registry.Register(std::move(quit), &QuitHandler);
		}
	}
}
