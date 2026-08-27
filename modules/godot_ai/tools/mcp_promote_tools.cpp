/**************************************************************************/
/*  mcp_promote_tools.cpp                                                 */
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

// Keeping a value that was tuned while the game was running.
//
// This is a small tool and it is the last act of the loop everything else in this module
// exists to support. The agent changes a value in the running game with
// Godot_SetRuntimeProperty, a person watches it and decides it feels right - and until
// now that was where it stopped. The value lived in a process that was about to exit,
// and carrying it back into the project meant reading a number off the screen and typing
// it in. Live tuning whose last act is manual is theatre.
//
// It is one tool rather than a suggestion to call two, because the interesting part is
// the join: the running game's `/root/Main/Player` and the editor's `Player` are two
// addresses for what should be the same node, and deciding they are the same node is
// where a promotion can silently write the right value onto the wrong thing. That rule
// is in `mcp_runtime_paths.{h,cpp}` with tests; this file is the round trip.

#include "mcp_builtin_tools.h"

#include "../mcp_checkpoints.h"
#include "../mcp_deferred.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_runtime_paths.h"
#include "../mcp_schema.h"
#include "../mcp_tool_registry.h"

#include "core/object/callable_mp.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"
#endif

namespace {

class PromoteRuntimeValueTool : public MCPTool {
	// One promotion at a time. Each is two round trips and they would otherwise
	// interleave their answers.
	bool running = false;
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	String runtime_path;
	String scene_path;
	String property;

	void _fail(MCPToolError::Kind p_kind, const String &p_message) {
		running = false;
		if (token != MCPDeferred::INVALID_TOKEN) {
			MCPDeferred::fail(token, p_kind, p_message);
			token = MCPDeferred::INVALID_TOKEN;
		}
	}

	void _complete(const Dictionary &p_result) {
		running = false;
		if (token != MCPDeferred::INVALID_TOKEN) {
			MCPDeferred::complete(token, p_result);
			token = MCPDeferred::INVALID_TOKEN;
		}
	}

	// The running game answered with the value. Everything from here is editor-side.
	void _on_value(bool p_ok, const Dictionary &p_payload) {
		if (!running) {
			return;
		}
#ifdef TOOLS_ENABLED
		if (!p_ok) {
			_fail(MCPToolError::NOT_FOUND,
					vformat("could not read '%s' from '%s' in the running game: %s", property,
							runtime_path, String(p_payload.get("error", "no reason given"))));
			return;
		}

		Node *root = EditorInterface::get_singleton()
				? EditorInterface::get_singleton()->get_edited_scene_root()
				: nullptr;
		if (!root) {
			// The scene closed while the game was answering.
			_fail(MCPToolError::INVALID_STATE, "there is no scene open in the editor");
			return;
		}
		Node *node = scene_path == "." ? root : root->get_node_or_null(NodePath(scene_path));
		if (!node) {
			_fail(MCPToolError::NOT_FOUND,
					vformat("'%s' is in the running game but there is no '%s' in the edited "
							"scene; the running scene and the open one have drifted apart",
							runtime_path, scene_path));
			return;
		}
		if (node != root && node->get_owner() != root) {
			_fail(MCPToolError::INVALID_STATE,
					vformat("'%s' belongs to an instanced sub-scene; promote it in that scene "
							"instead, or the value would be written where it cannot be seen",
							scene_path));
			return;
		}

		bool valid = false;
		const Variant before = node->get(property, &valid);
		if (!valid) {
			_fail(MCPToolError::INVALID_ARGUMENTS,
					vformat("the running node has '%s' but the edited '%s' does not; the two have "
							"drifted apart",
							property, node->get_class()));
			return;
		}

		// The value as the game holds it, converted to the edited property's type. The
		// runtime answer travels as text precisely because it round-trips for every type;
		// the JSON `value` loses Vector2 and Color.
		const String text = p_payload.get("text", String());
		Variant value = p_payload.get("value", Variant());
		if (!text.is_empty()) {
			const Variant parsed = _parse_like(before, text);
			if (parsed.get_type() == before.get_type()) {
				value = parsed;
			}
		}
		if (value.get_type() != before.get_type()) {
			_fail(MCPToolError::FAILED,
					vformat("the running value of '%s' is a %s and the edited property is a %s; "
							"promoting it would change the property's type",
							property, Variant::get_type_name(value.get_type()),
							Variant::get_type_name(before.get_type())));
			return;
		}

		if (value == before) {
			// Nothing to do, and saying so beats a checkpoint and an undo step for a
			// write that changes nothing.
			Dictionary unchanged;
			unchanged["promoted"] = false;
			unchanged["reason"] = "the edited scene already holds this value";
			unchanged["runtime_path"] = runtime_path;
			unchanged["scene_path"] = scene_path;
			unchanged["property"] = property;
			unchanged["value"] = value;
			unchanged["text"] = String(value);
			_complete(unchanged);
			return;
		}

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(vformat("Godot AI: promote %s.%s from the running game",
				node->get_name(), property));
		undo_redo->add_do_property(node, property, value);
		undo_redo->add_undo_property(node, property, before);
		undo_redo->commit_action();

		// Read back before answering: a write that did not take is the failure mode this
		// whole tool family exists to make impossible to report as success.
		bool after_valid = false;
		const Variant after = node->get(property, &after_valid);
		if (!after_valid || after != value) {
			_fail(MCPToolError::FAILED,
					vformat("'%s' did not take the promoted value; it holds %s instead", property,
							String(after)));
			return;
		}

		Dictionary result;
		result["promoted"] = true;
		result["runtime_path"] = runtime_path;
		result["scene_path"] = scene_path;
		result["property"] = property;
		result["value"] = value;
		result["text"] = String(value);
		result["previous_text"] = String(before);
		result["persistent"] = false;
		result["next"] = "The scene is changed but not saved. Call Godot_SaveScene to keep it, "
						 "or Godot_UndoLastAction to put it back.";
		_complete(result);
#else
		_fail(MCPToolError::UNSUPPORTED, "promotion needs a running editor");
#endif
	}

