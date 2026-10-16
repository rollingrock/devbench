// Host-independent coverage for ToolExtensions::MergeExtensionArgs: a base tool that routes to
// mod-registered extensions (inspect kind=<key>, menu invoke name=<key>, capture providers) must
// accept the arguments those extensions read, or the registry's unknown-key check reports
// correct arguments as mistakes.
//
// ToolExtensions is process-global with no unregister, so every case uses its own base-tool name.

#include "test_framework.h"

#include "core/ToolExtensions.h"
#include "core/ToolRegistry.h"

using dvb::json;
using dvb::ToolContext;
using dvb::ToolDescriptor;
using dvb::ToolRegistry;
using dvb::ToolResult;
namespace ToolExtensions = dvb::ToolExtensions;

namespace
{
	json BaseSchema()
	{
		return json{ { "type", "object" }, { "properties", { { "kind", json{ { "type", "string" } } } } } };
	}

	json EmptyHandler(const json&, const ToolContext&) { return json::object(); }

	void Extend(const char* a_base, const char* a_key, json a_descriptor)
	{
		ToolExtensions::Register(a_base, a_key, std::move(a_descriptor), &EmptyHandler);
	}

	ToolResult InvokeWith(const json& a_schema, const json& a_args, bool a_strict = false)
	{
		ToolRegistry   reg;
		ToolDescriptor d;
		d.name = "base";
		d.inputSchema = a_schema;
		reg.Register(std::move(d), &EmptyHandler);
		reg.SetStrictArgs(a_strict);
		return reg.Invoke("base", a_args, ToolContext{});
	}
}

TEST_CASE("with no extensions registered the base schema is unchanged")
{
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-none", schema);
	CHECK(schema == BaseSchema());
}

TEST_CASE("an extension's declared arguments join the base schema, tagged with its key")
{
	Extend("mx-declared", "frik", json{ { "description", "body state" }, { "inputSchema", { { "type", "object" }, { "properties", { { "hand", json{ { "type", "string" }, { "description", "left|right" } } } } } } } });
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-declared", schema);

	CHECK(schema["properties"].contains("hand"));
	CHECK(schema["properties"]["hand"]["type"] == "string");
	CHECK(schema["properties"]["hand"]["description"] == "[extension frik] left|right");
	CHECK(!schema.contains("additionalProperties"));  // every extension declared: keep checking
}

TEST_CASE("a base tool's own key is never replaced by an extension's")
{
	Extend("mx-collide", "scope", json{ { "inputSchema", { { "type", "object" }, { "properties", { { "kind", json{ { "type", "integer" } } } } } } } });
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-collide", schema);
	const json baseKind = BaseSchema()["properties"]["kind"];
	CHECK(schema["properties"]["kind"] == baseKind);
}

TEST_CASE("an argument several extensions read names all of them")
{
	const json desc{ { "inputSchema", { { "type", "object" }, { "properties", { { "since", json::object() } } } } } };
	Extend("mx-shared", "b", desc);
	Extend("mx-shared", "a", desc);
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-shared", schema);
	CHECK(schema["properties"]["since"]["description"] == "[extensions a, b]");
}

TEST_CASE("an extension's declared argument no longer draws an unknown-key warning")
{
	Extend("mx-e2e", "frik", json{ { "inputSchema", { { "type", "object" }, { "properties", { { "hand", json::object() } } } } } });
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-e2e", schema);

	const ToolResult ok = InvokeWith(schema, json{ { "kind", "frik" }, { "hand", "left" } });
	CHECK(ok.ok);
	CHECK(!ok.value.contains("warnings"));

	// Still a checked schema: a key nobody declares is still reported.
	const ToolResult typo = InvokeWith(schema, json{ { "kind", "frik" }, { "hnad", "left" } });
	CHECK(typo.ok);
	CHECK(typo.value.contains("warnings"));
}

TEST_CASE("an extension that declares no arguments turns the base tool's check off, not into false reports")
{
	Extend("mx-opaque", "declared", json{ { "inputSchema", { { "type", "object" }, { "properties", { { "hand", json::object() } } } } } });
	Extend("mx-opaque", "opaque", json{ { "description", "no inputSchema" } });
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-opaque", schema);

	CHECK(schema["additionalProperties"] == true);
	CHECK(schema["properties"].contains("hand"));  // what IS declared is still documented

	// The opaque extension's argument is neither warned about nor refused in strict mode.
	const ToolResult r = InvokeWith(schema, json{ { "kind", "opaque" }, { "whatever", 1 } }, true);
	CHECK(r.ok);
	CHECK(!r.value.contains("warnings"));
}

TEST_CASE("an extension schema that is itself open counts as undeclared")
{
	Extend("mx-open", "x", json{ { "inputSchema", { { "type", "object" }, { "properties", json::object() }, { "additionalProperties", true } } } });
	json schema = BaseSchema();
	ToolExtensions::MergeExtensionArgs("mx-open", schema);
	CHECK(schema["additionalProperties"] == true);
}

TEST_CASE("a base schema that enumerates no keys is left alone")
{
	Extend("mx-schemaless", "x", json{ { "description", "no inputSchema" } });
	const json original = json{ { "type", "object" } };
	json       schema = original;
	ToolExtensions::MergeExtensionArgs("mx-schemaless", schema);
	CHECK(schema == original);
}
