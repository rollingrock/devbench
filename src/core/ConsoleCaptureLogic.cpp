#include "core/ConsoleCaptureLogic.h"

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		bool Contains(std::string_view a_text, const char* a_token)
		{
			return a_text.find(a_token) != std::string_view::npos;
		}

		void TrimToMostRecent(std::vector<std::string>& a_lines, std::size_t a_maxLines)
		{
			if (a_lines.size() > a_maxLines)
				a_lines.erase(a_lines.begin(), a_lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		}

		// Splits on '\n' and '\r', dropping blank lines.
		void SplitLines(std::string_view a_text, std::vector<std::string>& a_out)
		{
			std::string line;
			const auto  flush = [&]() {
				if (!line.empty()) {
					a_out.push_back(line);
					line.clear();
				}
			};
			for (const char c : a_text) {
				if (c == '\n' || c == '\r')
					flush();
				else
					line += c;
			}
			flush();
		}

		// Length of the well-formed UTF-8 sequence at a_text[a_at], or 0 if there is none. The
		// same rules the serialiser enforces: no overlong forms, no surrogates, nothing past U+10FFFF.
		std::size_t Utf8SequenceLength(std::string_view a_text, std::size_t a_at)
		{
			const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(a_text[i]); };
			const auto c = byte(a_at);
			if (c < 0x80)
				return 1;
			std::size_t   length = 0;
			unsigned char low = 0x80;
			unsigned char high = 0xBF;
			if (c >= 0xC2 && c <= 0xDF) {
				length = 2;
			} else if (c >= 0xE0 && c <= 0xEF) {
				length = 3;
				if (c == 0xE0)
					low = 0xA0;
				else if (c == 0xED)
					high = 0x9F;
			} else if (c >= 0xF0 && c <= 0xF4) {
				length = 4;
				if (c == 0xF0)
					low = 0x90;
				else if (c == 0xF4)
					high = 0x8F;
			} else {
				return 0;
			}
			if (a_at + length > a_text.size())
				return 0;
			if (byte(a_at + 1) < low || byte(a_at + 1) > high)
				return 0;
			for (std::size_t i = 2; i < length; ++i) {
				if ((byte(a_at + i) & 0xC0) != 0x80)
					return 0;
			}
			return length;
		}
	}

	FenceState FindFence(std::string_view a_text, std::size_t a_fromOffset)
	{
		FenceState        state;
		const std::size_t begin = a_text.rfind(kMarkerBegin);
		if (begin == std::string_view::npos || begin < a_fromOffset)
			return state;
		state.hasBegin = true;
		state.hasEnd = a_text.find(kMarkerEnd, begin) != std::string_view::npos;
		return state;
	}

	Slice SliceFencedText(std::string_view a_text, std::size_t a_maxLines, std::size_t a_fromOffset)
	{
		Slice             out;
		const std::size_t begin = a_text.rfind(kMarkerBegin);
		if (begin == std::string_view::npos || begin < a_fromOffset)
			return out;
		out.sawBegin = true;
		const std::size_t end = a_text.find(kMarkerEnd, begin);

		std::size_t start = a_text.find('\n', begin);
		start = (start == std::string_view::npos) ? a_text.size() : start + 1;
		std::size_t stop = a_text.size();
		if (end != std::string_view::npos) {
			out.sawEnd = true;
			const std::size_t lineStart = a_text.rfind('\n', end);
			stop = (lineStart == std::string_view::npos || lineStart < start) ? start : lineStart;
		}

		SplitLines(a_text.substr(start, stop - start), out.lines);
		TrimToMostRecent(out.lines, a_maxLines);
		return out;
	}

	Appended AppendedLines(std::string_view a_text, std::size_t a_baseline, std::size_t a_maxLines)
	{
		Appended out;
		out.drained = a_text.size() < a_baseline;
		std::vector<std::string> lines;
		SplitLines(out.drained ? a_text : a_text.substr(a_baseline), lines);
		if (lines.size() > a_maxLines)
			out.omitted = lines.size() - a_maxLines;
		TrimToMostRecent(lines, a_maxLines);
		out.lines.reserve(lines.size());
		for (const auto& line : lines)
			out.lines.push_back(EscapeInvalidUtf8(line));
		return out;
	}

	std::vector<std::string> SplitConsoleCommands(std::string_view a_line)
	{
		std::vector<std::string> out;
		const auto               push = [&](std::string_view a_part) {
			const auto first = a_part.find_first_not_of(" \t");
			if (first == std::string_view::npos)
				return;
			const auto last = a_part.find_last_not_of(" \t");
			out.emplace_back(a_part.substr(first, last - first + 1));
		};
		bool        quoted = false;
		std::size_t start = 0;
		for (std::size_t i = 0; i < a_line.size(); ++i) {
			if (a_line[i] == '"')
				quoted = !quoted;
			else if (a_line[i] == ';' && !quoted) {
				push(a_line.substr(start, i - start));
				start = i + 1;
			}
		}
		push(a_line.substr(start));
		return out;
	}

	std::string EscapeInvalidUtf8(std::string_view a_text)
	{
		static constexpr char kHex[] = "0123456789ABCDEF";
		std::string           out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size();) {
			if (const auto length = Utf8SequenceLength(a_text, i); length > 0) {
				out.append(a_text.substr(i, length));
				i += length;
				continue;
			}
			const auto c = static_cast<unsigned char>(a_text[i++]);
			out += "\\x";
			out += kHex[c >> 4];
			out += kHex[c & 0xF];
		}
		return out;
	}

	Slice SliceFencedLines(const std::deque<std::string>& a_lines, std::size_t a_maxLines)
	{
		Slice       out;
		std::size_t begin = a_lines.size();
		for (std::size_t i = a_lines.size(); i-- > 0;) {
			if (Contains(a_lines[i], kMarkerBegin)) {
				begin = i;
				break;
			}
		}
		if (begin >= a_lines.size())
			return out;
		out.sawBegin = true;

		std::size_t end = a_lines.size();
		for (std::size_t i = begin + 1; i < a_lines.size(); ++i) {
			if (Contains(a_lines[i], kMarkerEnd)) {
				end = i;
				out.sawEnd = true;
				break;
			}
		}
		for (std::size_t i = begin + 1; i < end; ++i)
			if (!a_lines[i].empty())
				out.lines.push_back(a_lines[i]);
		TrimToMostRecent(out.lines, a_maxLines);
		return out;
	}

	void LineSampler::Reset(std::string a_seedLine)
	{
		m_lines.clear();
		m_lastSeen = std::move(a_seedLine);
		m_samples = 0;
		m_ticks = 0;
		m_sawBegin = false;
		m_sawEnd = false;
	}

	void LineSampler::Record(std::string_view a_line)
	{
		m_lastSeen.assign(a_line);
		++m_samples;
		m_lines.emplace_back(a_line);
		while (m_lines.size() > kRingMax)
			m_lines.pop_front();
	}

	LineSampler::Seen LineSampler::Observe(std::string_view a_line)
	{
		++m_ticks;
		if (a_line.empty())
			return Seen::kNothing;
		// A marker is recorded the first time it shows even if it matches the seed, so a stale
		// marker left by an aborted capture cannot swallow this capture's own.
		if (Contains(a_line, kMarkerBegin) && !m_sawBegin) {
			m_sawBegin = true;
			Record(a_line);
			return Seen::kBegin;
		}
		if (Contains(a_line, kMarkerEnd) && m_sawBegin && !m_sawEnd) {
			m_sawEnd = true;
			Record(a_line);
			return Seen::kEnd;
		}
		if (a_line == m_lastSeen)
			return Seen::kNothing;
		Record(a_line);
		return Seen::kLine;
	}

	SourceChooser::Choice SourceChooser::Look(bool a_bufferHasBegin, bool a_samplerSawBegin)
	{
		m_bufferStreak = a_bufferHasBegin ? m_bufferStreak + 1 : 0;
		if (a_samplerSawBegin)
			++m_samplerLooks;
		if (m_bufferStreak >= kStableLooks)
			return Choice::kBuffer;
		if (m_samplerLooks >= kStableLooks)
			return Choice::kSampler;
		return Choice::kUndecided;
	}

	bool QuietDetector::Look(bool a_newLine)
	{
		if (a_newLine) {
			m_sawLine = true;
			m_quiet = 0;
		} else if (m_sawLine) {
			++m_quiet;
		}
		return m_sawLine && m_quiet >= kQuietLooks;
	}
}
