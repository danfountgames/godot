/**************************************************************************/
/*  test_mcp_runtime_paths.h                                              */
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

#ifndef TEST_MCP_RUNTIME_PATHS_H
#define TEST_MCP_RUNTIME_PATHS_H

#include "modules/godot_ai/mcp_runtime_paths.h"

#include "tests/test_macros.h"

namespace TestMCPRuntimePaths {

TEST_CASE("[godot_ai] A running node's path becomes the edited scene's path for it") {
	String path;
	String error;

	REQUIRE(MCPRuntimePaths::to_scene_path("/root/Main/Player", "Main", path, error));
	CHECK(path == "Player");

	REQUIRE(MCPRuntimePaths::to_scene_path("/root/Main/Hud/Field", "Main", path, error));
	CHECK(path == "Hud/Field");

	// The scene root itself, which the editor addresses as ".".
	REQUIRE(MCPRuntimePaths::to_scene_path("/root/Main", "Main", path, error));
	CHECK(path == ".");
}

TEST_CASE("[godot_ai] Promoting out of a scene the editor does not have open is refused") {
	String path;
	String error;

	// The running game is playing Level2 and the editor has Main open. Stripping two
	// components and writing anyway would put a value from one scene into another, and
	// the result would look entirely plausible.
	CHECK_FALSE(MCPRuntimePaths::to_scene_path("/root/Level2/Player", "Main", path, error));
	CHECK(error.contains("Level2"));
	CHECK(error.contains("Main"));
	CHECK(path.is_empty());
}

TEST_CASE("[godot_ai] A path that is not a running-game path is refused, and says so") {
	String path;
	String error;

	// An editor path handed to the wrong side.
	CHECK_FALSE(MCPRuntimePaths::to_scene_path("Player", "Main", path, error));
	CHECK(error.contains("/root"));

	// The game's viewport is not a node in any scene.
	CHECK_FALSE(MCPRuntimePaths::to_scene_path("/root", "Main", path, error));
	CHECK(error.contains("viewport"));

	CHECK_FALSE(MCPRuntimePaths::to_scene_path("", "Main", path, error));
	CHECK_FALSE(MCPRuntimePaths::to_scene_path("/root/Main/Player", "", path, error));
}

TEST_CASE("[godot_ai] Extra and trailing separators do not change which node is meant") {
	String path;
	String error;

	REQUIRE(MCPRuntimePaths::to_scene_path("/root/Main/Hud//Field/", "Main", path, error));
	CHECK(path == "Hud/Field");

	REQUIRE(MCPRuntimePaths::to_scene_path("  /root/Main/Player  ", "Main", path, error));
	CHECK(path == "Player");
}

TEST_CASE("[godot_ai] The translation runs backwards for an editor selection") {
	String path;
	String error;

	REQUIRE(MCPRuntimePaths::to_runtime_path("Player", "Main", path, error));
	CHECK(path == "/root/Main/Player");

	REQUIRE(MCPRuntimePaths::to_runtime_path(".", "Main", path, error));
	CHECK(path == "/root/Main");

	REQUIRE(MCPRuntimePaths::to_runtime_path("Hud/Field", "Main", path, error));
	CHECK(path == "/root/Main/Hud/Field");

	// A runtime path handed to the wrong side is refused rather than doubled up into
	// /root/Main/root/Main/Player.
	CHECK_FALSE(MCPRuntimePaths::to_runtime_path("/root/Main/Player", "Main", path, error));
	CHECK(error.contains("already"));
}

TEST_CASE("[godot_ai] Both directions agree with each other") {
	for (const char *scene_path : { ".", "Player", "Hud/Field", "A/B/C" }) {
		String runtime;
		String back;
		String error;
		REQUIRE(MCPRuntimePaths::to_runtime_path(scene_path, "Main", runtime, error));
		REQUIRE(MCPRuntimePaths::to_scene_path(runtime, "Main", back, error));
		CHECK(back == String(scene_path));
	}
}

TEST_CASE("[godot_ai] The scene a runtime path is rooted in can be read off it") {
	CHECK(MCPRuntimePaths::scene_name_of("/root/Main/Player") == "Main");
	CHECK(MCPRuntimePaths::scene_name_of("/root/Level2") == "Level2");
	CHECK(MCPRuntimePaths::scene_name_of("/root").is_empty());
	CHECK(MCPRuntimePaths::scene_name_of("Player").is_empty());
}

TEST_CASE("[godot_ai] Godot's text form round-trips the types JSON cannot carry") {
	// This is why a promoted value is read from the text and not from the JSON: a
	// position promoted through JSON would arrive as an array and stop being a Vector2.
	CHECK(Variant(mcp_variant_from_text("Vector2(3, 4)")) == Variant(Vector2(3, 4)));
	CHECK(Variant(mcp_variant_from_text("Color(1, 0, 0, 1)")) == Variant(Color(1, 0, 0, 1)));
	CHECK(Variant(mcp_variant_from_text("Vector3(1, 2, 3)")) == Variant(Vector3(1, 2, 3)));
	CHECK(Variant(mcp_variant_from_text("42")) == Variant(42));
	CHECK(Variant(mcp_variant_from_text("3.5")) == Variant(3.5));
	CHECK(Variant(mcp_variant_from_text("true")) == Variant(true));
	CHECK(Variant(mcp_variant_from_text("\"hello\"")) == Variant("hello"));
}

TEST_CASE("[godot_ai] Text that does not parse yields nothing rather than an error") {
	// The caller has the JSON value to fall back on; a refusal about spelling would be
	// worse than a value that arrived.
	CHECK(mcp_variant_from_text("").get_type() == Variant::NIL);
	CHECK(mcp_variant_from_text("   ").get_type() == Variant::NIL);
	CHECK(mcp_variant_from_text("Vector2(").get_type() == Variant::NIL);
}

} // namespace TestMCPRuntimePaths

#endif // TEST_MCP_RUNTIME_PATHS_H
