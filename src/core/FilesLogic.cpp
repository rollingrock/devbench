#include "core/FilesLogic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

namespace dvb::FilesLogic
{
	namespace
	{
		constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		char Lower(char a_c)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(a_c)));
		}

		/// Whether one segment matches one pattern segment: '*' is any run of characters and
		/// '?' one character. Case is ignored, as Windows ignores it.
		bool SegmentMatch(std::string_view a_pattern, std::string_view a_text)
		{
			if (a_pattern.empty())
				return a_text.empty();
			if (a_pattern.front() == '*') {
				for (std::size_t skip = 0; skip <= a_text.size(); ++skip)
					if (SegmentMatch(a_pattern.substr(1), a_text.substr(skip)))
						return true;
				return false;
			}
			if (a_text.empty())
				return false;
			if (a_pattern.front() != '?' && Lower(a_pattern.front()) != Lower(a_text.front()))
				return false;
			return SegmentMatch(a_pattern.substr(1), a_text.substr(1));
		}

		bool GlobMatchFrom(const std::vector<std::string>& a_pattern, std::size_t a_p, const std::vector<std::string>& a_path, std::size_t a_s)
		{
			if (a_p == a_pattern.size())
				return a_s == a_path.size();
			if (a_pattern[a_p] == "**") {
				for (std::size_t s = a_s; s <= a_path.size(); ++s)
					if (GlobMatchFrom(a_pattern, a_p + 1, a_path, s))
						return true;
				return false;
			}
			return a_s < a_path.size() && SegmentMatch(a_pattern[a_p], a_path[a_s]) && GlobMatchFrom(a_pattern, a_p + 1, a_path, a_s + 1);
		}
	}

	/// The root named by the tool's 'root' argument: "data" or "mygames".
	std::optional<Root> ParseRoot(std::string_view a_name)
	{
		if (a_name == "data")
			return Root::Data;
		if (a_name == "mygames")
			return Root::MyGames;
		return std::nullopt;
	}

	/// Split a path relative to a root at '/' and '\', dropping empty and "." segments. A path
	/// that could leave the root is refused: an absolute one, a drive, a ':' (also an NTFS
	/// stream), a ".." segment. Wildcards ('*', '?', and "**" as a whole segment) are a glob's.
	RelativePath SplitRelativePath(std::string_view a_path, bool a_allowWildcards)
	{
		RelativePath result;
		if (a_path.empty()) {
			result.error = "the path is empty";
			return result;
		}
		if (a_path.front() == '/' || a_path.front() == '\\') {
			result.error = "the path must be relative to the root";
			return result;
		}
		std::size_t start = 0;
		while (start <= a_path.size()) {
			const auto end = std::min(a_path.find_first_of("/\\", start), a_path.size());
			const auto segment = a_path.substr(start, end - start);
			start = end + 1;
			if (segment.empty() || segment == ".")
				continue;
			if (segment == "..") {
				result.error = "the path must not contain '..'";
				return result;
			}
			if (segment.find_first_of(":<>\"|") != std::string_view::npos || std::ranges::any_of(segment, [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
				result.error = "the path has a character a file name cannot have: '" + std::string(segment) + "'";
				return result;
			}
			if (!a_allowWildcards && HasWildcard(segment)) {
				result.error = "the path has a wildcard, which only a list takes";
				return result;
			}
			if (segment.find("**") != std::string_view::npos && segment != "**") {
				result.error = "'**' must be a whole segment: '" + std::string(segment) + "'";
				return result;
			}
			result.segments.emplace_back(segment);
		}
		if (result.segments.empty())
			result.error = "the path names no file";
		return result;
	}

	/// The first a_count segments joined with '/'.
	std::string JoinSegments(const std::vector<std::string>& a_segments, std::size_t a_count)
	{
		std::string joined;
		for (std::size_t i = 0; i < std::min(a_count, a_segments.size()); ++i) {
			if (i > 0)
				joined += '/';
			joined += a_segments[i];
		}
		return joined;
	}

	bool HasWildcard(std::string_view a_segment)
	{
		return a_segment.find_first_of("*?") != std::string_view::npos;
	}

	/// How many leading segments of a glob are folders with no wildcard: the folder a list
	/// starts its walk from. The last segment always names files, so it is never counted.
	std::size_t LiteralPrefixLength(const std::vector<std::string>& a_pattern)
	{
		std::size_t count = 0;
		while (count + 1 < a_pattern.size() && !HasWildcard(a_pattern[count]))
			++count;
		return count;
	}

	/// Whether a path's segments match a glob's: '*' and '?' within a segment, and a "**"
	/// segment for any number of folders, none included. Case is ignored.
	bool GlobMatch(const std::vector<std::string>& a_pattern, const std::vector<std::string>& a_path)
	{
		return GlobMatchFrom(a_pattern, 0, a_path, 0);
	}

	/// Whether the bytes are well-formed UTF-8, so a read can hand them back as JSON text.
	bool IsUtf8(std::string_view a_text)
	{
		std::size_t i = 0;
		while (i < a_text.size()) {
			const auto    c = static_cast<unsigned char>(a_text[i]);
			std::size_t   length = 0;
			std::uint32_t code = 0;
			if (c < 0x80) {
				++i;
				continue;
			}
			if ((c & 0xE0) == 0xC0) {
				length = 2;
				code = c & 0x1F;
			} else if ((c & 0xF0) == 0xE0) {
				length = 3;
				code = c & 0x0F;
			} else if ((c & 0xF8) == 0xF0) {
				length = 4;
				code = c & 0x07;
			} else {
				return false;
			}
			if (i + length > a_text.size())
				return false;
			for (std::size_t k = 1; k < length; ++k) {
				const auto next = static_cast<unsigned char>(a_text[i + k]);
				if ((next & 0xC0) != 0x80)
					return false;
				code = (code << 6) | (next & 0x3F);
			}
			constexpr std::array<std::uint32_t, 5> kMinimum{ 0, 0, 0x80, 0x800, 0x10000 };
			if (code < kMinimum[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
				return false;
			i += length;
		}
		return true;
	}

	/// Why a write is refused as native code, or empty when it is allowed: a name ending in an
	/// extension Windows or the script extender runs, or bytes that start with a PE header
	/// ("MZ") whatever the name. Windows drops trailing dots and spaces from a file name, so
	/// "x.dll." is x.dll and is refused too.
	std::string ExecutableRefusal(std::string_view a_fileName, std::string_view a_content)
	{
		constexpr std::array<std::string_view, 6> kRefused{ ".dll", ".exe", ".asi", ".bat", ".cmd", ".ps1" };

		auto name = a_fileName;
		while (!name.empty() && (name.back() == '.' || name.back() == ' '))
			name.remove_suffix(1);
		for (const auto extension : kRefused) {
			if (name.size() >= extension.size() &&
				std::ranges::equal(name.substr(name.size() - extension.size()), extension, [](char a, char b) { return Lower(a) == b; }))
				return "files: a '" + std::string(extension) + "' file is executable code, which files never writes";
		}
		if (a_content.starts_with("MZ"))
			return "files: the content starts with a Windows executable header ('MZ'), which files never writes";
		return {};
	}

	/// Standard base64 with '=' padding.
	std::string EncodeBase64(std::string_view a_bytes)
	{
		std::string out;
		out.reserve((a_bytes.size() + 2) / 3 * 4);
		for (std::size_t i = 0; i < a_bytes.size(); i += 3) {
			const auto    remaining = a_bytes.size() - i;
			std::uint32_t chunk = static_cast<unsigned char>(a_bytes[i]) << 16;
			if (remaining > 1)
				chunk |= static_cast<unsigned char>(a_bytes[i + 1]) << 8;
			if (remaining > 2)
				chunk |= static_cast<unsigned char>(a_bytes[i + 2]);
			out += kBase64Alphabet[(chunk >> 18) & 0x3F];
			out += kBase64Alphabet[(chunk >> 12) & 0x3F];
			out += remaining > 1 ? kBase64Alphabet[(chunk >> 6) & 0x3F] : '=';
			out += remaining > 2 ? kBase64Alphabet[chunk & 0x3F] : '=';
		}
		return out;
	}

	/// The bytes of standard base64, or nullopt for text that is not: a length that is not a
	/// multiple of 4, a character outside the alphabet, or padding before the end.
	std::optional<std::string> DecodeBase64(std::string_view a_text)
	{
		if (a_text.size() % 4 != 0)
			return std::nullopt;
		std::string out;
		out.reserve(a_text.size() / 4 * 3);
		for (std::size_t i = 0; i < a_text.size(); i += 4) {
			const bool    last = i + 4 == a_text.size();
			std::uint32_t chunk = 0;
			int           padding = 0;
			for (std::size_t k = 0; k < 4; ++k) {
				const char    c = a_text[i + k];
				std::uint32_t value = 0;
				if (c == '=') {
					if (!last || k < 2)
						return std::nullopt;
					++padding;
				} else {
					if (padding > 0)
						return std::nullopt;
					const auto at = kBase64Alphabet.find(c);
					if (at == std::string_view::npos)
						return std::nullopt;
					value = static_cast<std::uint32_t>(at);
				}
				chunk = (chunk << 6) | value;
			}
			out += static_cast<char>((chunk >> 16) & 0xFF);
			if (padding < 2)
				out += static_cast<char>((chunk >> 8) & 0xFF);
			if (padding < 1)
				out += static_cast<char>(chunk & 0xFF);
		}
		return out;
	}
}
