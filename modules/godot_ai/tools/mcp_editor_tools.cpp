/**************************************************************************/
/*  mcp_editor_tools.cpp                                                  */
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

#include "mcp_builtin_tools.h"

#include "../mcp_paths.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "scene/main/node.h"

namespace {

// Every editor tool goes through this: without a live editor there is nothing to
// drive, and saying so plainly beats crashing or lying about success.
//
// EditorInterface alone is not enough. It is created by register_editor_types(), so
// it exists in any editor build - including the headless unit-test binary - while
// EditorNode does not. Most EditorInterface methods dereference EditorNode, so
// checking only the former segfaults instead of refusing.
static bool require_editor(MCPToolError &r_error, const String &p_tool) {
	if (EditorNode::get_singleton() && EditorInterface::get_singleton()) {
		return true;
	}
	r_error.set(MCPToolError::UNSUPPORTED,
			vformat("'%s' needs a running Godot editor; this process has no editor interface", p_tool));
	return false;
}

static void serialize_node(Node *p_node, Node *p_scene_root, int p_depth, int p_max_depth, Array &r_nodes) {
	if (!p_node) {
		return;
	}
	Dictionary entry;
	entry["name"] = p_node->get_name();
	entry["type"] = p_node->get_class();
	entry["path"] = String(p_scene_root->get_path_to(p_node));
	entry["depth"] = p_depth;
	entry["child_count"] = p_node->get_child_count();
	if (!p_node->get_scene_file_path().is_empty()) {
		// Instanced sub-scene: the agent must know it is not editing plain nodes.
		entry["scene_file_path"] = p_node->get_scene_file_path();
	}
	const Ref<Script> script = p_node->get_script();
	if (script.is_valid()) {
		entry["script"] = script->get_path();
	}
	r_nodes.push_back(entry);

	if (p_depth >= p_max_depth) {
		return;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		serialize_node(p_node->get_child(i), p_scene_root, p_depth + 1, p_max_depth, r_nodes);
	}
}

class OpenSceneTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_OpenScene"; }
	virtual String get_description() const override {
		return "Open a scene in the editor and make it the edited scene.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Scene to open, as a res:// path.");
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Scene that was opened.");
		properties["root_name"] = MCPSchema::string_property("Name of the scene root node.");
		properties["root_type"] = MCPSchema::string_property("Class of the scene root node.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!require_editor(r_error, get_tool_name())) {
			return Dictionary();
		}
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_existing(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}
		const String extension = resolved.res_path.get_extension().to_lower();
		if (extension != "tscn" && extension != "scn" && extension != "res") {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is not a scene file", resolved.res_path));
			return Dictionary();
		}

		EditorInterface::get_singleton()->open_scene_from_path(resolved.res_path);

		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (!root) {
			r_error.set(MCPToolError::FAILED,
					vformat("the editor did not open '%s'; it may be broken or reference missing dependencies", resolved.res_path));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = resolved.res_path;
		result["root_name"] = root->get_name();
		result["root_type"] = root->get_class();
		return result;
	}
};

class SaveSceneTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SaveScene"; }
	virtual String get_description() const override {
		return "Save the currently edited scene, or every open scene.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["all"] = MCPSchema::bool_property("Save every open scene instead of only the current one.", false);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["saved"] = MCPSchema::bool_property("True when the save succeeded.");
		properties["path"] = MCPSchema::string_property("Scene that was saved, when saving a single scene.");
		return MCPSchema::object_schema(properties);
	}
	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		// Saving is the moment a scene edit becomes a file change, so this is where the
		// snapshot belongs - not on the in-memory edits that preceded it.
		Vector<String> paths;
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			return paths;
		}
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (root && !root->get_scene_file_path().is_empty()) {
			paths.push_back(root->get_scene_file_path());
		}
		return paths;
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!require_editor(r_error, get_tool_name())) {
			return Dictionary();
		}
		Dictionary result;
		if ((bool)p_arguments["all"]) {
			EditorInterface::get_singleton()->save_all_scenes();
			result["saved"] = true;
			return result;
		}

		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (!root) {
			r_error.set(MCPToolError::INVALID_STATE, "there is no scene open in the editor to save");
			return Dictionary();
		}
		const String path = root->get_scene_file_path();
		if (path.is_empty()) {
			// Saving would need a target path, which is the user's decision to make.
			r_error.set(MCPToolError::INVALID_STATE,
					"the edited scene has never been saved, so it has no path; save it once in the editor first");
			return Dictionary();
		}
		const Error error = EditorInterface::get_singleton()->save_scene();
		if (error != OK) {
			r_error.set(MCPToolError::FAILED, vformat("saving '%s' failed with error %d", path, (int)error));
			return Dictionary();
		}
		result["saved"] = true;
		result["path"] = path;
		return result;
	}
};

class GetEditedSceneTreeTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetEditedSceneTree"; }
	virtual String get_description() const override {
		return "Return the node tree of the scene currently open in the editor.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["max_depth"] = MCPSchema::integer_property("How deep to descend from the scene root.", 32);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary node_properties;
		node_properties["name"] = MCPSchema::string_property("Node name.");
		node_properties["type"] = MCPSchema::string_property("Node class.");
		node_properties["path"] = MCPSchema::string_property("Path relative to the scene root.");
		node_properties["depth"] = MCPSchema::integer_property("Depth below the root.");
		node_properties["child_count"] = MCPSchema::integer_property("Number of children.");

		Dictionary properties;
		properties["scene_path"] = MCPSchema::string_property("Path of the edited scene, empty when unsaved.");
		properties["nodes"] = MCPSchema::array_property("Nodes in depth-first order.",
				MCPSchema::object_schema(node_properties, Vector<String>(), true));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!require_editor(r_error, get_tool_name())) {
			return Dictionary();
		}
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (!root) {
			r_error.set(MCPToolError::INVALID_STATE, "there is no scene open in the editor");
			return Dictionary();
		}

		Array nodes;
		serialize_node(root, root, 0, (int)p_arguments["max_depth"], nodes);

		Dictionary result;
		result["scene_path"] = root->get_scene_file_path();
		result["nodes"] = nodes;
		return result;
	}
};

class PlaySceneTool : public MCPTool {
	bool main_scene = false;

public:
	explicit PlaySceneTool(bool p_main_scene) :
			main_scene(p_main_scene) {}

	virtual String get_tool_name() const override {
		return main_scene ? "Godot_PlayMainScene" : "Godot_PlayCurrentScene";
	}
	virtual String get_description() const override {
		return main_scene
				? "Run the project's main scene. Changes made while the game runs are not persistent."
				: "Run the scene currently open in the editor. Changes made while the game runs are not persistent.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["playing"] = MCPSchema::bool_property("True when the game is running.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!require_editor(r_error, get_tool_name())) {
			return Dictionary();
		}
		if (main_scene) {
			EditorInterface::get_singleton()->play_main_scene();
		} else {
			if (!EditorInterface::get_singleton()->get_edited_scene_root()) {
				r_error.set(MCPToolError::INVALID_STATE, "there is no scene open in the editor to run");
				return Dictionary();
			}
			EditorInterface::get_singleton()->play_current_scene();
		}

		Dictionary result;
		result["playing"] = EditorInterface::get_singleton()->is_playing_scene();
		return result;
	}
};

class StopPlayingTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_StopPlaying"; }
	virtual String get_description() const override { return "Stop the running game."; }
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["playing"] = MCPSchema::bool_property("True when the game is still running.");
		properties["was_playing"] = MCPSchema::bool_property("True when a game was running before this call.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!require_editor(r_error, get_tool_name())) {
			return Dictionary();
		}
		const bool was_playing = EditorInterface::get_singleton()->is_playing_scene();
		EditorInterface::get_singleton()->stop_playing_scene();

		Dictionary result;
		result["was_playing"] = was_playing;
		result["playing"] = EditorInterface::get_singleton()->is_playing_scene();
		return result;
	}
};

class GetEditorStatusTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetEditorStatus"; }
	virtual String get_description() const override {
		return "Report what the editor is currently doing: edited scene, play state, and project root.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["has_editor"] = MCPSchema::bool_property("True when an editor interface is available.");
		properties["edited_scene"] = MCPSchema::string_property("Path of the edited scene, empty when none.");
		properties["playing"] = MCPSchema::bool_property("True when the game is running.");
		properties["project_root"] = MCPSchema::string_property("Absolute project directory.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["project_root"] = MCPPaths::get_project_root();
		result["has_editor"] = EditorInterface::get_singleton() != nullptr;
		if (!EditorInterface::get_singleton()) {
			result["edited_scene"] = String();
			result["playing"] = false;
			return result;
		}
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		result["edited_scene"] = root ? root->get_scene_file_path() : String();
		result["playing"] = EditorInterface::get_singleton()->is_playing_scene();
		return result;
	}
};

} // namespace

void mcp_register_builtin_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	mcp_register_project_tools();
	mcp_register_scene_tools();
	mcp_register_skill_tools();
	mcp_register_checkpoint_tools();
	mcp_register_output_tools();
	mcp_register_property_tools();
	mcp_register_capture_tools();
	mcp_register_ask_user_tool();

	registry->register_tool(Ref<MCPTool>(memnew(OpenSceneTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SaveSceneTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetEditedSceneTreeTool)));
	registry->register_tool(Ref<MCPTool>(memnew(PlaySceneTool(false))));
	registry->register_tool(Ref<MCPTool>(memnew(PlaySceneTool(true))));
	registry->register_tool(Ref<MCPTool>(memnew(StopPlayingTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetEditorStatusTool)));
}
