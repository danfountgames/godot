/**************************************************************************/
/*  mcp_scene_test_tools.cpp                                              */
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

// Running the project's own tests.
//
// A test here is a *scene*, not a shell command. That is not a limitation worked
// around: no tool in this module may execute an arbitrary command, and a test runner is
// exactly the place that rule would be quietly broken. A scene runs under the engine
// the game runs under, through the same play path a developer uses, and reports back
// over the channel that already exists.
//
// The contract a test scene declares, on its root node, as either script properties or
// node metadata (metadata too, because a scene with no script can carry nothing else):
//
//   test_finished : bool   - set true when the scene has run every case
//   test_results  : Array  - one Dictionary per case:
//                            { name, passed, message, duration_ms }
//
// Per-case results rather than a count, because "3 failed" is not something anyone can
// act on. A named case with a message is.

#include "mcp_builtin_tools.h"

#include "../mcp_deferred.h"
#include "../mcp_paths.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_tool_registry.h"

#include "core/io/dir_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/variant/array.h"

#include "editor/editor_interface.h"
#include "editor/editor_node.h"

namespace {

const char *TEST_SCENE_PREFIX = "test_";

void find_test_scenes(const String &p_directory, const String &p_prefix, Array &r_scenes) {
	Ref<DirAccess> access = DirAccess::open(p_directory);
	if (access.is_null()) {
		return;
	}
	access->list_dir_begin();
	for (String entry = access->get_next(); !entry.is_empty(); entry = access->get_next()) {
		if (entry.begins_with(".")) {
			continue;
		}
		const String full = p_directory.path_join(entry);
		if (access->current_is_dir()) {
			find_test_scenes(full, p_prefix, r_scenes);
			continue;
		}
		if (entry.get_extension().to_lower() != "tscn" || !entry.begins_with(p_prefix)) {
			continue;
		}
		Dictionary scene;
		scene["path"] = full;
		scene["name"] = entry.get_basename();
		scene["directory"] = p_directory;
		r_scenes.push_back(scene);
	}
	access->list_dir_end();
}

class ListSceneTestsTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListSceneTests"; }
	virtual String get_description() const override {
		return "List the project's test scenes - scenes whose file name begins with 'test_'. "
			   "A test here is a scene the engine plays, not a shell command, because nothing "
			   "in this interface executes arbitrary commands. What cases a scene contains is "
			   "only known once it has run; this says which scenes there are to run.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["directory"] = MCPSchema::string_property(
				"Where to look, as a res:// path. Searched recursively.", "res://");
		properties["prefix"] = MCPSchema::string_property(
				"File name prefix that marks a scene as a test.", TEST_SCENE_PREFIX);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["scenes"] = MCPSchema::array_property("Test scenes found.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["count"] = MCPSchema::integer_property("How many were found.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String requested = String(p_arguments.get("directory", "res://")).strip_edges();
		const String prefix = String(p_arguments.get("prefix", TEST_SCENE_PREFIX)).strip_edges();
		if (prefix.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					"a prefix of nothing would call every scene in the project a test");
			return Dictionary();
		}

		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_existing(requested.is_empty() ? "res://" : requested, resolved, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}
		if (!resolved.is_directory) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' is a file, not a directory to search", resolved.res_path));
			return Dictionary();
		}

		Array scenes;
		find_test_scenes(resolved.res_path, prefix, scenes);

		Dictionary result;
		result["scenes"] = scenes;
		result["count"] = scenes.size();
		return result;
	}
};

class RunSceneTestTool : public MCPTool {
	// One run at a time. Two test scenes cannot play at once - there is one play
	// session - so a second request is refused rather than silently joining the first.
	enum Stage {
		IDLE,
		WAITING_FOR_GAME,
		WATCHING,
		ANSWERED,
	};

	Stage stage = IDLE;
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	String scene;
	double deadline = 0.0;
	double started_msec = 0.0;
	double watch_timeout = 60.0;
	Dictionary answer;

	void _finish(const Dictionary &p_reply) {
		// Stop the game whatever happened. A test scene left running would be inherited
		// by whatever ran next, and the next tool's answers would be about this scene.
		if (EditorInterface::get_singleton()) {
			EditorInterface::get_singleton()->stop_playing_scene();
		}
		answer = p_reply;
		stage = ANSWERED;
	}

