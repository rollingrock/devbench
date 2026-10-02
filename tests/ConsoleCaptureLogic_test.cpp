#include "test_framework.h"

#include "core/ConsoleCaptureLogic.h"
#include "core/Json.h"

using dvb::ConsoleLogCapture::kMarkerBegin;
using dvb::ConsoleLogCapture::kMarkerEnd;
using dvb::ConsoleLogCapture::kRingMax;
using dvb::ConsoleLogCapture::LineSampler;
using dvb::ConsoleLogCapture::SliceFencedLines;
using dvb::ConsoleLogCapture::SliceFencedText;

namespace
{
	std::string BeginLine()
	{
		return std::string("Script command \"") + kMarkerBegin + "\" not found.";
	}

	std::string EndLine()
	{
		return std::string("Script command \"") + kMarkerEnd + "\" not found.";
	}
}

TEST_CASE("text slicing returns the lines between the fence markers")
{
	const std::string text = "older output\n" + BeginLine() + "\nline one\nline two\n" + EndLine() + "\nlater\n";
	const auto        slice = SliceFencedText(text, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "line one");
	CHECK(slice.lines[1] == "line two");
}

TEST_CASE("text slicing handles CRLF and drops blank lines")
{
	const std::string text = BeginLine() + "\r\nalpha\r\n\r\nbeta\r\n" + EndLine() + "\r\n";
	const auto        slice = SliceFencedText(text, 200);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "alpha");
	CHECK(slice.lines[1] == "beta");
}

TEST_CASE("text slicing uses the last begin marker")
{
	const std::string text = BeginLine() + "\nold\n" + EndLine() + "\n" + BeginLine() + "\nnew\n" + EndLine() + "\n";
	const auto        slice = SliceFencedText(text, 200);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "new");
}

TEST_CASE("text slicing reports a missing end marker and a missing begin marker")
{
	const auto noEnd = SliceFencedText(BeginLine() + "\npartial\n", 200);
	CHECK(noEnd.sawBegin);
	CHECK(!noEnd.sawEnd);
	CHECK(noEnd.lines.size() == 1);

	const auto noBegin = SliceFencedText("just output\n" + EndLine() + "\n", 200);
	CHECK(!noBegin.sawBegin);
	CHECK(noBegin.lines.empty());

	CHECK(!SliceFencedText("", 200).sawBegin);
}

TEST_CASE("slicing keeps only the most recent lines when capped")
{
	std::string text = BeginLine() + "\n";
	for (int i = 0; i < 10; ++i)
		text += "row " + std::to_string(i) + "\n";
	text += EndLine() + "\n";
	const auto slice = SliceFencedText(text, 3);
	CHECK(slice.lines.size() == 3);
	CHECK(slice.lines.front() == "row 7");
	CHECK(slice.lines.back() == "row 9");
}

TEST_CASE("line slicing matches text slicing on the same fenced window")
{
	std::deque<std::string> lines{ "stale", BeginLine(), "one", "two", EndLine(), "after" };
	const auto              slice = SliceFencedLines(lines, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "one");

	std::deque<std::string> unfinished{ BeginLine(), "one" };
	const auto              partial = SliceFencedLines(unfinished, 200);
	CHECK(partial.sawBegin);
	CHECK(!partial.sawEnd);
	CHECK(SliceFencedLines({}, 200).lines.empty());
}

TEST_CASE("sampler records changes and ignores an unchanged line")
{
	LineSampler sampler;
	sampler.Reset("stale message");
	CHECK(sampler.Observe("stale message") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("first") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe("first") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("second") == LineSampler::Seen::kLine);
	CHECK(sampler.Samples() == 2);
	CHECK(sampler.Ticks() == 5);
}

TEST_CASE("sampler reports each marker once and slices a complete capture")
{
	LineSampler sampler;
	sampler.Reset("");
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.SawBegin());
	CHECK(sampler.Observe("GetActorValue: Health >> 100.00") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(sampler.SawEnd());

	const auto slice = SliceFencedLines(sampler.Lines(), 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "GetActorValue: Health >> 100.00");
}

TEST_CASE("a stale begin marker left by an aborted capture cannot swallow the next one")
{
	LineSampler sampler;
	sampler.Reset(BeginLine());  // seeded with the marker an aborted capture left showing
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.SawBegin());
	CHECK(sampler.Observe("output") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(SliceFencedLines(sampler.Lines(), 200).lines.size() == 1);
}