	// Parses `p_text` into the same type as `p_like`.
	//
	// The runtime answer carries the value twice: as JSON, which cannot express a
	// Vector2 or a Color, and as Godot's own text form, which round-trips every type.
	// The text is what makes promoting a position possible at all, so it is preferred
	// and the JSON value is the fallback.
	static Variant _parse_like(const Variant &p_like, const String &p_text) {
		const Variant parsed = mcp_variant_from_text(p_text);
		if (parsed.get_type() == p_like.get_type()) {
			return parsed;
		}
		// Numbers are the common case and the one worth coercing: a float property whose
		// running value prints as "3" would otherwise be refused for being an int.
		if (p_like.get_type() == Variant::FLOAT && parsed.get_type() == Variant::INT) {
			return (double)(int64_t)parsed;
		}
		if (p_like.get_type() == Variant::INT && parsed.get_type() == Variant::FLOAT) {
			return (int64_t)(double)parsed;
		}
		return parsed;
	}

public:
	virtual String get_tool_name() const override { return "Godot_PromoteRuntimeValue"; }
	virtual String get_description() const override {
		return "Keep a value you tuned while the game was running. Reads a property from a node "
			   "in the running game and writes it into the same node in the edited scene, so a "
			   "number that felt right in play becomes the authored number. This is the last "
			   "act of the tuning loop: Godot_SetRuntimeProperty changes it live, this makes it "
			   "stick. The scene is changed but not saved - call Godot_SaveScene to keep it, or "
			   "Godot_UndoLastAction to put it back. Refuses when the running scene and the open "
			   "one are not the same scene, rather than writing a value from one into the other.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property(
				"Node path in the *running game*, such as /root/Main/Player. The matching node "
				"in the edited scene is worked out from it.");
		properties["property"] = MCPSchema::string_property(
				"Property to promote, such as 'speed' or 'position'.");
		Vector<String> required;
		required.push_back("path");
		required.push_back("property");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["promoted"] = MCPSchema::bool_property(
				"True when the edited scene was changed. False when it already held the value.");
		properties["runtime_path"] = MCPSchema::string_property("Where the value came from.");
		properties["scene_path"] = MCPSchema::string_property("Where it went, in the edited scene.");
		properties["property"] = MCPSchema::string_property("The property that was promoted.");
		properties["value"] = MCPSchema::any_property("The value, for types JSON can carry.");
		properties["text"] = MCPSchema::string_property("The value in Godot's own text form.");
		properties["previous_text"] = MCPSchema::string_property("What the scene held before.");
		properties["next"] = MCPSchema::string_property("What to do to keep it.");
		return MCPSchema::object_schema(properties);
	}

	// The scene file this may change, so the protocol layer snapshots it first.
	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
#ifdef TOOLS_ENABLED
		if (EditorInterface::get_singleton()) {
			Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
			if (root && !root->get_scene_file_path().is_empty()) {
				paths.push_back(root->get_scene_file_path());
			}
		}
#endif
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
#ifdef TOOLS_ENABLED
		if (running) {
			r_error.set(MCPToolError::INVALID_STATE,
					"a promotion is already in flight; wait for it to answer");
			return Dictionary();
		}

		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no game is running, so there is no tuned value to keep. Press play, set the "
					"value with Godot_SetRuntimeProperty, then promote it.");
			return Dictionary();
		}

		Node *root = EditorInterface::get_singleton()
				? EditorInterface::get_singleton()->get_edited_scene_root()
				: nullptr;
		if (!root) {
			r_error.set(MCPToolError::INVALID_STATE,
					"there is no scene open in the editor to promote into");
			return Dictionary();
		}

		runtime_path = String(p_arguments.get("path", String())).strip_edges();
		property = String(p_arguments.get("property", String())).strip_edges();
		if (property.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "a property name is needed");
			return Dictionary();
		}

		String path_error;
		if (!MCPRuntimePaths::to_scene_path(runtime_path, String(root->get_name()), scene_path,
					path_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, path_error);
			return Dictionary();
		}

		Dictionary arguments;
		arguments["path"] = runtime_path;
		arguments["property"] = property;

		running = true;
		token = MCPDeferred::begin(15.0,
				"the running game did not answer with the value in time");
		if (!bridge->request("get_property", arguments, 12.0,
					callable_mp(this, &PromoteRuntimeValueTool::_on_value))) {
			running = false;
			MCPDeferred::abandon(token);
			token = MCPDeferred::INVALID_TOKEN;
			r_error.set(MCPToolError::INVALID_STATE, "the running game stopped answering");
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(token);
#else
		r_error.set(MCPToolError::UNSUPPORTED, "promotion needs a running editor");
		return Dictionary();
#endif
	}
};

} // namespace

void mcp_register_promote_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(PromoteRuntimeValueTool)));
}
