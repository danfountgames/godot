/**************************************************************************/
/*  mcp_property_tools.cpp                                                */
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

#include "../mcp_deferred.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/editor_debugger_tree.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"

namespace {

// JSON has no Vector2/Vector3/Color, so a value arriving from a client has to be
// coerced into whatever the property already holds. Coercing against the *current*
// value means the tool never has to be taught Godot's whole type system.
static bool coerce_value(const Variant &p_current, const Variant &p_incoming, Variant &r_out, String &r_error) {
	const Variant::Type wanted = p_current.get_type();
	if (p_incoming.get_type() == wanted) {
		r_out = p_incoming;
		return true;
	}

	switch (wanted) {
		case Variant::NIL:
			r_out = p_incoming;
			return true;
		case Variant::BOOL:
			r_out = p_incoming.booleanize();
			return true;
		case Variant::INT:
			if (p_incoming.get_type() == Variant::FLOAT || p_incoming.get_type() == Variant::INT) {
				r_out = (int64_t)(double)p_incoming;
				return true;
			}
			break;
		case Variant::FLOAT:
			if (p_incoming.get_type() == Variant::FLOAT || p_incoming.get_type() == Variant::INT) {
				r_out = (double)p_incoming;
				return true;
			}
			break;
		case Variant::STRING:
		case Variant::STRING_NAME:
			r_out = String(p_incoming);
			return true;
		case Variant::VECTOR2:
		case Variant::VECTOR2I:
		case Variant::VECTOR3:
		case Variant::VECTOR3I:
		case Variant::COLOR: {
			if (p_incoming.get_type() != Variant::ARRAY) {
				break;
			}
			const Array values = p_incoming;
			if (wanted == Variant::VECTOR2 && values.size() == 2) {
				r_out = Vector2((double)values[0], (double)values[1]);
				return true;
			}
			if (wanted == Variant::VECTOR2I && values.size() == 2) {
				r_out = Vector2i((int)values[0], (int)values[1]);
				return true;
			}
			if (wanted == Variant::VECTOR3 && values.size() == 3) {
				r_out = Vector3((double)values[0], (double)values[1], (double)values[2]);
				return true;
			}
			if (wanted == Variant::VECTOR3I && values.size() == 3) {
				r_out = Vector3i((int)values[0], (int)values[1], (int)values[2]);
				return true;
			}
			if (wanted == Variant::COLOR && (values.size() == 3 || values.size() == 4)) {
				r_out = Color((double)values[0], (double)values[1], (double)values[2],
						values.size() == 4 ? (double)values[3] : 1.0);
				return true;
			}
		} break;
		default:
			// Anything else is passed through and left to Godot's own conversion.
			r_out = p_incoming;
			return true;
	}

	r_error = vformat("cannot use a %s value for a %s property",
			Variant::get_type_name(p_incoming.get_type()), Variant::get_type_name(wanted));
	return false;
}

class SetScenePropertyTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SetSceneProperty"; }
	virtual String get_description() const override {
		return "Set a property on a node in the edited scene. This is a persistent edit: it "
			   "goes through the editor's undo history and survives once the scene is saved. "
			   "To change the running game instead, use Godot_SetRuntimeProperty.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node path relative to the scene root ('.' is the root).");
		properties["property"] = MCPSchema::string_property("Property name, e.g. 'position' or 'visible'.");
		// Any JSON value: vectors and colours arrive as arrays and are coerced against
		// the property's current type.
		properties["value"] = MCPSchema::any_property("New value. Vectors and colours are arrays, e.g. [128, 64].");
		Vector<String> required;
		required.push_back("path");
		required.push_back("property");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node that was changed.");
		properties["property"] = MCPSchema::string_property("Property that was set.");
		properties["persistent"] = MCPSchema::bool_property("Always true: save the scene to keep it.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		if (!root) {
			r_error.set(MCPToolError::INVALID_STATE, "there is no scene open in the editor");
			return Dictionary();
		}