TEST_CASE("the previous capture's end marker is not taken as this capture's end")
{
	LineSampler sampler;
	sampler.Reset(EndLine());  // the last capture's end marker is still showing
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kNothing);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.Observe("output") == LineSampler::Seen::kLine);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(sampler.SawEnd());
	CHECK(SliceFencedLines(sampler.Lines(), 200).lines.size() == 1);
}

TEST_CASE("a command that prints nothing still ends on the end marker")
{
	LineSampler sampler;
	sampler.Reset("");
	sampler.Observe(BeginLine());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	const auto slice = SliceFencedLines(sampler.Lines(), 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.empty());
}

TEST_CASE("reset clears the previous capture")
{
	LineSampler sampler;
	sampler.Reset("");
	sampler.Observe(BeginLine());
	sampler.Observe("x");
	sampler.Observe(EndLine());
	sampler.Reset("seed");
	CHECK(!sampler.SawBegin());
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Lines().empty());
	CHECK(sampler.Samples() == 0);
	CHECK(sampler.Ticks() == 0);
}

TEST_CASE("the sampler ring is bounded")
{
	LineSampler sampler;
	sampler.Reset("");
	for (std::size_t i = 0; i < kRingMax + 40; ++i)
		sampler.Observe("line " + std::to_string(i));
	CHECK(sampler.Lines().size() == kRingMax);
	CHECK(sampler.Lines().back() == "line " + std::to_string(kRingMax + 39));
}

using dvb::ConsoleLogCapture::kQuietLooks;
using dvb::ConsoleLogCapture::kStableLooks;
using dvb::ConsoleLogCapture::QuietDetector;
using dvb::ConsoleLogCapture::SourceChooser;

TEST_CASE("the buffer is chosen once it holds the begin marker for several looks in a row")
{
	SourceChooser chooser;
	for (int i = 1; i < kStableLooks; ++i)
		CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kBuffer);
}

TEST_CASE("a marker that vanishes from the buffer falls back to the sampler")
{
	// The Console menu drains the buffer each frame: the marker shows once, then is gone.
	SourceChooser chooser;
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	SourceChooser::Choice choice = SourceChooser::Choice::kUndecided;
	for (int i = 0; i < kStableLooks && choice == SourceChooser::Choice::kUndecided; ++i)
		choice = chooser.Look(false, true);
	CHECK(choice == SourceChooser::Choice::kSampler);
}

TEST_CASE("a buffer marker that flickers never reaches a stable streak")
{
	SourceChooser chooser;
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(false, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kSampler);
}

TEST_CASE("nothing is chosen until a begin marker has been seen")
{
	SourceChooser chooser;
	for (int i = 0; i < 20; ++i)
		CHECK(chooser.Look(false, false) == SourceChooser::Choice::kUndecided);
}

TEST_CASE("a command counts as finished only after output and then quiet")
{
	QuietDetector detector;
	for (int i = 0; i < 10; ++i)
		CHECK(!detector.Look(false));  // a command that has not printed yet is never finished
	CHECK(!detector.Look(true));
	for (int i = 1; i < kQuietLooks; ++i)
		CHECK(!detector.Look(false));
	CHECK(detector.Look(false));
}

TEST_CASE("new output restarts the quiet count")
{
	QuietDetector detector;
	detector.Look(true);
	detector.Look(false);
	detector.Look(false);
	CHECK(!detector.Look(true));
	for (int i = 1; i < kQuietLooks; ++i)
		CHECK(!detector.Look(false));
	CHECK(detector.Look(false));
}

using dvb::ConsoleLogCapture::FindFence;

TEST_CASE("a fence before the starting offset belongs to an earlier capture")
{
	const std::string previous = BeginLine() + "\nold output\n" + EndLine() + "\n";
	const auto        state = FindFence(previous, previous.size());
	CHECK(!state.hasBegin);
	CHECK(!state.hasEnd);
	CHECK(!SliceFencedText(previous, 200, previous.size()).sawBegin);
}

TEST_CASE("a stale end marker cannot complete a new capture")
{
	const std::string previous = BeginLine() + "\nold output\n" + EndLine() + "\n";
	std::string       text = previous + BeginLine() + "\n";
	const auto        started = FindFence(text, previous.size());
	CHECK(started.hasBegin);
	CHECK(!started.hasEnd);

	text += "new output\n" + EndLine() + "\n";
	const auto finished = FindFence(text, previous.size());
	CHECK(finished.hasBegin);
	CHECK(finished.hasEnd);
	const auto slice = SliceFencedText(text, 200, previous.size());
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "new output");
}

