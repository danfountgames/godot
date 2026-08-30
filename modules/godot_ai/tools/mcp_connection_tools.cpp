/**************************************************************************/
/*  mcp_connection_tools.cpp                                              */
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

#include "../mcp_schema.h"
#include "../mcp_tool_registry.h"

#include "core/object/class_db.h"
#include "core/variant/array.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"

namespace {

// Wiring a signal to a method.
//
// This was missing, and the way it was found is worth recording: not a test, but the
// first real benchmark run. `dead-button/find` is a task about an unwired control, and
// completing it meant writing the `.tscn` as text - the exact edit the repository's
// rules exist to prevent - because ninety-six tools included no way to connect a
// signal. It is among the commonest things anyone does in this editor.
//
// Text editing a scene is worse than it looks. A connection is not just a line in a
// file: the editor holds the scene as live nodes, so a hand-written `[connection]` is
// invisible until the scene is reloaded, it bypasses the undo history entirely, and it
// silently disagrees with whatever the editor has in memory. Going through
// Object::connect and EditorUndoRedoManager keeps all three honest.

static bool require_edited_scene(Node **r_root, MCPToolError &r_error, const String &p_tool) {
	if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
		r_error.set(MCPToolError::UNSUPPORTED,
				vformat("'%s' needs a running Godot editor; this process has no editor interface", p_tool));
		return false;
	}
	*r_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!*r_root) {
		r_error.set(MCPToolError::INVALID_STATE, "there is no scene open in the editor");
		return false;
	}
	return true;
}

static Node *resolve_node(Node *p_root, const String &p_path, MCPToolError &r_error) {
	const String path = p_path.strip_edges();
	if (path.is_empty() || path == "." || path == "/root") {
		return p_root;
	}
	Node *node = p_root->get_node_or_null(NodePath(path));
	if (!node) {
		r_error.set(MCPToolError::NOT_FOUND, vformat("no node at '%s' in the edited scene", p_path));
	}
	return node;
}

// The signals a node actually has, for a refusal that helps rather than just refuses.
//
// The class's *own* signals first, and only then inherited ones. Listing whatever
// get_signal_list() returns first offered `script_changed, property_list_changed,
// ready, renamed, tree_entered…` to someone who had misspelt a Button signal - eight
// names from Object and Node, and not one of them the answer.
static String nearby_signals(Node *p_node, const String &p_wanted) {
	List<MethodInfo> all;
	p_node->get_signal_list(&all);

	String close;
	for (const MethodInfo &info : all) {
		const String name = info.name;
		if (name.findn(p_wanted) >= 0 || p_wanted.findn(name) >= 0) {
			close += (close.is_empty() ? "" : ", ") + name;
		}
	}
	if (!close.is_empty()) {
		return vformat(". Did you mean: %s", close);
	}

	// Climb until a class actually declares something. Asking Button for its own
	// signals returns nothing - `pressed` belongs to BaseButton - so stopping at the
	// first empty answer offered Object's `script_changed, property_list_changed,
	// ready, renamed…` to someone who had misspelt `pressed`.
	String owner = p_node->get_class();
	while (!owner.is_empty() && owner != "Node" && owner != "Object") {
		List<MethodInfo> declared;
		ClassDB::get_signal_list(owner, &declared, true);
		if (!declared.is_empty()) {
			String listed;
			int count = 0;
			for (const MethodInfo &info : declared) {
				if (count++ >= 8) {
					break;
				}
				listed += (listed.is_empty() ? "" : ", ") + String(info.name);
			}
			return vformat(". %s has: %s", owner, listed);
		}
		owner = ClassDB::get_parent_class(owner);
	}
	return String();
}

// True when `p_node` belongs to the scene being edited rather than to the editor.
//
// Listing a node's connections without this is useless: the editor watches every node
// in the edited scene, so `Settings` came back with eleven connections and all eleven
// were ScriptEditor and SceneTreeEditor hooking `script_changed` and `tree_exited`.
// None of that is the scene's own wiring, and it would bury the one line that is.
static bool inside_scene(Node *p_root, Node *p_node) {
	for (Node *walk = p_node; walk; walk = walk->get_parent()) {
		if (walk == p_root) {
			return true;
		}
	}
	return false;
}

class ManageConnectionTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ManageConnection"; }
	virtual String get_description() const override {
		return "Connect a node's signal to a method on another node, disconnect one, or list "
			   "what a node is connected to. This is how a button is wired to the code that "
			   "runs when it is pressed. Every change goes through the editor's undo history, "
			   "so Godot_UndoLastAction reverts it, and the scene is not saved automatically - "
			   "call Godot_SaveScene to persist. Never write a [connection] line into a .tscn "
			   "by hand: the editor holds the scene as live nodes, so a hand-edited connection "
			   "is invisible until reload and disagrees with what the editor has in memory.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }

	virtual Dictionary get_input_schema() const override {
		Vector<String> actions;
		actions.push_back("connect");
		actions.push_back("disconnect");
		actions.push_back("list");

		Dictionary properties;
		properties["action"] = MCPSchema::enum_property("What to do.", actions, "list");
		properties["from"] = MCPSchema::string_property(
				"Node emitting the signal, relative to the scene root. '.' is the root.");
		properties["signal"] = MCPSchema::string_property(
				"Signal name, e.g. 'pressed'. Required to connect or disconnect.", "");
		properties["to"] = MCPSchema::string_property(
				"Node carrying the method. Defaults to the scene root, which is where a "
				"scene's own script usually lives.", "");
		properties["method"] = MCPSchema::string_property(
				"Method to call. Required to connect or disconnect.", "");
		Vector<String> required;
		required.push_back("from");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property("What was done.");
		properties["connections"] = MCPSchema::array_property(
				"Connections from the node, when listing.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["from"] = MCPSchema::string_property("The emitting node.");
		properties["signal"] = MCPSchema::string_property("The signal.");
		properties["to"] = MCPSchema::string_property("The receiving node.");
		properties["method"] = MCPSchema::string_property("The method.");
		properties["editor_connections_hidden"] = MCPSchema::integer_property(
				"How many connections were left out because they are machinery rather than "
				"this scene's wiring: the editor watching the scene, and the engine's own "
				"bindings such as a container laying its children out.");
		properties["note"] = MCPSchema::string_property("Guidance when the answer needs it.");
		return MCPSchema::object_schema(properties);
	}

	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		if (EditorInterface::get_singleton()) {
			Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
			if (root && !root->get_scene_file_path().is_empty()) {
				paths.push_back(root->get_scene_file_path());
			}
		}
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Node *root = nullptr;
		if (!require_edited_scene(&root, r_error, get_tool_name())) {
			return Dictionary();
		}

		const String action = String(p_arguments.get("action", "list"));
		Node *from = resolve_node(root, p_arguments["from"], r_error);
		if (!from) {
			return Dictionary();
		}

		Dictionary result;
		result["action"] = action;
		result["from"] = String(root->get_path_to(from));

		if (action == "list") {
			Array listed;
			int editor_side = 0;
			int engine_side = 0;
			List<MethodInfo> signals;
			from->get_signal_list(&signals);
			for (const MethodInfo &info : signals) {
				// Not `connections`: Object has a member of that name and -Wshadow is
				// an error here.
				List<Object::Connection> wired;
				from->get_signal_connection_list(info.name, &wired);
				for (const Object::Connection &connection : wired) {
					Node *target = Object::cast_to<Node>(connection.callable.get_object());
					if (!target || !inside_scene(root, target)) {
						// The editor's own observers, not the scene's wiring. Counted
						// rather than silently dropped.
						editor_side++;
						continue;
					}
					const String method = String(connection.callable.get_method());
					if (method.contains("::")) {
						// An engine binding, not the scene's wiring: a Button inside a
						// VBoxContainer carries five of these
						// (Container::_child_minsize_changed and friends) against one
						// line the author wrote, so a listing that includes them buries
						// the answer. Counted with the editor's own observers, because
						// both are "the machinery, not your scene".
						engine_side++;
						continue;
					}
					Dictionary entry;
					entry["signal"] = String(info.name);
					entry["method"] = method;
					entry["to"] = String(root->get_path_to(target));
					listed.push_back(entry);
				}
			}
			result["connections"] = listed;
			if (editor_side + engine_side > 0) {
				result["editor_connections_hidden"] = editor_side + engine_side;
			}
			if (listed.is_empty()) {
				result["note"] = vformat(
						"%s emits nothing to anything inside this scene. A control that does "
						"nothing when pressed usually looks exactly like this.",
						String(root->get_path_to(from)));
			}
			return result;
		}

		const String signal_name = String(p_arguments.get("signal", String())).strip_edges();
		const String method_name = String(p_arguments.get("method", String())).strip_edges();
		if (signal_name.is_empty() || method_name.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' needs both a 'signal' and a 'method'", action));
			return Dictionary();
		}

		const String to_path = String(p_arguments.get("to", String())).strip_edges();
		Node *to = to_path.is_empty() ? root : resolve_node(root, to_path, r_error);
		if (!to) {
			return Dictionary();
		}

		if (!from->has_signal(signal_name)) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("%s has no signal called '%s'%s", from->get_class(), signal_name,
							nearby_signals(from, signal_name)));
			return Dictionary();
		}

		const Callable callable(to, StringName(method_name));
		result["signal"] = signal_name;
		result["to"] = String(root->get_path_to(to));
		result["method"] = method_name;

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		ERR_FAIL_NULL_V(undo_redo, Dictionary());

		if (action == "disconnect") {
			if (!from->is_connected(signal_name, callable)) {
				r_error.set(MCPToolError::NOT_FOUND,
						vformat("%s's '%s' is not connected to %s on %s", String(root->get_path_to(from)),
								signal_name, method_name, String(root->get_path_to(to))));
				return Dictionary();
			}
			undo_redo->create_action(vformat("Disconnect %s from %s", signal_name, method_name));
			undo_redo->add_do_method(from, "disconnect", signal_name, callable);
			// Restored as a persistent connection, because that is what it was: undoing
			// a disconnect must put back something that survives a save.
			undo_redo->add_undo_method(from, "connect", signal_name, callable, (uint32_t)Object::CONNECT_PERSIST);
			undo_redo->commit_action();
			return result;
		}

		// Connecting. A method that does not exist is the defect this tool most often
		// exists to fix, so creating the connection anyway would be reproducing the bug
		// rather than repairing it - and it is exactly what a hand-edited .tscn does.
		if (!to->has_method(method_name)) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("%s has no method called '%s', so connecting to it would make a "
							"connection that does nothing - which is the defect, not the fix. "
							"Add the method first, or name the one that is already there.",
							String(root->get_path_to(to)), method_name));
			return Dictionary();
		}
		if (from->is_connected(signal_name, callable)) {
			// Already wired is not a failure; reporting it as one invites an agent to
			// "fix" it a second way.
			result["note"] = "This connection already existed; nothing was changed.";
			return result;
		}

		// CONNECT_PERSIST, and this is the whole difference between a tool that works and
		// one that only appears to. A plain Object::connect is a runtime connection:
		// PackedScene::pack() only records connections carrying this flag, so without it
		// the editor behaves correctly for the rest of the session, Godot_SaveScene
		// reports success, and the connection is simply absent from the .tscn. It was
		// absent, on the first run of this tool against the benchmark it was written for.
		undo_redo->create_action(vformat("Connect %s to %s", signal_name, method_name));
		undo_redo->add_do_method(from, "connect", signal_name, callable, (uint32_t)Object::CONNECT_PERSIST);
		undo_redo->add_undo_method(from, "disconnect", signal_name, callable);
		undo_redo->commit_action();
		return result;
	}
};

} // namespace

void mcp_register_connection_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(ManageConnectionTool)));
}
