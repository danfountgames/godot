/**************************************************************************/
/*  test_mcp_protocol.h                                                   */
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

#ifndef TEST_MCP_PROTOCOL_H
#define TEST_MCP_PROTOCOL_H

#include "modules/godot_ai/mcp_protocol.h"
#include "modules/godot_ai/mcp_tool_registry.h"

#include "tests/test_macros.h"

namespace TestMCPProtocol {

// A delegate that records what it was asked and answers deterministically, so the
// protocol can be tested without an editor, a socket, or a user.
class TestDelegate : public MCPProtocol::Delegate {
public:
	bool allow_client = true;
	bool allow_prompt = false;
	String deny_reason = "denied by test";

	int approve_calls = 0;
	int prompt_calls = 0;
	Vector<String> audit_tools;
	Vector<bool> audit_allowed;

	virtual bool approve_client(MCPSession &p_session, String &r_reason) override {
		approve_calls++;
		if (!allow_client) {
			r_reason = deny_reason;
			return false;
		}
		return true;
	}
	virtual bool prompt_for_tool(const MCPSession &p_session, const Ref<MCPTool> &p_tool, const Dictionary &p_arguments, String &r_reason) override {
		prompt_calls++;
		if (!allow_prompt) {
			r_reason = "the user declined";
			return false;
		}
		return true;
	}
	virtual void record_invocation(const MCPSession &p_session, const String &p_tool_name, const String &p_summary, bool p_allowed, const String &p_reason) override {
		audit_tools.push_back(p_tool_name);
		audit_allowed.push_back(p_allowed);
	}
	virtual String get_project_path() const override { return "/tmp/test-project"; }
	virtual String get_project_name() const override { return "Test Project"; }
	virtual String get_editor_version() const override { return "4.3.test"; }
};

class ProbeTool : public MCPTool {
	String tool_name;
	MCPCapability capability;

public:
	ProbeTool(const String &p_name, MCPCapability p_capability) :
			tool_name(p_name), capability(p_capability) {}

	virtual String get_tool_name() const override { return tool_name; }
	virtual String get_description() const override { return "Probe."; }
	virtual MCPCapability get_capability() const override { return capability; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["value"] = MCPSchema::string_property("A value.", "default");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["value"] = MCPSchema::string_property("Echoed value.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["value"] = p_arguments["value"];
		return result;
	}
};

static Dictionary request(const String &p_method, const Variant &p_id, const Dictionary &p_params = Dictionary()) {
	Dictionary message;
	message["jsonrpc"] = "2.0";
	message["method"] = p_method;
	if (p_id.get_type() != Variant::NIL) {
		message["id"] = p_id;
	}
	if (!p_params.is_empty()) {
		message["params"] = p_params;
	}
	return message;
}

static Dictionary hello_params() {
	Dictionary params;
	Array versions;
	versions.push_back(MCPProtocol::BRIDGE_VERSION);
	params["bridge_versions"] = versions;
	params["client_name"] = "test-client";
	return params;
}

// Drives a session through handshake + initialize, the state every other method
// requires.
static void open_session(MCPSession &r_session, TestDelegate &p_delegate) {
	Dictionary response;
	MCPProtocol::handle_message(request("godot/hello", 1, hello_params()), r_session, &p_delegate, response);
	Dictionary initialize_params;
	initialize_params["protocolVersion"] = MCPProtocol::PROTOCOL_VERSION;
	MCPProtocol::handle_message(request("initialize", 2, initialize_params), r_session, &p_delegate, response);
	MCPProtocol::handle_message(request("notifications/initialized", Variant()), r_session, &p_delegate, response);
}

TEST_CASE("[godot_ai] Bridge handshake accepts a matching relay") {
	MCPSession session;
	TestDelegate delegate;
	Dictionary response;

	REQUIRE(MCPProtocol::handle_message(request("godot/hello", 1, hello_params()), session, &delegate, response));
	CHECK(response.has("result"));
	const Dictionary result = response["result"];
	CHECK(String(result["bridge_version"]) == MCPProtocol::BRIDGE_VERSION);
	CHECK(String(result["project_path"]) == "/tmp/test-project");
	CHECK(session.client_approved);
	CHECK(session.client_name == "test-client");
	CHECK(delegate.approve_calls == 1);
}

TEST_CASE("[godot_ai] Bridge handshake rejects an incompatible relay") {
	MCPSession session;
	TestDelegate delegate;
	Dictionary response;

	Dictionary params;
	Array versions;
	versions.push_back("999");
	params["bridge_versions"] = versions;

	REQUIRE(MCPProtocol::handle_message(request("godot/hello", 1, params), session, &delegate, response));
	const Dictionary error = response["error"];
	CHECK((int)error["code"] == MCPProtocol::ERROR_BRIDGE_VERSION);
	CHECK_FALSE(session.client_approved);
	// A version mismatch is caught before the client is even considered.
	CHECK(delegate.approve_calls == 0);
}

TEST_CASE("[godot_ai] An unapproved client cannot reach any tool method") {
	MCPSession session;
	TestDelegate delegate;
	delegate.allow_client = false;
	Dictionary response;

	MCPProtocol::handle_message(request("godot/hello", 1, hello_params()), session, &delegate, response);
	CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_CLIENT_NOT_APPROVED);

