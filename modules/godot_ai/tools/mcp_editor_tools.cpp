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
#include "../mcp_deferred.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"
#include "servers/display/display_server.h"

#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_data.h"
#include "editor/script/script_editor_base.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/run/editor_run_bar.h"
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

// The game processes the editor believes it owns.
//
// A consuming project lost an hour to three game instances running at once: under software
// rendering they starve each other, and a runtime read and an input injection do not
// necessarily reach the same one. Every symptom - 1 fps, a frame counter that stopped, touch
// input accepted and never delivered - pointed at the game. None of it was the game.
//
// Counting them was a shell trick, and `pgrep -f` matches its own command line, so the trick
// has its own trap. This makes it a tool answer.
static void add_game_processes(Dictionary &r_result) {
	EditorRunBar *bar = EditorRunBar::get_singleton();
	if (!bar) {
		return;
	}
	Array pids;
	for (const ProcessID &pid : bar->get_editor_run().pids) {
		pids.push_back((int64_t)pid);
	}
	r_result["game_pids"] = pids;
	r_result["game_process_count"] = pids.size();
	if (pids.size() > 1) {
		// Not an error - the editor can legitimately run several - but it *is* the thing to
		// know before believing any runtime measurement.
		r_result["game_process_note"] = "more than one game is running: a runtime read and an "
									   "input injection may not reach the same process, and "
									   "under software rendering they starve each other";
	}
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

// An open scene tab holds an in-memory copy that outranks the file on disk, and "Play"
// saves every dirty scene before launching. So an agent that deletes or rewrites a scene
// file behind the editor's back sees it silently reappear on the next play - with no
// error anywhere, which reads as a broken filesystem tool rather than a stale tab.
//
// Godot_OpenScene could open tabs and nothing could close one, so the fix belongs here.
class CloseSceneTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CloseScene"; }
	virtual String get_description() const override {
		return "Close an open scene tab in the editor, so its in-memory copy can no longer be written back over the file on disk.";
	}
	// Not read_project: closing a tab drops the editor's copy of the scene, and with
	// discard_unsaved it destroys edits. That is a scene mutation by any honest reading.
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Scene to close, as a res:// path. Omit to close the currently edited scene.");
		properties["discard_unsaved"] = MCPSchema::bool_property("Close even when the scene has unsaved changes, losing them. Refused by default.", false);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["closed"] = MCPSchema::bool_property("True when the tab was closed.");
		properties["path"] = MCPSchema::string_property("Scene that was closed.");
		properties["open_scenes"] = MCPSchema::array_property("Scenes still open in the editor, as res:// paths.", MCPSchema::string_property("A res:// path."));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!require_editor(r_error, get_tool_name())) {
			return Dictionary();
		}

		String path;
		if (p_arguments.has("path")) {
			// resolve() rather than resolve_existing(): the whole point of this tool is
			// closing a tab whose file the caller has already deleted.
			MCPPaths::Resolved resolved;
			String error;
			if (!MCPPaths::resolve(p_arguments["path"], resolved, error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
			}
			path = resolved.res_path;
		} else {
			Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
			if (!root) {
				r_error.set(MCPToolError::INVALID_STATE, "there is no scene open in the editor to close");
				return Dictionary();
			}
			path = root->get_scene_file_path();
			if (path.is_empty()) {
				r_error.set(MCPToolError::INVALID_STATE,
						"the edited scene has never been saved, so it has no path to identify it by");
				return Dictionary();
			}
		}

		const bool discard = p_arguments.has("discard_unsaved") && (bool)p_arguments["discard_unsaved"];
		String error;
		if (!EditorNode::get_singleton()->close_scene_by_path(path, discard, error)) {
			// A scene that is not open and a scene with unsaved edits are different
			// refusals, and a caller can act on the difference.
			r_error.set(error.contains("unsaved") ? MCPToolError::INVALID_STATE : MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}

		Array open_scenes;
		EditorData &data = EditorNode::get_singleton()->get_editor_data();
		for (int i = 0; i < data.get_edited_scene_count(); i++) {
			open_scenes.push_back(data.get_scene_path(i));
		}

		Dictionary result;
		result["closed"] = true;
		result["path"] = path;
		result["open_scenes"] = open_scenes;
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
				? "Run the project's main scene, and wait until the runtime tools can reach it. "
				  "Changes made while the game runs are not persistent."
				: "Run the scene currently open in the editor, and wait until the runtime tools "
				  "can reach it. Changes made while the game runs are not persistent.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["playing"] = MCPSchema::bool_property("True when the game is running.");
		properties["game_pids"] = MCPSchema::array_property(
				"Process ids of the running game(s).", MCPSchema::integer_property("A pid."));
		properties["game_process_count"] = MCPSchema::integer_property(
				"How many game processes the editor is running. More than one means a runtime "
				"read and an input injection may not reach the same process.");
		properties["reachable"] = MCPSchema::bool_property(
				"True when the runtime tools can talk to the game. This call waits for it, so a "
				"false here means the game started and its debugger connection never came up.");
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

		// Answers when the game can actually be *talked to*, not when the process has
		// been launched.
		//
		// Launching is instant; the debugger connection that every runtime tool goes
		// through takes a second or two more. Returning at launch meant `playing: true`
		// followed immediately by "no game is running" from the very next call - which is
		// not true, and sends the reader looking for a crash that did not happen. Making
		// the caller sleep an arbitrary amount is the thing this whole interface exists
		// to stop people doing.
		deadline_msec = OS::get_singleton()->get_ticks_msec() + REACHABLE_TIMEOUT_MSEC;
		return MCPDeferred::make_deferred_result(
				MCPDeferred::begin_polled(REACHABLE_TIMEOUT_MSEC / 1000.0 + 2.0,
						callable_mp(this, &PlaySceneTool::_poll)));
	}

private:
	static constexpr uint64_t REACHABLE_TIMEOUT_MSEC = 20000;
	uint64_t deadline_msec = 0;

	Variant _poll() {
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		const bool reachable = bridge && bridge->is_game_reachable();
		const bool timed_out = OS::get_singleton()->get_ticks_msec() >= deadline_msec;
		if (!reachable && !timed_out) {
			return Variant();
		}

		Dictionary result;
		result["playing"] = EditorInterface::get_singleton()
				? EditorInterface::get_singleton()->is_playing_scene()
				: false;
		result["reachable"] = reachable;
		add_game_processes(result);
		if (!reachable) {
			// Reported rather than refused: the process may be alive and simply slow, and
			// the pid list above is the thing to look at next.
			result["note"] = "the game was launched but its debugger connection has not come "
							 "up, so the runtime tools cannot reach it yet. If the process "
							 "list above is empty it failed to start; read Godot_ReadOutputLog.";
		}
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
		return "Report what the editor is currently doing: edited scene, play state, project root, "
			   "and whether it has a display, which decides whether the visual tools can work.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["has_editor"] = MCPSchema::bool_property("True when an editor interface is available.");
		properties["edited_scene"] = MCPSchema::string_property("Path of the edited scene, empty when none.");
		properties["playing"] = MCPSchema::bool_property("True when the game is running.");
		properties["game_paused_at_breakpoint"] = MCPSchema::bool_property(
				"True when the running game is stopped at a debugger break. Its process is alive "
				"and still answers every runtime tool, but its frames have stopped: input is "
				"accepted and never delivered, paced gestures never complete, and captures return "
				"the same frame forever. Nothing else in this interface reveals this - the game "
				"looks healthy from every read - so check it before diagnosing a hang.");
		properties["project_root"] = MCPSchema::string_property("Absolute project directory.");
		properties["display_server"] = MCPSchema::string_property(
				"Name of the display server driving this editor, \"headless\" when it has none.");
		properties["can_render"] = MCPSchema::bool_property(
				"True when the editor is drawing to a display, so screenshots and dialogs work. "
				"False means it was launched headless; relaunching it under a display (see "
				"tools/virtual_display.py) is what makes the visual tools usable.");
		properties["main_screen"] = MCPSchema::string_property(
				"The workspace the user is looking at: 2D, 3D, Script, Game, AssetLib or GodotAI.");
		properties["selection"] = MCPSchema::array_property(
				"The nodes the user has selected in the scene tree, in selection order. When a "
				"request says \"this node\" or \"the enemy\", this is what it means.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["open_script"] = MCPSchema::string_property(
				"The script open in the script editor, empty when none is.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["project_root"] = MCPPaths::get_project_root();
		// An agent should be able to find out that screenshots are impossible here
		// without having to take one and read the refusal.
		const String display_server = DisplayServer::get_singleton()
				? DisplayServer::get_singleton()->get_name()
				: String("none");
		result["display_server"] = display_server;
		result["can_render"] = display_server != "headless" && display_server != "none";
		result["has_editor"] = EditorInterface::get_singleton() != nullptr;
		if (!EditorInterface::get_singleton()) {
			result["edited_scene"] = String();
			result["playing"] = false;
			result["game_paused_at_breakpoint"] = false;
			return result;
		}
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		result["edited_scene"] = root ? root->get_scene_file_path() : String();
		result["playing"] = EditorInterface::get_singleton()->is_playing_scene();
		// A game stopped at a break is the one state this interface could not see, and the
		// consuming project lost days to it: the process stays alive, every runtime tool
		// keeps answering from cached state, and the frame counter quietly stops. Reads of
		// errors, output, scene tree and performance all said the game was fine. Only the
		// editor's own Debugger dock knew, and no tool asked it.
		EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
		result["game_paused_at_breakpoint"] = debugger && debugger->get_default_debugger()
				? debugger->get_default_debugger()->is_breaked()
				: false;

		// What the user is looking at.
		//
		// The worst friction in the whole journey was here: you have a node selected in
		// the scene tree and you still have to describe it in prose, because the editor
		// knew and the agent did not. It rides on this tool rather than a new one so
		// that every caller already asking "what is the editor doing" gets the answer
		// without having to know to ask a second question.
		if (EditorNode::get_editor_main_screen() && EditorNode::get_editor_main_screen()->get_selected_plugin()) {
			result["main_screen"] = EditorNode::get_editor_main_screen()->get_selected_plugin()->get_plugin_name();
		} else {
			result["main_screen"] = String();
		}

		Array selection;
		if (EditorSelection *selected = EditorInterface::get_singleton()->get_selection()) {
			// Top-level selections only: when a node and its child are both selected,
			// the parent is what the user means, and reporting both invites the agent
			// to act twice on the same subtree.
			for (Node *node : selected->get_top_selected_node_list()) {
				if (!node) {
					continue;
				}
				Dictionary entry;
				entry["name"] = node->get_name();
				// Relative to the edited scene root, because that is the form every
				// other tool in this interface takes.
				entry["path"] = root ? String(root->get_path_to(node)) : String(node->get_path());
				entry["class"] = node->get_class();
				const Ref<Script> script = node->get_script();
				if (script.is_valid()) {
					entry["script"] = script->get_path();
				}
				selection.push_back(entry);
			}
		}
		result["selection"] = selection;

		// Through the editor base rather than ScriptEditor's own current-script helper,
		// which is private. The resource on the open tab is the same answer and needs
		// no engine change to reach.
		String open_script;
		if (ScriptEditor::get_singleton() && ScriptEditor::get_singleton()->get_current_editor()) {
			const Ref<Resource> edited = ScriptEditor::get_singleton()->get_current_editor()->get_edited_resource();
			if (edited.is_valid()) {
				open_script = edited->get_path();
			}
		}
		result["open_script"] = open_script;

		return result;
	}
};

// Why this reads the editor and not the game: the game is what has stopped. Every runtime
// tool routes a request through the debugger bus and waits for the game's own main loop to
// answer, and a broken game's main loop is exactly what is not running. The editor already
// holds the whole diagnosis - it received it at the moment of the break - so asking the
// editor is both the only thing that can work and the cheapest thing that could.
class GetDebuggerBreakTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetDebuggerBreak"; }
	virtual String get_description() const override {
		return "Report why the running game is stopped at a debugger break: the error, the "
			   "script call stack with file, function and line for every frame, and the locals "
			   "of the frame it stopped in. A broken game is alive and useless - it keeps its "
			   "window and keeps answering every other runtime tool from frozen state, while "
			   "input is accepted and never delivered and the frame counter never moves again. "
			   "Nothing else can tell you, because everything else asks the game, and the game "
			   "is what has stopped. Ask this the moment a game stops responding to input.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["breaked"] = MCPSchema::bool_property(
				"True when the game is stopped at a break. False means it is running normally, "
				"or is not running at all - check Godot_GetEditorStatus.playing to tell those apart.");
		properties["reason"] = MCPSchema::string_property(
				"The error or breakpoint that stopped it, as the Debugger dock shows it.");
		properties["thread"] = MCPSchema::string_property("Which thread stopped.");
		properties["can_debug"] = MCPSchema::bool_property(
				"True when the stopped thread can be stepped or continued.");
		properties["frames"] = MCPSchema::array_property(
				"The script call stack, innermost first: frame, file, function, line.",
				MCPSchema::object_schema(Dictionary()));
		properties["locals"] = MCPSchema::object_schema(Dictionary());
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *session = debugger ? debugger->get_default_debugger() : nullptr;
		if (!session) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no editor debugger");
			return Dictionary();
		}
		Dictionary result = session->get_break_report();
		if (!(bool)result.get("breaked", false)) {
			result["note"] = "the game is not stopped at a break";
		}
		return result;
	}
};