		const String path = String(p_arguments["path"]).strip_edges();
		Node *node = (path.is_empty() || path == ".") ? root : root->get_node_or_null(NodePath(path));
		if (!node) {
			r_error.set(MCPToolError::NOT_FOUND, vformat("no node at '%s' in the edited scene", path));
			return Dictionary();
		}
		if (node != root && node->get_owner() != root) {
			r_error.set(MCPToolError::INVALID_STATE,
					vformat("'%s' belongs to an instanced sub-scene; edit it in its own scene instead", path));
			return Dictionary();
		}

		const String property = p_arguments["property"];
		bool valid = false;
		const Variant current = node->get(property, &valid);
		if (!valid) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' has no property '%s'", node->get_class(), property));
			return Dictionary();
		}

		Variant value;
		String coercion_error;
		if (!coerce_value(current, p_arguments.has("value") ? p_arguments["value"] : Variant(), value, coercion_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, coercion_error);
			return Dictionary();
		}

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(vformat("Godot AI: set %s.%s", node->get_name(), property));
		undo_redo->add_do_property(node, property, value);
		undo_redo->add_undo_property(node, property, current);
		undo_redo->commit_action();

		Dictionary result;
		result["path"] = String(root->get_path_to(node));
		result["property"] = property;
		result["persistent"] = true;
		return result;
	}
};

// --- runtime -----------------------------------------------------------------

static EditorDebuggerTree *get_remote_tree(MCPToolError &r_error, const String &p_tool) {
	if (!EditorNode::get_singleton() || !EditorDebuggerNode::get_singleton()) {
		r_error.set(MCPToolError::UNSUPPORTED, vformat("'%s' needs a running Godot editor", p_tool));
		return nullptr;
	}
	if (!EditorInterface::get_singleton() || !EditorInterface::get_singleton()->is_playing_scene()) {
		r_error.set(MCPToolError::INVALID_STATE,
				vformat("'%s' needs the game to be running; start it with Godot_PlayCurrentScene", p_tool));
		return nullptr;
	}
	EditorDebuggerTree *tree = EditorDebuggerNode::get_singleton()->get_remote_tree();
	if (!tree) {
		r_error.set(MCPToolError::INVALID_STATE, "the editor has no remote scene tree");
		return nullptr;
	}
	return tree;
}

// The editor only populates its remote scene tree while the Remote panel is visible,
// because that is the only thing that normally asks for it. A tool has to ask
// explicitly, and then wait: the answer arrives from the game with no signal to
// listen for, so it is polled through the deferred-response path.
static bool remote_tree_ready() {
	if (!EditorDebuggerNode::get_singleton()) {
		return false;
	}
	EditorDebuggerTree *tree = EditorDebuggerNode::get_singleton()->get_remote_tree();
	return tree && tree->get_root();
}

static void request_remote_tree() {
	if (EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->request_remote_tree();
	}
}

static void collect_remote_nodes(TreeItem *p_item, int p_depth, int p_max_depth, Array &r_nodes) {
	if (!p_item) {
		return;
	}
	Dictionary entry;
	entry["name"] = p_item->get_text(0);
	entry["depth"] = p_depth;
	r_nodes.push_back(entry);
	if (p_depth >= p_max_depth) {
		return;
	}
	for (TreeItem *child = p_item->get_first_child(); child; child = child->get_next()) {
		collect_remote_nodes(child, p_depth + 1, p_max_depth, r_nodes);
	}
}

class GetRuntimeSceneTreeTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetRuntimeSceneTree"; }
	virtual String get_description() const override {
		return "Return the node tree of the *running* game, as reported by the debugger. "
			   "This is the live tree, which can differ from the edited scene.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["max_depth"] = MCPSchema::integer_property("How deep to descend.", 32);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["nodes"] = MCPSchema::array_property("Nodes in the running game.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["running"] = MCPSchema::bool_property("Always true when this succeeds.");
		return MCPSchema::object_schema(properties);
	}
	Dictionary _build(int p_max_depth) const {
		Array nodes;
		collect_remote_nodes(EditorDebuggerNode::get_singleton()->get_remote_tree()->get_root(),
				0, p_max_depth, nodes);
		Dictionary result;
		result["nodes"] = nodes;
		result["running"] = true;
		return result;
	}

	Variant _poll() {
		if (!remote_tree_ready()) {
			return Variant();
		}
		return _build(pending_max_depth);
	}

	int pending_max_depth = 32;