	for (const String &method : { String("initialize"), String("tools/list"), String("tools/call"), String("ping") }) {
		Dictionary blocked;
		REQUIRE(MCPProtocol::handle_message(request(method, 5), session, &delegate, blocked));
		CHECK((int)((Dictionary)blocked["error"])["code"] == MCPProtocol::ERROR_CLIENT_NOT_APPROVED);
	}
}

TEST_CASE("[godot_ai] initialize negotiates a protocol version") {
	MCPSession session;
	TestDelegate delegate;
	Dictionary response;
	MCPProtocol::handle_message(request("godot/hello", 1, hello_params()), session, &delegate, response);

	SUBCASE("a supported version is echoed back") {
		Dictionary params;
		params["protocolVersion"] = "2024-11-05";
		REQUIRE(MCPProtocol::handle_message(request("initialize", 2, params), session, &delegate, response));
		const Dictionary result = response["result"];
		CHECK(String(result["protocolVersion"]) == "2024-11-05");
		CHECK_FALSE(result.has("_meta"));
	}

	SUBCASE("an unsupported version falls back to one the server speaks") {
		Dictionary params;
		params["protocolVersion"] = "1999-01-01";
		REQUIRE(MCPProtocol::handle_message(request("initialize", 2, params), session, &delegate, response));
		const Dictionary result = response["result"];
		CHECK(String(result["protocolVersion"]) == MCPProtocol::PROTOCOL_VERSION);
		// The client is told its request was not honoured.
		const Dictionary meta = result["_meta"];
		CHECK(String(meta["requestedProtocolVersion"]) == "1999-01-01");
	}

	SUBCASE("capabilities advertise tool list-change notifications") {
		REQUIRE(MCPProtocol::handle_message(request("initialize", 2), session, &delegate, response));
		const Dictionary result = response["result"];
		const Dictionary capabilities = result["capabilities"];
		const Dictionary tools = capabilities["tools"];
		CHECK((bool)tools["listChanged"]);
		CHECK(String(((Dictionary)result["serverInfo"])["name"]) == MCPProtocol::SERVER_NAME);
	}
}

TEST_CASE("[godot_ai] Methods before initialize are refused") {
	MCPSession session;
	TestDelegate delegate;
	Dictionary response;
	MCPProtocol::handle_message(request("godot/hello", 1, hello_params()), session, &delegate, response);

	REQUIRE(MCPProtocol::handle_message(request("tools/list", 2), session, &delegate, response));
	CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_NOT_INITIALIZED);

	// ping is deliberately available before initialize, for liveness checks.
	REQUIRE(MCPProtocol::handle_message(request("ping", 3), session, &delegate, response));
	CHECK(response.has("result"));
}

TEST_CASE("[godot_ai] tools/list returns registered tools") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	registry->unregister_tool("Test_Probe");
	registry->register_tool(Ref<MCPTool>(memnew(ProbeTool("Test_Probe", MCP_CAP_READ_PROJECT))));

	MCPSession session;
	TestDelegate delegate;
	open_session(session, delegate);

	Dictionary response;
	REQUIRE(MCPProtocol::handle_message(request("tools/list", 10), session, &delegate, response));
	const Array tools = ((Dictionary)response["result"])["tools"];

	bool found = false;
	for (int i = 0; i < tools.size(); i++) {
		const Dictionary tool = tools[i];
		if (String(tool["name"]) == "Test_Probe") {
			found = true;
			CHECK(tool.has("inputSchema"));
			CHECK(tool.has("description"));
		}
	}
	CHECK(found);

	registry->unregister_tool("Test_Probe");
}

TEST_CASE("[godot_ai] tools/call executes and returns structured content") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	registry->unregister_tool("Test_Probe");
	registry->register_tool(Ref<MCPTool>(memnew(ProbeTool("Test_Probe", MCP_CAP_READ_PROJECT))));

	MCPSession session;
	TestDelegate delegate;
	open_session(session, delegate);

	Dictionary arguments;
	arguments["value"] = "hello";
	Dictionary params;
	params["name"] = "Test_Probe";
	params["arguments"] = arguments;

	Dictionary response;
	REQUIRE(MCPProtocol::handle_message(request("tools/call", 11, params), session, &delegate, response));
	const Dictionary result = response["result"];
	CHECK_FALSE((bool)result["isError"]);

	const Array content = result["content"];
	CHECK(content.size() == 1);
	CHECK(String(((Dictionary)content[0])["type"]) == "text");

	const Dictionary structured = result["structuredContent"];
	CHECK(String(structured["value"]) == "hello");

	// Every invocation is auditable, including the successful ones.
	CHECK(delegate.audit_tools.size() == 1);
	CHECK(delegate.audit_allowed[0]);

	registry->unregister_tool("Test_Probe");
}

