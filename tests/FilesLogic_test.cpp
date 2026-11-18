#include "test_framework.h"

#include "core/FilesLogic.h"

using namespace dvb::FilesLogic;

namespace
{
	std::vector<std::string> Segments(std::string_view a_path)
	{
		return SplitRelativePath(a_path, true).segments;
	}

	bool Matches(std::string_view a_pattern, std::string_view a_path)
	{
		return GlobMatch(Segments(a_pattern), Segments(a_path));
	}
}

TEST_CASE("a files root is data or mygames")
{
	CHECK(ParseRoot("data") == Root::Data);
	CHECK(ParseRoot("mygames") == Root::MyGames);
	CHECK(!ParseRoot("Data").has_value());
	CHECK(!ParseRoot("").has_value());
}

TEST_CASE("a relative path splits at either slash and drops empty and dot segments")
{
	const auto split = SplitRelativePath("MCM\\Settings//./FallUI.ini", false);
	CHECK(split.error.empty());
	const std::vector<std::string> expected{ "MCM", "Settings", "FallUI.ini" };
	CHECK(split.segments == expected);
	CHECK(JoinSegments(split.segments, split.segments.size()) == "MCM/Settings/FallUI.ini");
	CHECK(JoinSegments(split.segments, 2) == "MCM/Settings");
}

TEST_CASE("a path that could leave its root is refused")
{
	CHECK(!SplitRelativePath("", false).error.empty());
	CHECK(!SplitRelativePath("/Windows/win.ini", false).error.empty());
	CHECK(!SplitRelativePath("\\Windows\\win.ini", false).error.empty());
	CHECK(!SplitRelativePath("C:/Windows/win.ini", false).error.empty());
	CHECK(!SplitRelativePath("MCM/../../x.ini", false).error.empty());
	CHECK(!SplitRelativePath("file.ini:stream", false).error.empty());
	CHECK(!SplitRelativePath("./.", false).error.empty());
}

TEST_CASE("a wildcard is a glob's, and '**' must be a whole segment")
{
	CHECK(!SplitRelativePath("MCM/Settings/*.ini", false).error.empty());
	CHECK(SplitRelativePath("MCM/Settings/*.ini", true).error.empty());
	CHECK(SplitRelativePath("Mods_Config/**", true).error.empty());
	CHECK(!SplitRelativePath("Mods_Config/a**", true).error.empty());
}

TEST_CASE("a glob's walk starts at its folders with no wildcard")
{
	CHECK(LiteralPrefixLength(Segments("MCM/Settings/*.ini")) == 2);
	CHECK(LiteralPrefixLength(Segments("MCM/Settings/FallUI.ini")) == 2);
	CHECK(LiteralPrefixLength(Segments("ModsConfigFO4/Config/*/*")) == 2);
	CHECK(LiteralPrefixLength(Segments("**/*.toml")) == 0);
	CHECK(LiteralPrefixLength(Segments("x.ini")) == 0);
}

TEST_CASE("a glob matches names with '*' and '?', ignoring case")
{
	CHECK(Matches("MCM/Settings/*.ini", "mcm/settings/FallUI.INI"));
	CHECK(Matches("MCM/Settings/*", "MCM/Settings/Keybinds.json"));
	CHECK(Matches("a?c.txt", "abc.txt"));
	CHECK(!Matches("a?c.txt", "ac.txt"));
	CHECK(!Matches("MCM/Settings/*.ini", "MCM/Settings/Keybinds.json"));
	CHECK(!Matches("MCM/Settings/*.ini", "MCM/Settings/sub/FallUI.ini"));
	CHECK(!Matches("MCM/*", "MCM/Settings/FallUI.ini"));
}

TEST_CASE("a '**' segment matches any number of folders, none included")
{
	CHECK(Matches("Mods_Config/**", "Mods_Config/ModsConfigFO4VR/ModsConfigFO4VR.toml"));
	CHECK(Matches("Mods_Config/**", "Mods_Config/top.toml"));
	CHECK(Matches("**/*.toml", "FRIK_Config/FRIK.toml"));
	CHECK(Matches("**/*.toml", "FRIK.toml"));
	CHECK(Matches("a/**/b/*.ini", "a/b/x.ini"));
	CHECK(Matches("a/**/b/*.ini", "a/x/y/b/x.ini"));
	CHECK(!Matches("a/**/b/*.ini", "a/x/y/c/x.ini"));
	CHECK(!Matches("Mods_Config/**", "FRIK_Config/FRIK.toml"));
}

TEST_CASE("UTF-8 text is told from other bytes")
{
	CHECK(IsUtf8("plain ascii\r\n"));
	CHECK(IsUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
	CHECK(!IsUtf8("caf\xE9"));  // cp1252
	CHECK(!IsUtf8(std::string_view(
		"\xFF\xFE"
		"a\0",
		4)));                        // UTF-16 BOM
	CHECK(!IsUtf8("\xC0\xAF"));      // overlong
	CHECK(!IsUtf8("\xED\xA0\x80"));  // surrogate
	CHECK(!IsUtf8("\xE2\x82"));      // cut short
}

TEST_CASE("native code is refused by its extension or its PE header")
{
	CHECK(ExecutableRefusal("FRIK.toml", "[General]\n").empty());
	CHECK(ExecutableRefusal("Script.pex", "\xFA\x57\xC0\xDE").empty());
	CHECK(ExecutableRefusal("readme.dll.txt", "text").empty());
	CHECK(!ExecutableRefusal("evil.dll", "").empty());
	CHECK(!ExecutableRefusal("EVIL.DLL", "").empty());
	CHECK(!ExecutableRefusal("evil.dll.", "").empty());
	CHECK(!ExecutableRefusal("evil.dll . ", "").empty());
	CHECK(!ExecutableRefusal("loader.asi", "").empty());
	CHECK(!ExecutableRefusal("run.ps1", "").empty());
	CHECK(!ExecutableRefusal("innocent.ini", "MZ\x90\x00").empty());
}

TEST_CASE("base64 round-trips any bytes, with padding")
{
	CHECK(EncodeBase64("") == "");
	CHECK(EncodeBase64("f") == "Zg==");
	CHECK(EncodeBase64("fo") == "Zm8=");
	CHECK(EncodeBase64("foo") == "Zm9v");
	CHECK(EncodeBase64("foobar") == "Zm9vYmFy");
	const std::string bytes("\x00\xFF\r\n\x80 ini", 9);
	CHECK(DecodeBase64(EncodeBase64(bytes)) == bytes);
	CHECK(DecodeBase64("Zg==") == std::string("f"));
}

TEST_CASE("text that is not base64 is refused")
{
	CHECK(!DecodeBase64("Zg=").has_value());
	CHECK(!DecodeBase64("Z===").has_value());
	CHECK(!DecodeBase64("Zg==Zm8=").has_value());
	CHECK(!DecodeBase64("Zm9*").has_value());
}