	void _on_reply(bool p_ok, const Dictionary &p_payload) {
		if (stage != WATCHING) {
			return;
		}
		if (!p_ok) {
			_fail(String(p_payload.get("message", "the test scene did not report a result")));
			return;
		}
		if (!(bool)p_payload.get("declared", false)) {
			_fail(vformat("'%s' ran, but its root node declares neither `test_finished` nor "
						  "`test_results`, so it is not a test scene this can read. A test scene "
						  "sets `test_finished` to true and fills `test_results` with one entry "
						  "per case.",
					scene));
			return;
		}
		if (!(bool)p_payload.get("finished", false)) {
			_fail(vformat("'%s' never set `test_finished` within %d seconds. It reported %d "
						  "case(s) before giving up.",
					scene, (int)watch_timeout, (int)Array(p_payload.get("cases", Array())).size()));
			return;
		}

		const Array cases = p_payload.get("cases", Array());
		int passed = 0;
		int failed = 0;
		double duration = 0.0;
		Array reported;
		for (int i = 0; i < cases.size(); i++) {
			const Dictionary entry = cases[i];
			Dictionary normalised;
			normalised["name"] = entry.get("name", vformat("case %d", i + 1));
			const bool ok = entry.get("passed", false);
			normalised["passed"] = ok;
			normalised["message"] = entry.get("message", String());
			normalised["duration_ms"] = entry.get("duration_ms", 0);
			duration += (double)normalised["duration_ms"];
			ok ? passed++ : failed++;
			reported.push_back(normalised);
		}

		Dictionary result;
		result["scene"] = scene;
		result["passed"] = passed;
		result["failed"] = failed;
		result["total"] = cases.size();
		result["cases"] = reported;
		result["case_duration_ms"] = duration;
		result["wall_duration_ms"] = OS::get_singleton()->get_ticks_msec() - started_msec;
		// A run in which nothing ran is not a pass, and reporting "0 failed" would let it
		// be read as one.
		result["succeeded"] = failed == 0 && cases.size() > 0;
		_finish(result);
	}

	void _fail(const String &p_message) {
		if (EditorInterface::get_singleton()) {
			EditorInterface::get_singleton()->stop_playing_scene();
		}
		stage = IDLE;
		MCPDeferred::fail(token, MCPToolError::FAILED, p_message);
		token = MCPDeferred::INVALID_TOKEN;
	}

	Variant _poll() {
		switch (stage) {
			case WAITING_FOR_GAME: {
				MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
				if (bridge && bridge->is_game_reachable()) {
					stage = WATCHING;
					if (!bridge->request("watch_scene_test", _watch_arguments(), watch_timeout + 5.0,
								callable_mp(this, &RunSceneTestTool::_on_reply))) {
						_fail("the test scene started but its runtime agent could not be reached");
					}
					return Variant();
				}
				if (OS::get_singleton()->get_ticks_msec() / 1000.0 > deadline) {
					_fail(vformat("'%s' did not start within the timeout; check the Output log "
								  "for a script error that stopped it booting",
							scene));
				}
				return Variant();
			}

			case ANSWERED: {
				stage = IDLE;
				token = MCPDeferred::INVALID_TOKEN;
				return answer;
			}

			default:
				// WATCHING: the game answers by calling back, not by being polled.
				return Variant();
		}
	}

	Dictionary _watch_arguments() const {
		Dictionary arguments;
		arguments["timeout_seconds"] = watch_timeout;
		return arguments;
	}

public:
	virtual String get_tool_name() const override { return "Godot_RunSceneTest"; }
	virtual String get_description() const override {
		return "Play a test scene and return its per-case results. The scene runs under the "
			   "real engine, through the same play path a developer uses - no shell command is "
			   "executed, here or anywhere in this interface. The scene reports by setting "
			   "`test_finished` to true on its root node and filling `test_results` with one "
			   "entry per case ({name, passed, message, duration_ms}), as script properties or "
			   "node metadata. The game is stopped afterwards either way.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Test scene to run, as a res:// path.");
		properties["timeout_seconds"] = MCPSchema::integer_property(
				"How long to give the scene to finish once it is running.", 60);
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["scene"] = MCPSchema::string_property("Scene that ran.");
		properties["succeeded"] = MCPSchema::bool_property(
				"True when at least one case ran and none failed.");
		properties["passed"] = MCPSchema::integer_property("Cases that passed.");
		properties["failed"] = MCPSchema::integer_property("Cases that failed.");
		properties["total"] = MCPSchema::integer_property("Cases reported.");
		properties["cases"] = MCPSchema::array_property(
				"One entry per case: name, passed, message, duration_ms.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["case_duration_ms"] = MCPSchema::number_property(
				"Total time the cases reported for themselves.");
		properties["wall_duration_ms"] = MCPSchema::number_property(
				"Time from launching the scene to its answer, which includes engine startup.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "'Godot_RunSceneTest' needs a running Godot editor");
			return Dictionary();
		}
		if (stage != IDLE) {
			r_error.set(MCPToolError::INVALID_STATE,
					vformat("a test is already running ('%s'); there is one play session, so "
							"they cannot overlap",
							scene));
			return Dictionary();
		}

		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_existing(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}
		if (resolved.res_path.get_extension().to_lower() != "tscn") {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' is not a scene; a test is a scene the engine plays",
							resolved.res_path));
			return Dictionary();
		}

		scene = resolved.res_path;
		watch_timeout = MAX(1, (int)p_arguments.get("timeout_seconds", 60));
		started_msec = OS::get_singleton()->get_ticks_msec();
		// Booting an engine is slower than anything the scene itself will do, so the wait
		// for the game to appear gets its own allowance rather than eating the test's.
		deadline = OS::get_singleton()->get_ticks_msec() / 1000.0 + 60.0;
		stage = WAITING_FOR_GAME;
		answer = Dictionary();

		EditorInterface::get_singleton()->play_custom_scene(scene);

		token = MCPDeferred::begin_polled(watch_timeout + 90.0,
				callable_mp(this, &RunSceneTestTool::_poll));
		return MCPDeferred::make_deferred_result(token);
	}
};

} // namespace

void mcp_register_scene_test_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(ListSceneTestsTool)));
	registry->register_tool(Ref<MCPTool>(memnew(RunSceneTestTool)));
}
