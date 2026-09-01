/**************************************************************************/
/*  test_mcp_docs.h                                                       */
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

#ifndef TEST_MCP_DOCS_H
#define TEST_MCP_DOCS_H

#include "modules/godot_ai/mcp_docs.h"

#include "tests/test_macros.h"

namespace TestMCPDocs {

TEST_CASE("[godot_ai] A method reads as the call the caller is about to write") {
	DocData::MethodDoc method;
	method.name = "move_and_slide";
	method.return_type = "bool";

	CHECK(MCPDocs::signature(method) == "move_and_slide() -> bool");

	DocData::ArgumentDoc path;
	path.name = "path";
	path.type = "NodePath";
	DocData::ArgumentDoc fallback;
	fallback.name = "fallback";
	fallback.type = "Node";
	fallback.default_value = "null";

	DocData::MethodDoc get_node;
	get_node.name = "get_node_or_null";
	get_node.return_type = "Node";
	get_node.qualifiers = "const";
	get_node.arguments.push_back(path);
	get_node.arguments.push_back(fallback);

	CHECK(MCPDocs::signature(get_node) ==
			"get_node_or_null(path: NodePath, fallback: Node = null) -> Node const");
}

TEST_CASE("[godot_ai] A void return is left off the signature") {
	DocData::MethodDoc method;
	method.name = "queue_free";
	method.return_type = "void";
	CHECK(MCPDocs::signature(method) == "queue_free()");

	// An empty return type is the same case, not a different one.
	DocData::MethodDoc unknown;
	unknown.name = "_ready";
	CHECK(MCPDocs::signature(unknown) == "_ready()");
}

TEST_CASE("[godot_ai] BBCode is stripped but the names inside it survive") {
	// Godot's docs are BBCode and nothing downstream renders it, so passing the
	// markup through would just be noise in the model's context. What the reader
	// needs is the names the tags wrap.
	CHECK(MCPDocs::strip_bbcode("Sets the [b]speed[/b] value.") == "Sets the speed value.");
	CHECK(MCPDocs::strip_bbcode("Returns a [Node2D].") == "Returns a Node2D.");
	CHECK(MCPDocs::strip_bbcode("Use [method move_and_slide].") == "Use move_and_slide.");
	CHECK(MCPDocs::strip_bbcode("If [param enabled] is true.") == "If enabled is true.");
	CHECK(MCPDocs::strip_bbcode("See [member velocity] and [constant MAX].") ==
			"See velocity and MAX.");
	CHECK(MCPDocs::strip_bbcode("Wrap in [code]quotes[/code].") == "Wrap in quotes.");
}

TEST_CASE("[godot_ai] An unmatched bracket is text, not markup") {
	// Real descriptions contain array syntax and stray brackets; eating the rest of
	// the string because one was unclosed would silently lose the description.
	CHECK(MCPDocs::strip_bbcode("An array like [1, 2, 3") == "An array like [1, 2, 3");
	CHECK(MCPDocs::strip_bbcode("Empty [] tag") == "Empty  tag");
}

TEST_CASE("[godot_ai] A brief is the first sentence, without the markup") {
	CHECK(MCPDocs::brief_of("Moves the body. Returns true on collision. More detail follows.") ==
			"Moves the body.");
	CHECK(MCPDocs::brief_of("A [b]single[/b] line with no full stop") ==
			"A single line with no full stop");
	CHECK(MCPDocs::brief_of("First line\nSecond line") == "First line");
	CHECK(MCPDocs::brief_of("   ").is_empty());

	// A decimal point is not a sentence end, because ". " is what is looked for.
	CHECK(MCPDocs::brief_of("Defaults to 0.5 for this property. Then more.") ==
			"Defaults to 0.5 for this property.");
}

TEST_CASE("[godot_ai] Matching is case-insensitive, and an empty needle matches") {
	CHECK(MCPDocs::matches("move_and_slide", "SLIDE"));
	CHECK(MCPDocs::matches("CharacterBody2D", "body"));
	CHECK_FALSE(MCPDocs::matches("move_and_slide", "rotate"));

	// This is what makes "list the members" and "search the members" one code path
	// rather than two.
	CHECK(MCPDocs::matches("anything", ""));
}

TEST_CASE("[godot_ai] Where a class comes from is said plainly") {
	CHECK(MCPDocs::describe_api_type("core") == "core");
	CHECK(MCPDocs::describe_api_type("editor") == "editor");
	CHECK(MCPDocs::describe_api_type("editor_extension") == "extension");
	CHECK(MCPDocs::describe_api_type("") == "unknown");
}

TEST_CASE("[godot_ai] A query naming nothing is refused before any lookup") {
	MCPDocs::Query query;
	Dictionary out;
	String error;
	CHECK(MCPDocs::lookup(query, out, error) == MCPDocs::RESULT_INVALID);
	CHECK(error.contains("search"));

	// A member belongs to something; asking for one without a class is a mistake
	// worth naming rather than a search across every class in the engine.
	MCPDocs::Query orphan;
	orphan.member = "move_and_slide";
	CHECK(MCPDocs::lookup(orphan, out, error) == MCPDocs::RESULT_INVALID);
	CHECK(error.contains("name the class"));
}

} // namespace TestMCPDocs

#endif // TEST_MCP_DOCS_H