public:
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		EditorDebuggerTree *tree = get_remote_tree(r_error, get_tool_name());
		if (!tree) {
			return Dictionary();
		}
		if (remote_tree_ready()) {
			return _build((int)p_arguments["max_depth"]);
		}

		// Ask the game for its tree, then answer once it arrives.
		pending_max_depth = (int)p_arguments["max_depth"];
		request_remote_tree();
		return MCPDeferred::make_deferred_result(
				MCPDeferred::begin_polled(10.0, callable_mp(this, &GetRuntimeSceneTreeTool::_poll)));
	}
};

class SetRuntimePropertyTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SetRuntimeProperty"; }
	virtual String get_description() const override {
		return "Set a property on a node in the RUNNING game. The change is NOT persistent: "
			   "it is lost as soon as the game stops, and it never reaches the scene file. "
			   "To make a lasting change, use Godot_SetSceneProperty.";
	}
	// Driving the running game, not editing the project: nothing persistent can come
	// out of this, so it is gated with play-mode rather than as a project mutation.
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property(
				"Node path in the running game, e.g. '/root/Main/Player'.");
		properties["property"] = MCPSchema::string_property("Property name.");
		properties["value"] = MCPSchema::any_property("New value. Vectors and colours are arrays, e.g. [128, 64].");
		Vector<String> required;
		required.push_back("path");
		required.push_back("property");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node that was changed.");
		properties["property"] = MCPSchema::string_property("Property that was set.");
		properties["persistent"] = MCPSchema::bool_property("Always false: this is lost when the game stops.");
		return MCPSchema::object_schema(properties);
	}

	String pending_path;
	String pending_property;
	Variant pending_value;

	// Returns the result once the tree is available; Variant() means "still waiting".
	// An error is reported by returning a result the protocol turns into a failure,
	// because a poller has no error channel of its own.
	Variant _apply() {
		if (!remote_tree_ready()) {
			return Variant();
		}
		EditorDebuggerTree *tree = EditorDebuggerNode::get_singleton()->get_remote_tree();
		const ObjectID id = tree->get_object_id_for_path(pending_path);
		ScriptEditorDebugger *debugger = EditorDebuggerNode::get_singleton()->get_current_debugger();
		if (id.is_null() || !debugger) {
			Dictionary failure;
			failure["error"] = vformat("no node at '%s' in the running game; paths there start at '/root'", pending_path);
			return failure;
		}
		debugger->update_remote_object(id, pending_property, pending_value);

		Dictionary result;
		result["path"] = pending_path;
		result["property"] = pending_property;
		result["persistent"] = false;
		return result;
	}

public:
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		EditorDebuggerTree *tree = get_remote_tree(r_error, get_tool_name());
		if (!tree) {
			return Dictionary();
		}

		pending_path = String(p_arguments["path"]).strip_edges();
		pending_property = p_arguments["property"];
		pending_value = p_arguments.has("value") ? p_arguments["value"] : Variant();

		if (remote_tree_ready()) {
			const Variant applied = _apply();
			const Dictionary result = applied;
			if (result.has("error")) {
				r_error.set(MCPToolError::NOT_FOUND, result["error"]);
				return Dictionary();
			}
			return result;
		}

		request_remote_tree();
		return MCPDeferred::make_deferred_result(
				MCPDeferred::begin_polled(10.0, callable_mp(this, &SetRuntimePropertyTool::_apply)));
	}
};

} // namespace

void mcp_register_property_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(SetScenePropertyTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetRuntimeSceneTreeTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SetRuntimePropertyTool)));
}
