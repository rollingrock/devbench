#pragma once

#include <format>
#include <string>
#include <string_view>

// Core logging seam.
//
// The core must not depend on a script extender, so it cannot call CommonLib's
// `logs::` (SKSE::log) / `logger::` (F4SE) aliases directly. `dvb::dlog` is the
// core-side facade: same `{}`-placeholder call style, but every line is handed to
// a sink the platform installs at startup, so core log lines land in the same file
// as the platform's own.
//
// Deliberately NOT named `logs`: a `dvb::logs` would silently shadow the global
// CommonLibSSE alias inside every `namespace dvb` translation unit, which is the
// kind of change that compiles everywhere and surprises a maintainer later.
//
// If no sink is installed the calls are no-ops rather than a crash — the core is
// linkable and unit-testable without a game (tests/ builds it with no extender).

namespace dvb::dlog
{
	enum class Level
	{
		trace,
		debug,
		info,
		warn,
		error,
		critical
	};

	/// Installed once by the platform (SkyrimHost / Fallout4Host) before the server
	/// starts. Not thread-safe against concurrent Write calls — set it at load time.
	using Sink = void (*)(Level, std::string_view);
	void SetSink(Sink a_sink);

	/// Emit one already-formatted line. Formatting happens in the templates below so
	/// this stays a plain function (no template instantiation across the sink seam).
	void Write(Level a_level, std::string_view a_message);

	namespace detail
	{
		template <class... Args>
		void Emit(Level a_level, std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			// A bad format argument must never take the game down for a log line.
			try {
				Write(a_level, std::format(a_fmt, std::forward<Args>(a_args)...));
			} catch (...) {
			}
		}
	}

	template <class... Args>
	void trace(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		detail::Emit(Level::trace, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void debug(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		detail::Emit(Level::debug, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		detail::Emit(Level::info, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		detail::Emit(Level::warn, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		detail::Emit(Level::error, a_fmt, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void critical(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		detail::Emit(Level::critical, a_fmt, std::forward<Args>(a_args)...);
	}
}
