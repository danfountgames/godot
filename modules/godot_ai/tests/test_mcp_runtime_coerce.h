/**************************************************************************/
/*  test_mcp_runtime_coerce.h                                             */
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

#ifndef TEST_MCP_RUNTIME_COERCE_H
#define TEST_MCP_RUNTIME_COERCE_H

#include "modules/godot_ai/mcp_runtime_agent.h"

#include "core/variant/variant_parser.h"

#include "tests/test_macros.h"

// What may be written into a running game's property.
//
// `Object::set` does not convert and refuses quietly, so everything a caller sends has to
// be turned into the property's real type first. That makes this function the whole of the
// contract for "what counts as a value", and it is the place a silent no-op comes from.
namespace TestMCPRuntimeCoerce {

// The invariant the round trip rests on: whatever `get_property` prints, `set_property`
// must accept. Writing the value out with the same writer the runtime uses is what makes
// this a test of the pair rather than of one half.
static void check_round_trip(const Variant &p_value) {
	String text;
	VariantWriter::write_to_string(p_value, text);

	Variant out;
	String error;
	REQUIRE_MESSAGE(MCPRuntimeAgent::coerce(text, p_value.get_type(), out, error),
			vformat("%s did not survive its own printed form '%s': %s",
					Variant::get_type_name(p_value.get_type()), text, error));
	CHECK_MESSAGE(out == p_value, vformat("'%s' came back as something else", text));
}

TEST_CASE("[godot_ai] Anything the runtime prints, the runtime accepts back") {
	// Not a tidiness argument. Without this the two halves of the interface disagree: a
	// caller can read a position out and cannot write the same string back in, so anything
	// that round-trips a value - the tuning workspace putting the original back, most
	// obviously - silently fails on every structured property while reporting success.
	check_round_trip(Vector2(128, 64));
	check_round_trip(Vector2i(3, -4));
	check_round_trip(Vector3(1.5, 2.5, -3.5));
	check_round_trip(Color(0.25, 0.5, 0.75, 1.0));
	check_round_trip(Rect2(0, 0, 320, 240));
	check_round_trip(true);
	check_round_trip(42);
	check_round_trip(3.5);
}

TEST_CASE("[godot_ai] A JSON array still builds the structured type it describes") {
	// The other half of the same problem, and the one that was already handled: JSON has
	// no Vector2, so a client that has only JSON sends [64, 32].
	Array pair;
	pair.push_back(64);
	pair.push_back(32);

	Variant out;
	String error;
	REQUIRE(MCPRuntimeAgent::coerce(pair, Variant::VECTOR2, out, error));
	CHECK(Vector2(out) == Vector2(64, 32));

	// And an array that cannot make one says so rather than producing a default.
	Array too_many;
	for (int i = 0; i < 5; i++) {
		too_many.push_back(i);
	}
	CHECK_FALSE(MCPRuntimeAgent::coerce(too_many, Variant::VECTOR2, out, error));
	CHECK(error.contains("Vector2"));
}

TEST_CASE("[godot_ai] A string destined for a string property is left alone") {
	// The parse is a fallback for structured types, not a general reinterpretation. A
	// label whose text happens to read "(1, 2)" must stay that text.
	Variant out;
	String error;
	REQUIRE(MCPRuntimeAgent::coerce(String("(1, 2)"), Variant::STRING, out, error));
	CHECK(String(out) == "(1, 2)");

	REQUIRE(MCPRuntimeAgent::coerce(String("Main/Player"), Variant::NODE_PATH, out, error));
	CHECK(out.get_type() == Variant::NODE_PATH);
	CHECK(String(out) == "Main/Player");
}

TEST_CASE("[godot_ai] A string that parses as the wrong type is not accepted as the right one") {
	Variant out;
	String error;
	// Parses perfectly well - as a Vector2, which is not what was asked for. The text
	// fallback declines, and nothing else offers to turn a string into a Vector3, so this
	// is refused rather than becoming a Vector3 of whatever fell out.
	CHECK_FALSE(MCPRuntimeAgent::coerce(String("Vector2(1, 2)"), Variant::VECTOR3, out, error));
	CHECK_FALSE(error.is_empty());
}

TEST_CASE("[godot_ai] Godot's own string conversions still apply, including the loose one") {
	Variant out;
	String error;
	// A colour by name is a real and useful thing to write, and Godot converts it - so the
	// fallback must not get in the way of it.
	REQUIRE(MCPRuntimeAgent::coerce(String("red"), Variant::COLOR, out, error));
	CHECK(Color(out) == Color(1, 0, 0, 1));

	// The uncomfortable half of the same rule, recorded rather than hidden: Godot's
	// String-to-Color conversion accepts anything and answers black, so a typo in a colour
	// name sets black and reports success. That is the engine's behaviour and not this
	// module's to override - a tool that second-guessed it would refuse valid colours -
	// but a reader of this file should not have to discover it in a game.
	REQUIRE(MCPRuntimeAgent::coerce(String("nonsense"), Variant::COLOR, out, error));
	CHECK(Color(out) == Color(0, 0, 0, 1));
}

TEST_CASE("[godot_ai] Nonsense is refused with a message naming both types") {
	Variant out;
	String error;
	CHECK_FALSE(MCPRuntimeAgent::coerce(String("not a vector at all"), Variant::VECTOR2, out, error));
	CHECK(error.contains("Vector2"));
}

TEST_CASE("[godot_ai] A value already of the right type passes straight through") {
	Variant out;
	String error;
	REQUIRE(MCPRuntimeAgent::coerce(Vector2(7, 8), Variant::VECTOR2, out, error));
	CHECK(Vector2(out) == Vector2(7, 8));
}

} // namespace TestMCPRuntimeCoerce

#endif // TEST_MCP_RUNTIME_COERCE_H