// Releasing the break is the other half. Without it the only way out of a break is to kill
// the game, which throws away the state that caused it - and a break is often the most
// informative state a session will ever have.
class ResumeFromBreakTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ResumeFromBreak"; }
	virtual String get_description() const override {
		return "Let a game stopped at a debugger break carry on running. Use it after reading "
			   "Godot_GetDebuggerBreak: the alternative is stopping the game, which destroys the "
			   "state that caused the break. Set skip_breakpoints to run past further breaks of "
			   "the same kind, for a route that would otherwise stop on every frame.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["skip_breakpoints"] = MCPSchema::bool_property(
				"Also stop breaking on subsequent breakpoints, so a route can be driven to its "
				"end past a fault that repeats.", false);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["resumed"] = MCPSchema::bool_property("True when a break was released.");
		properties["was_breaked"] = MCPSchema::bool_property("True when it had been stopped.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *session = debugger ? debugger->get_default_debugger() : nullptr;
		if (!session) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no editor debugger");
			return Dictionary();
		}
		Dictionary result;
		result["was_breaked"] = session->is_breaked();
		if (!session->is_breaked()) {
			result["resumed"] = false;
			result["note"] = "the game was not stopped at a break";
			return result;
		}
		if ((bool)p_arguments.get("skip_breakpoints", false)) {
			session->debug_skip_breakpoints();
		}
		session->debug_continue();
		result["resumed"] = true;
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
	mcp_register_input_tools();
	mcp_register_user_data_tools();
	mcp_register_project_config_tools();
	mcp_register_asset_tools();
	mcp_register_editor_ui_tools();
	mcp_register_scene_test_tools();
	mcp_register_profiler_tools();
	mcp_register_activity_tools();
	mcp_register_session_tools();
	mcp_register_workspace_tools();
	mcp_register_playtest_tools();
	mcp_register_promote_tools();
	mcp_register_variant_tools();
	mcp_register_proposal_tools();
	mcp_register_memory_tools();
	mcp_register_docs_tools();
	mcp_register_compare_tools();
	mcp_register_connection_tools();

	registry->register_tool(Ref<MCPTool>(memnew(OpenSceneTool)));
	registry->register_tool(Ref<MCPTool>(memnew(CloseSceneTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SaveSceneTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetEditedSceneTreeTool)));
	registry->register_tool(Ref<MCPTool>(memnew(PlaySceneTool(false))));
	registry->register_tool(Ref<MCPTool>(memnew(PlaySceneTool(true))));
	registry->register_tool(Ref<MCPTool>(memnew(StopPlayingTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetEditorStatusTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetDebuggerBreakTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ResumeFromBreakTool)));
}