TEST_CASE("fence detection with no offset finds the latest fence")
{
	const std::string text = BeginLine() + "\na\n" + EndLine() + "\n" + BeginLine() + "\n";
	const auto        state = FindFence(text);
	CHECK(state.hasBegin);
	CHECK(!state.hasEnd);
	CHECK(!FindFence("no markers here").hasBegin);
}

using dvb::ConsoleLogCapture::AppendedLines;
using dvb::ConsoleLogCapture::EscapeInvalidUtf8;
using dvb::ConsoleLogCapture::SplitConsoleCommands;

TEST_CASE("appended lines are what the buffer gained past the baseline")
{
	const std::string before = "player.getpos x\nGetPos: X >> -68.98\n";
	const std::string after = before + "player.getpos y\nGetPos: Y >> 12.50\n\n";
	const auto        appended = AppendedLines(after, before.size(), 200);
	CHECK(!appended.drained);
	CHECK(appended.omitted == 0);
	CHECK(appended.lines.size() == 2);
	CHECK(appended.lines[0] == "player.getpos y");
	CHECK(appended.lines[1] == "GetPos: Y >> 12.50");
	CHECK(AppendedLines(before, before.size(), 200).lines.empty());
}

TEST_CASE("a buffer shorter than the baseline was drained, so all of it is new")
{
	const auto appended = AppendedLines("GetPos: Z >> 3.00\n", 400, 200);
	CHECK(appended.drained);
	CHECK(appended.lines.size() == 1);
	CHECK(appended.lines[0] == "GetPos: Z >> 3.00");
}

TEST_CASE("appended lines keep the most recent and count the rest")
{
	const auto appended = AppendedLines("a\nb\nc\nd\ne\n", 0, 2);
	CHECK(appended.omitted == 3);
	CHECK(appended.lines.size() == 2);
	CHECK(appended.lines[0] == "d");
	CHECK(appended.lines[1] == "e");
}

TEST_CASE("invalid UTF-8 is escaped and valid UTF-8 is kept")
{
	CHECK(EscapeInvalidUtf8("plain text") == "plain text");
	CHECK(EscapeInvalidUtf8("Caf\xC3\xA9") == "Caf\xC3\xA9");                // e-acute, UTF-8
	CHECK(EscapeInvalidUtf8("Caf\xE9") == "Caf\\xE9");                       // e-acute, Windows-1252
	CHECK(EscapeInvalidUtf8("\xC0\xAF") == "\\xC0\\xAF");                    // overlong '/'
	CHECK(EscapeInvalidUtf8("\xED\xA0\x80") == "\\xED\\xA0\\x80");           // UTF-16 surrogate
	CHECK(EscapeInvalidUtf8("\xF4\x90\x80\x80") == "\\xF4\\x90\\x80\\x80");  // past U+10FFFF
	CHECK(EscapeInvalidUtf8("end\xE2\x82") == "end\\xE2\\x82");              // cut-off sequence
	CHECK(EscapeInvalidUtf8("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80");      // U+1F600
}

TEST_CASE("appended lines always serialise, whatever bytes the game printed")
{
	const auto appended = AppendedLines("Name: Caf\xE9 \xC0\n", 0, 200);
	CHECK(appended.lines.size() == 1);
	CHECK(appended.lines[0] == "Name: Caf\\xE9 \\xC0");
	CHECK_NOTHROW(dvb::json(appended.lines).dump());
}

TEST_CASE("a console line splits on semicolons outside quotes")
{
	const auto parts = SplitConsoleCommands("player.getpos y; player.getpos z ;;  tgm ");
	CHECK(parts.size() == 3);
	CHECK(parts[0] == "player.getpos y");
	CHECK(parts[1] == "player.getpos z");
	CHECK(parts[2] == "tgm");

	const auto quoted = SplitConsoleCommands("player.setname \"a;b\"; tcl");
	CHECK(quoted.size() == 2);
	CHECK(quoted[0] == "player.setname \"a;b\"");
	CHECK(quoted[1] == "tcl");

	CHECK(SplitConsoleCommands(" ; ").empty());
	CHECK(SplitConsoleCommands("help").size() == 1);
}
