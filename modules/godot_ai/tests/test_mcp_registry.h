/**************************************************************************/
/*  test_mcp_registry.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef TEST_MCP_REGISTRY_H
#define TEST_MCP_REGISTRY_H

#include "modules/godot_ai/mcp_schema.h"
#include "modules/godot_ai/mcp_tool_registry.h"

#include "tests/test_macros.h"

namespace TestMCPRegistry {

// Minimal tool used to exercise registry and validation behaviour without depending
// on the editor.
class EchoTool : public MCPTool {
	String tool_name;
	MCPCapability capability;

public:
	EchoTool(const String &p_name = "Test_Echo", MCPCapability p_capability = MCP_CAP_READ_PROJECT) :
			tool_name(p_name), capability(p_capability) {}

	virtual String get_tool_name() const override { return tool_name; }
	virtual String get_description() const override { return "Echoes its arguments."; }
	virtual MCPCapability get_capability() const override { return capability; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["text"] = MCPSchema::string_property("Text to echo.");
		properties["count"] = MCPSchema::integer_property("Repeat count.", 1);
		Vector<String> required;
		required.push_back("text");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["text"] = MCPSchema::string_property("Echoed text.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["text"] = p_arguments["text"];
		result["count"] = p_arguments["count"];
		return result;
	}
};

class FailingTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Test_Failing"; }
	virtual String get_description() const override { return "Always fails."; }
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		r_error.set(MCPToolError::FAILED, "this tool always fails");
		return Dictionary();
	}
};

// Registry tests share the process-wide singleton, so each cleans up after itself
// rather than assuming an empty registry.
static void unregister_if_present(const String &p_name) {
	if (MCPToolRegistry::get_singleton()->has_tool(p_name)) {
		MCPToolRegistry::get_singleton()->unregister_tool(p_name);
	}
}

TEST_CASE("[godot_ai] Tool registration and duplicate rejection") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	unregister_if_present("Test_Echo");

	const int initial_count = registry->get_tool_count();
	CHECK(registry->register_tool(Ref<MCPTool>(memnew(EchoTool))) == OK);
	CHECK(registry->has_tool("Test_Echo"));
	CHECK(registry->get_tool_count() == initial_count + 1);

	ERR_PRINT_OFF;
	// A second registration must not silently replace the first.
	CHECK(registry->register_tool(Ref<MCPTool>(memnew(EchoTool))) == ERR_ALREADY_EXISTS);
	CHECK(registry->register_tool(Ref<MCPTool>()) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
	CHECK(registry->get_tool_count() == initial_count + 1);

	CHECK(registry->unregister_tool("Test_Echo"));
	CHECK_FALSE(registry->unregister_tool("Test_Echo"));
	CHECK(registry->get_tool_count() == initial_count);
}

TEST_CASE("[godot_ai] Tool descriptors carry schema and capability") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	unregister_if_present("Test_Echo");
	registry->register_tool(Ref<MCPTool>(memnew(EchoTool)));

	const Dictionary descriptor = registry->get_tool_descriptor("Test_Echo");
	CHECK(String(descriptor["name"]) == "Test_Echo");
	CHECK(descriptor.has("inputSchema"));
	CHECK(descriptor.has("outputSchema"));

	const Dictionary meta = descriptor["_meta"];
	CHECK(String(meta["capability"]) == "read_project");
	CHECK_FALSE((bool)meta["mutating"]);

	// The advertised schema must be the one that gets enforced.
	const Dictionary schema = descriptor["inputSchema"];
	CHECK(String(schema["type"]) == "object");
	CHECK_FALSE((bool)schema["additionalProperties"]);

	registry->unregister_tool("Test_Echo");
}

TEST_CASE("[godot_ai] Tool listing is sorted and stable") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	unregister_if_present("Test_Zebra");
	unregister_if_present("Test_Alpha");
	registry->register_tool(Ref<MCPTool>(memnew(EchoTool("Test_Zebra"))));
	registry->register_tool(Ref<MCPTool>(memnew(EchoTool("Test_Alpha"))));

	const Array descriptors = registry->get_tool_descriptors();
	int alpha_index = -1;
	int zebra_index = -1;
	for (int i = 0; i < descriptors.size(); i++) {
		const String name = ((Dictionary)descriptors[i])["name"];
		if (name == "Test_Alpha") {
			alpha_index = i;
		} else if (name == "Test_Zebra") {
			zebra_index = i;
		}
	}
	CHECK(alpha_index >= 0);
	CHECK(zebra_index >= 0);
	CHECK(alpha_index < zebra_index);

	registry->unregister_tool("Test_Zebra");
	registry->unregister_tool("Test_Alpha");
}

TEST_CASE("[godot_ai] Argument validation happens before the tool runs") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	unregister_if_present("Test_Echo");
	registry->register_tool(Ref<MCPTool>(memnew(EchoTool)));

	SUBCASE("valid arguments are passed through with defaults applied") {
		Dictionary arguments;
		arguments["text"] = "hello";
		MCPToolError error;
		const Dictionary result = registry->call_tool("Test_Echo", arguments, error);
		CHECK_FALSE(error.has_error());
		CHECK(String(result["text"]) == "hello");
		CHECK((int)result["count"] == 1);
	}

	SUBCASE("a missing required argument is reported by name") {
		MCPToolError error;
		registry->call_tool("Test_Echo", Dictionary(), error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
		CHECK(error.message.contains("text"));
	}

	SUBCASE("a wrongly typed argument is rejected") {
		Dictionary arguments;
		arguments["text"] = 42;
		MCPToolError error;
		registry->call_tool("Test_Echo", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
		CHECK(error.message.contains("string"));
	}

	SUBCASE("an unknown argument is rejected rather than ignored") {
		Dictionary arguments;
		arguments["text"] = "hello";
		arguments["txet"] = "typo";
		MCPToolError error;
		registry->call_tool("Test_Echo", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
		CHECK(error.message.contains("txet"));
	}

	SUBCASE("an unknown tool is reported as not found") {
		MCPToolError error;
		registry->call_tool("Test_DoesNotExist", Dictionary(), error);
		CHECK(error.kind == MCPToolError::NOT_FOUND);
	}

	registry->unregister_tool("Test_Echo");
}

TEST_CASE("[godot_ai] Tool failures surface as tool errors") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	unregister_if_present("Test_Failing");
	registry->register_tool(Ref<MCPTool>(memnew(FailingTool)));

	MCPToolError error;
	registry->call_tool("Test_Failing", Dictionary(), error);
	CHECK(error.kind == MCPToolError::FAILED);
	CHECK(error.message == "this tool always fails");

	registry->unregister_tool("Test_Failing");
}

TEST_CASE("[godot_ai] Invocation summaries redact sensitive arguments") {
	EchoTool tool;
	Dictionary arguments;
	arguments["text"] = "visible";
	arguments["api_key"] = "super-secret-value";

	const String summary = tool.describe_invocation(arguments);
	CHECK(summary.contains("visible"));
	CHECK(summary.contains("<redacted>"));
	CHECK_FALSE(summary.contains("super-secret-value"));
}

TEST_CASE("[godot_ai] Script-registered tools are validated") {
	String error;

	SUBCASE("a descriptor without a handler is rejected") {
		Dictionary descriptor;
		descriptor["name"] = "Test_Scripted";
		CHECK(MCPCallableTool::from_descriptor(descriptor, error).is_null());
		CHECK(error.contains("handler"));
	}

	SUBCASE("a descriptor without a name is rejected") {
		Dictionary descriptor;
		CHECK(MCPCallableTool::from_descriptor(descriptor, error).is_null());
		CHECK(error.contains("name"));
	}

	SUBCASE("plugins cannot claim the dangerous_exec capability") {
		Dictionary descriptor;
		descriptor["name"] = "Test_Scripted";
		descriptor["capability"] = "dangerous_exec";
		descriptor["handler"] = Callable();
		CHECK(MCPCallableTool::from_descriptor(descriptor, error).is_null());
	}
}

} // namespace TestMCPRegistry

#endif // TEST_MCP_REGISTRY_H
