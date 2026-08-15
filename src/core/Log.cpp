#include "core/Log.h"

#include <atomic>

namespace dvb::dlog
{
	namespace
	{
		std::atomic<Sink> g_sink{ nullptr };
	}

	void SetSink(Sink a_sink)
	{
		g_sink.store(a_sink, std::memory_order_release);
	}

	void Write(Level a_level, std::string_view a_message)
	{
		if (const auto sink = g_sink.load(std::memory_order_acquire)) {
			sink(a_level, a_message);
		}
	}
}
