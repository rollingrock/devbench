#include "core/ToolExtensions.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <unordered_map>

namespace dvb::ToolExtensions
{
	namespace
	{
		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		// Per base tool: lowercased key → entry, and lowercased key → original display key.
		struct Bucket
		{
			std::unordered_map<std::string, Entry>       byKey;
			std::unordered_map<std::string, std::string> displayKey;
		};

		std::mutex                              g_mutex;
		std::unordered_map<std::string, Bucket> g_tools;  // lowercased baseTool → Bucket
		ChangeListener                          g_onChange;
	}

	void SetChangeListener(ChangeListener a_listener)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_onChange = std::move(a_listener);
	}

	bool Register(std::string a_baseTool, std::string a_key, json a_descriptor, ToolHandler a_handler)
	{
		const std::string original = a_baseTool;
		const std::string tool = Lower(a_baseTool);
		const std::string key = Lower(a_key);
		ChangeListener    onChange;
		bool              replaced;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			Bucket&                     b = g_tools[tool];
			replaced = b.byKey.find(key) != b.byKey.end();
			b.byKey[key] = Entry{ std::move(a_descriptor), std::move(a_handler) };
			b.displayKey[key] = std::move(a_key);
			onChange = g_onChange;
		}
		if (onChange)
			onChange(original);  // outside the lock — listener re-enters the registry
		return !replaced;
	}

	std::optional<Entry> Find(std::string_view a_baseTool, std::string_view a_key)
	{
		const std::string           tool = Lower(std::string(a_baseTool));
		const std::string           key = Lower(std::string(a_key));
		std::lock_guard<std::mutex> lock(g_mutex);
		const auto                  t = g_tools.find(tool);
		if (t == g_tools.end())
			return std::nullopt;
		const auto it = t->second.byKey.find(key);
		if (it == t->second.byKey.end())
			return std::nullopt;
		return it->second;
	}

	std::vector<std::string> Keys(std::string_view a_baseTool)
	{
		const std::string           tool = Lower(std::string(a_baseTool));
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<std::string>    out;
		const auto                  t = g_tools.find(tool);
		if (t != g_tools.end()) {
			out.reserve(t->second.displayKey.size());
			for (const auto& [key, name] : t->second.displayKey)
				out.push_back(name);
			std::sort(out.begin(), out.end());
		}
		return out;
	}

	void MergeExtensionArgs(std::string_view a_baseTool, json& a_inputSchema)
	{
		if (!a_inputSchema.is_object())
			return;
		const auto baseProps = a_inputSchema.find("properties");
		if (baseProps == a_inputSchema.end() || !baseProps->is_object())
			return;
		if (const auto open = a_inputSchema.find("additionalProperties"); open != a_inputSchema.end() && *open == true)
			return;

		std::map<std::string, json>                     extra;      // arg name -> the first declarer's schema
		std::map<std::string, std::vector<std::string>> declarers;  // arg name -> extension keys that read it
		bool                                            unknowable = false;
		for (const auto& key : Keys(a_baseTool)) {
			const auto entry = Find(a_baseTool, key);
			if (!entry)
				continue;
			const auto schema = entry->descriptor.find("inputSchema");
			const bool enumerated = schema != entry->descriptor.end() && schema->is_object() &&
			                        schema->contains("properties") && (*schema)["properties"].is_object() &&
			                        !(schema->contains("additionalProperties") && (*schema)["additionalProperties"] == true);
			if (!enumerated) {
				unknowable = true;  // still merge what the others declare: it documents them in tools/list
				continue;
			}
			for (const auto& [name, argSchema] : (*schema)["properties"].items()) {
				if (baseProps->contains(name))
					continue;  // the base tool's own key wins: it is what the base tool reads
				extra.try_emplace(name, argSchema.is_object() ? argSchema : json::object());
				declarers[name].push_back(key);
			}
		}

		for (auto& [name, argSchema] : extra) {
			const auto& keys = declarers[name];
			std::string tag = keys.size() == 1 ? "[extension " : "[extensions ";
			for (std::size_t i = 0; i < keys.size(); ++i)
				tag += (i ? ", " : "") + keys[i];
			tag += "]";
			const auto desc = argSchema.find("description");
			argSchema["description"] = (desc != argSchema.end() && desc->is_string() && !desc->get<std::string>().empty()) ?
			                               tag + " " + desc->get<std::string>() :
			                               tag;
			(*baseProps)[name] = std::move(argSchema);
		}
		if (unknowable)
			a_inputSchema["additionalProperties"] = true;
	}
}