TEST_CASE("[godot_ai] tools/call error behaviour") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	registry->unregister_tool("Test_Probe");
	registry->register_tool(Ref<MCPTool>(memnew(ProbeTool("Test_Probe", MCP_CAP_READ_PROJECT))));

	MCPSession session;
	TestDelegate delegate;
	open_session(session, delegate);
	Dictionary response;

	SUBCASE("an unknown tool is a protocol error") {
		Dictionary params;
		params["name"] = "Test_NoSuchTool";
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 12, params), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_METHOD_NOT_FOUND);
	}

	SUBCASE("a missing name is an invalid params error") {
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 13), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_INVALID_PARAMS);
	}

	SUBCASE("non-object arguments are rejected") {
		Dictionary params;
		params["name"] = "Test_Probe";
		params["arguments"] = "not an object";
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 14, params), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_INVALID_PARAMS);
	}

	SUBCASE("schema violations are invalid params, not tool results") {
		Dictionary arguments;
		arguments["unexpected"] = 1;
		Dictionary params;
		params["name"] = "Test_Probe";
		params["arguments"] = arguments;
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 15, params), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_INVALID_PARAMS);
	}

	registry->unregister_tool("Test_Probe");
}

TEST_CASE("[godot_ai] Permission decisions gate tools/call") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	registry->unregister_tool("Test_Mutator");
	registry->register_tool(Ref<MCPTool>(memnew(ProbeTool("Test_Mutator", MCP_CAP_EDIT_SCENE))));
	MCPPermissions::clear_policy_overrides();

	Dictionary params;
	params["name"] = "Test_Mutator";

	SUBCASE("a prompt the user declines denies the call") {
		MCPPermissions::set_policy_override(MCP_CAP_EDIT_SCENE, MCP_POLICY_ASK);
		MCPSession session;
		TestDelegate delegate;
		delegate.allow_prompt = false;
		open_session(session, delegate);

		Dictionary response;
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 16, params), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_PERMISSION_DENIED);
		CHECK(delegate.prompt_calls == 1);
		// A refusal is recorded too, so the audit trail is complete.
		CHECK(delegate.audit_tools.size() == 1);
		CHECK_FALSE(delegate.audit_allowed[0]);
	}

	SUBCASE("a prompt the user accepts runs the tool") {
		MCPPermissions::set_policy_override(MCP_CAP_EDIT_SCENE, MCP_POLICY_ASK);
		MCPSession session;
		TestDelegate delegate;
		delegate.allow_prompt = true;
		open_session(session, delegate);

		Dictionary response;
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 17, params), session, &delegate, response));
		CHECK(response.has("result"));
		CHECK_FALSE((bool)((Dictionary)response["result"])["isError"]);
	}

	SUBCASE("a read-only session refuses mutation without prompting") {
		MCPPermissions::set_policy_override(MCP_CAP_EDIT_SCENE, MCP_POLICY_ALLOW);
		MCPSession session;
		TestDelegate delegate;
		delegate.allow_prompt = true;
		open_session(session, delegate);
		session.read_only = true;

		Dictionary response;
		REQUIRE(MCPProtocol::handle_message(request("tools/call", 18, params), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_PERMISSION_DENIED);
		CHECK(delegate.prompt_calls == 0);
	}

	MCPPermissions::clear_policy_overrides();
	registry->unregister_tool("Test_Mutator");
}

TEST_CASE("[godot_ai] Unknown methods and malformed messages") {
	MCPSession session;
	TestDelegate delegate;
	open_session(session, delegate);
	Dictionary response;

	SUBCASE("an unknown method is reported as method not found") {
		REQUIRE(MCPProtocol::handle_message(request("resources/list", 20), session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_METHOD_NOT_FOUND);
	}

	SUBCASE("a message without a method is an invalid request") {
		Dictionary message;
		message["jsonrpc"] = "2.0";
		message["id"] = 21;
		REQUIRE(MCPProtocol::handle_message(message, session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_INVALID_REQUEST);
	}

	SUBCASE("a wrong jsonrpc version is an invalid request") {
		Dictionary message = request("tools/list", 22);
		message["jsonrpc"] = "1.0";
		REQUIRE(MCPProtocol::handle_message(message, session, &delegate, response));
		CHECK((int)((Dictionary)response["error"])["code"] == MCPProtocol::ERROR_INVALID_REQUEST);
	}

	SUBCASE("notifications never produce a response") {
		CHECK_FALSE(MCPProtocol::handle_message(request("notifications/cancelled", Variant()), session, &delegate, response));
		CHECK_FALSE(MCPProtocol::handle_message(request("resources/list", Variant()), session, &delegate, response));
	}
}

} // namespace TestMCPProtocol

#endif // TEST_MCP_PROTOCOL_H
