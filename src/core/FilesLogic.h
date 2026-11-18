#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The testable half of the `files` tool: which paths it accepts under its roots, how a
// glob matches them, and the encodings a file's bytes travel in. The tool itself
// (CommonTools.cpp) does the file system calls.

namespace dvb::FilesLogic
{
	/// The folders the tool reads and writes under: the game's Data folder, as the game
	/// sees it (so a mod manager's virtual files are in it), and Documents/My Games/<game>.
	enum class Root
	{
		Data,
		MyGames
	};

	std::optional<Root> ParseRoot(std::string_view a_name);

	/// A path relative to a root, split into its segments, or the reason it is refused.
	struct RelativePath
	{
		std::vector<std::string> segments;
		std::string              error;
	};

	RelativePath SplitRelativePath(std::string_view a_path, bool a_allowWildcards);

	std::string JoinSegments(const std::vector<std::string>& a_segments, std::size_t a_count);

	bool HasWildcard(std::string_view a_segment);

	std::size_t LiteralPrefixLength(const std::vector<std::string>& a_pattern);

	bool GlobMatch(const std::vector<std::string>& a_pattern, const std::vector<std::string>& a_path);

	bool IsUtf8(std::string_view a_text);

	std::string ExecutableRefusal(std::string_view a_fileName, std::string_view a_content);

	std::string EncodeBase64(std::string_view a_bytes);

	std::optional<std::string> DecodeBase64(std::string_view a_text);
}
