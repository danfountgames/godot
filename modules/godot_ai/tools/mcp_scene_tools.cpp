/**************************************************************************/
/*  mcp_scene_tools.cpp                                                   */
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

#include "../mcp_editor_refresh.h"
#include "../mcp_tool_registry.h"

#include "../mcp_paths.h"

#include "core/io/dir_access.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/variant/array.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

namespace {

// EditorInterface exists in any editor build (register_editor_types() creates it),
// including the headless test binary, but its methods dereference EditorNode. Both
// must be present before any of them may be called.
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

// Resolves a node path relative to the scene root. "." and "" both mean the root.
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

// Structural edits must not reach into an instanced sub-scene: those nodes belong to
// the instance, and changing them here would be lost or would corrupt the instance.
static bool check_editable(Node *p_node, Node *p_root, MCPToolError &r_error) {
	if (p_node == p_root) {
		return true;
	}
	if (p_node->get_owner() != p_root) {
		r_error.set(MCPToolError::INVALID_STATE,
				vformat("'%s' belongs to an instanced sub-scene; edit it in its own scene instead",
						String(p_root->get_path_to(p_node))));
		return false;
	}
	if (p_node->get_internal_mode() != Node::INTERNAL_MODE_DISABLED) {
		// move_child/get_index do not behave for internal nodes.
		r_error.set(MCPToolError::INVALID_STATE,
				vformat("'%s' is an internal node and cannot be restructured",
						String(p_root->get_path_to(p_node))));
		return false;
	}
	return true;
}

static Dictionary node_summary(Node *p_node, Node *p_root) {
	Dictionary summary;
	summary["name"] = p_node->get_name();
	summary["type"] = p_node->get_class();
	summary["path"] = String(p_root->get_path_to(p_node));
	summary["child_count"] = p_node->get_child_count();
	return summary;
}

class ManageNodeTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ManageNode"; }
	virtual String get_description() const override {
		return "Create, delete, rename or reparent a node in the edited scene. Every change "
			   "goes through the editor's undo history, so Godot_UndoLastAction reverts it. "
			   "The scene is not saved automatically - call Godot_SaveScene to persist.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }

	virtual Dictionary get_input_schema() const override {
		Vector<String> actions;
		actions.push_back("create");
		actions.push_back("delete");
		actions.push_back("rename");
		actions.push_back("reparent");

		Dictionary properties;
		properties["action"] = MCPSchema::enum_property("What to do.", actions);
		properties["path"] = MCPSchema::string_property(
				"Target node, relative to the scene root ('.' is the root). Required for "
				"delete, rename and reparent.",
				"");
		properties["parent"] = MCPSchema::string_property(
				"Parent for a new node, relative to the scene root. Used by create.", ".");
		properties["type"] = MCPSchema::string_property(
				"Node class to instantiate. Required for create.", "");
		properties["name"] = MCPSchema::string_property(
				"New node name. Required for rename; optional for create.", "");
		properties["new_parent"] = MCPSchema::string_property(
				"Destination parent. Required for reparent.", "");
		properties["index"] = MCPSchema::integer_property(
				"Position among the parent's children; -1 appends.", -1);

		Vector<String> required;
		required.push_back("action");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property("Action that was performed.");
		properties["node"] = MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
		properties["undo_action"] = MCPSchema::string_property("Name of the undo entry this created.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override;

private:
	Dictionary _create(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error);
	Dictionary _delete(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error);
	Dictionary _rename(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error);
	Dictionary _reparent(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error);
};

Dictionary ManageNodeTool::_create(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error) {
	const String type = String(p_arguments["type"]).strip_edges();
	if (type.is_empty()) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, "create requires a 'type' (a Node class name)");
		return Dictionary();
	}
	if (!ClassDB::class_exists(type)) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("unknown class '%s'", type));
		return Dictionary();
	}
	if (!ClassDB::can_instantiate(type)) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS,
				vformat("'%s' cannot be instantiated (it is abstract or a singleton)", type));
		return Dictionary();
	}
	if (!ClassDB::is_parent_class(type, "Node")) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is not a Node type", type));
		return Dictionary();
	}

	Node *parent = resolve_node(p_root, p_arguments["parent"], r_error);
	if (!parent || !check_editable(parent, p_root, r_error)) {
		return Dictionary();
	}

	Node *child = Object::cast_to<Node>(ClassDB::instantiate(type));
	if (!child) {
		r_error.set(MCPToolError::FAILED, vformat("could not instantiate '%s'", type));
		return Dictionary();
	}

	const String requested_name = String(p_arguments["name"]).strip_edges();
	if (!requested_name.is_empty()) {
		child->set_name(requested_name);
	}
	// Godot appends a suffix when the name is taken; report the name actually used.
	child->set_name(parent->validate_child_name(child));

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	const String action_name = vformat("Godot AI: create %s", type);
	undo_redo->create_action(action_name);
	undo_redo->add_do_method(parent, "add_child", child, true);
	undo_redo->add_do_method(child, "set_owner", p_root);
	const int index = (int)p_arguments["index"];
	if (index >= 0) {
		undo_redo->add_do_method(parent, "move_child", child, index);
	}
	// The new node is only referenced by the undo history until the action is
	// committed; without this it would leak if the action is discarded.
	undo_redo->add_do_reference(child);
	undo_redo->add_undo_method(parent, "remove_child", child);
	undo_redo->commit_action();

	Dictionary result;
	result["action"] = "create";
	result["node"] = node_summary(child, p_root);
	result["undo_action"] = action_name;
	return result;
}

Dictionary ManageNodeTool::_delete(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error) {
	Node *node = resolve_node(p_root, p_arguments["path"], r_error);
	if (!node) {
		return Dictionary();
	}
	if (node == p_root) {
		r_error.set(MCPToolError::INVALID_STATE,
				"the scene root cannot be deleted; close or replace the scene instead");
		return Dictionary();
	}
	if (!check_editable(node, p_root, r_error)) {
		return Dictionary();
	}
	Node *parent = node->get_parent();
	if (!parent) {
		r_error.set(MCPToolError::INVALID_STATE, "that node has no parent");
		return Dictionary();
	}

	const Dictionary summary = node_summary(node, p_root);

	// Everything the node owns loses its owner when it leaves the tree, so undo has
	// to restore each one explicitly or the nodes would come back unsaved.
	List<Node *> owned;
	node->get_owned_by(node->get_owner(), &owned);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	const String action_name = vformat("Godot AI: delete %s", node->get_name());
	undo_redo->create_action(action_name);
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_undo_method(parent, "add_child", node, true);
	undo_redo->add_undo_method(parent, "move_child", node, node->get_index(false));
	for (Node *owned_node : owned) {
		undo_redo->add_undo_method(owned_node, "set_owner", p_root);
	}
	// Keeps the detached node alive for as long as the undo entry exists.
	undo_redo->add_undo_reference(node);
	undo_redo->commit_action();

	Dictionary result;
	result["action"] = "delete";
	result["node"] = summary;
	result["undo_action"] = action_name;
	return result;
}

Dictionary ManageNodeTool::_rename(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error) {
	Node *node = resolve_node(p_root, p_arguments["path"], r_error);
	if (!node || !check_editable(node, p_root, r_error)) {
		return Dictionary();
	}
	const String new_name = String(p_arguments["name"]).strip_edges();
	if (new_name.is_empty()) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, "rename requires a non-empty 'name'");
		return Dictionary();
	}
	// Node names cannot contain the path separators or the reserved characters.
	if (new_name.contains(".") || new_name.contains("/") || new_name.contains(":") ||
			new_name.contains("@") || new_name.contains("%") || new_name.contains("\"")) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS,
				vformat("'%s' is not a valid node name: . / : @ %% and \" are not allowed", new_name));
		return Dictionary();
	}

	const String old_name = node->get_name();
	if (old_name == new_name) {
		Dictionary result;
		result["action"] = "rename";
		result["node"] = node_summary(node, p_root);
		result["undo_action"] = String();
		return result;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	const String action_name = vformat("Godot AI: rename %s to %s", old_name, new_name);
	undo_redo->create_action(action_name);
	undo_redo->add_do_method(node, "set_name", new_name);
	undo_redo->add_undo_method(node, "set_name", old_name);
	undo_redo->commit_action();

	Dictionary result;
	result["action"] = "rename";
	result["node"] = node_summary(node, p_root);
	result["previous_name"] = old_name;
	result["undo_action"] = action_name;
	return result;
}

Dictionary ManageNodeTool::_reparent(Node *p_root, const Dictionary &p_arguments, MCPToolError &r_error) {
	Node *node = resolve_node(p_root, p_arguments["path"], r_error);
	if (!node) {
		return Dictionary();
	}
	if (node == p_root) {
		r_error.set(MCPToolError::INVALID_STATE, "the scene root cannot be reparented");
		return Dictionary();
	}
	if (!check_editable(node, p_root, r_error)) {
		return Dictionary();
	}

	Node *new_parent = resolve_node(p_root, p_arguments["new_parent"], r_error);
	if (!new_parent || !check_editable(new_parent, p_root, r_error)) {
		return Dictionary();
	}
	Node *old_parent = node->get_parent();
	if (!old_parent) {
		r_error.set(MCPToolError::INVALID_STATE, "that node has no parent");
		return Dictionary();
	}
	if (new_parent == node || new_parent->is_greater_than(node)) {
		// is_greater_than() is true when new_parent is a descendant of node.
		r_error.set(MCPToolError::INVALID_ARGUMENTS,
				"a node cannot be reparented into itself or into one of its own descendants");
		return Dictionary();
	}
	if (new_parent == old_parent && (int)p_arguments["index"] < 0) {
		Dictionary result;
		result["action"] = "reparent";
		result["node"] = node_summary(node, p_root);
		result["undo_action"] = String();
		return result;
	}

	const int old_index = node->get_index(false);
	List<Node *> owned;
	node->get_owned_by(node->get_owner(), &owned);

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	const String action_name = vformat("Godot AI: reparent %s", node->get_name());
	undo_redo->create_action(action_name);
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node, true);
	undo_redo->add_do_method(node, "set_owner", p_root);
	const int index = (int)p_arguments["index"];
	if (index >= 0) {
		undo_redo->add_do_method(new_parent, "move_child", node, index);
	}

	undo_redo->add_undo_method(new_parent, "remove_child", node);
	undo_redo->add_undo_method(old_parent, "add_child", node, true);
	undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
	undo_redo->add_undo_method(node, "set_owner", p_root);
	for (Node *owned_node : owned) {
		undo_redo->add_undo_method(owned_node, "set_owner", p_root);
	}
	undo_redo->commit_action();

	Dictionary result;
	result["action"] = "reparent";
	result["node"] = node_summary(node, p_root);
	result["undo_action"] = action_name;
	return result;
}

Dictionary ManageNodeTool::run(const Dictionary &p_arguments, MCPToolError &r_error) {
	Node *root = nullptr;
	if (!require_edited_scene(&root, r_error, get_tool_name())) {
		return Dictionary();
	}

	const String action = p_arguments["action"];
	if (action != "create" && String(p_arguments["path"]).strip_edges().is_empty()) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("%s requires a 'path'", action));
		return Dictionary();
	}

	if (action == "create") {
		return _create(root, p_arguments, r_error);
	}
	if (action == "delete") {
		return _delete(root, p_arguments, r_error);
	}
	if (action == "rename") {
		return _rename(root, p_arguments, r_error);
	}
	if (action == "reparent") {
		return _reparent(root, p_arguments, r_error);
	}

	// Unreachable while the schema's enum and this dispatch agree.
	r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("unknown action '%s'", action));
	return Dictionary();
}

// Undo and redo are exposed so an agent can revert its own structural change, and so
// undo behaviour is testable through the same path a client uses.
class UndoRedoTool : public MCPTool {
	bool is_undo = true;

public:
	explicit UndoRedoTool(bool p_undo) :
			is_undo(p_undo) {}

	virtual String get_tool_name() const override {
		return is_undo ? "Godot_UndoLastAction" : "Godot_RedoLastAction";
	}
	virtual String get_description() const override {
		return is_undo
				? "Undo the most recent editor action, including changes made by other tools."
				: "Redo the most recently undone editor action.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["performed"] = MCPSchema::bool_property("False when there was nothing to undo or redo.");
		properties["action"] = MCPSchema::string_property("Name of the action that was undone or redone.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Node *root = nullptr;
		if (!require_edited_scene(&root, r_error, get_tool_name())) {
			return Dictionary();
		}
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		if (!undo_redo) {
			r_error.set(MCPToolError::UNSUPPORTED, "the editor undo history is not available");
			return Dictionary();
		}

		// Read the name before performing it: afterwards it names a different entry.
		const String action_name = is_undo ? undo_redo->get_current_action_name() : String();
		const bool performed = is_undo ? undo_redo->undo() : undo_redo->redo();

		Dictionary result;
		result["performed"] = performed;
		result["action"] = action_name;
		return result;
	}
};


// Creating a scene file.
//
// Godot_ManageNode edits the scene the editor already has open; it cannot bring one into
// existence. That left a real contradiction in the instructions an agent is given: it is
// told never to hand-write a `.tscn` when a structured API exists, and then has no
// structured way to make the first one - so it writes a two-line stub as text, which is
// precisely the thing that goes stale when the format changes.
class CreateSceneTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CreateScene"; }
	virtual String get_description() const override {
		return "Create a new scene file with a root node of the class you name, and open it. "
			   "This is how a scene comes into existence; Godot_ManageNode then builds it up. "
			   "Never hand-write a .tscn as text - the format is the engine's business, and a "
			   "stub written by hand is a stub that rots.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_SCENE; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Where to create it, as a res:// path ending in .tscn.");
		properties["root_type"] = MCPSchema::string_property(
				"Class of the root node, such as Node2D, Control or Node3D.", "Node2D");
		properties["root_name"] = MCPSchema::string_property(
				"Name of the root node. Defaults to the file name.");
		properties["open"] = MCPSchema::bool_property(
				"Open the scene in the editor afterwards, so Godot_ManageNode can build it.", true);
		properties["overwrite"] = MCPSchema::bool_property(
				"Replace the file if it already exists.", false);
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Scene that was created.");
		properties["root_name"] = MCPSchema::string_property("Name of the root node.");
		properties["root_type"] = MCPSchema::string_property("Class of the root node.");
		properties["opened"] = MCPSchema::bool_property("True when it is now the edited scene.");
		return MCPSchema::object_schema(properties);
	}

	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		// has() first: subscripting a const Dictionary inserts a null for a missing key.
		if (p_arguments.has("path")) {
			const String path = String(p_arguments["path"]).strip_edges();
			if (!path.is_empty()) {
				paths.push_back(path);
			}
		}
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "'Godot_CreateScene' needs a running Godot editor");
			return Dictionary();
		}

		MCPPaths::Resolved resolved;
		String path_error;
		if (!MCPPaths::resolve(p_arguments["path"], resolved, path_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, path_error);
			return Dictionary();
		}
		if (resolved.res_path.get_extension().to_lower() != "tscn") {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' must end in .tscn", resolved.res_path));
			return Dictionary();
		}
		if (resolved.exists && !(bool)p_arguments.get("overwrite", false)) {
			r_error.set(MCPToolError::INVALID_STATE,
					vformat("'%s' already exists; pass overwrite to replace it", resolved.res_path));
			return Dictionary();
		}

		const String root_type = String(p_arguments.get("root_type", "Node2D")).strip_edges();
		if (!ClassDB::class_exists(root_type)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("there is no class called '%s'", root_type));
			return Dictionary();
		}
		if (!ClassDB::is_parent_class(root_type, "Node")) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' is not a Node, so it cannot be a scene root", root_type));
			return Dictionary();
		}
		if (!ClassDB::can_instantiate(root_type)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' is abstract and cannot be instantiated", root_type));
			return Dictionary();
		}

		Node *root = Object::cast_to<Node>(ClassDB::instantiate(root_type));
		if (!root) {
			r_error.set(MCPToolError::FAILED, vformat("could not create a '%s'", root_type));
			return Dictionary();
		}
		String root_name = String(p_arguments.get("root_name", String())).strip_edges();
		if (root_name.is_empty()) {
			root_name = resolved.res_path.get_file().get_basename();
		}
		root->set_name(root_name);

		Ref<PackedScene> scene;
		scene.instantiate();
		const Error packed = scene->pack(root);
		// The root is only a template for the packed resource; the scene owns nothing of
		// it once packed, so it has to be freed either way.
		memdelete(root);
		if (packed != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not pack a '%s' into a scene", root_type));
			return Dictionary();
		}

		Ref<DirAccess> directory = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (directory.is_valid()) {
			directory->make_dir_recursive(resolved.absolute.get_base_dir());
		}
		if (ResourceSaver::save(scene, resolved.res_path) != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not write '%s'", resolved.res_path));
			return Dictionary();
		}
		// Not a null check: the singleton can exist outside the tree. See mcp_editor_refresh.h.
		if (MCPEditorRefresh::can_refresh()) {
			MCPEditorRefresh::update_file(resolved.res_path);
		}

		bool opened = false;
		if ((bool)p_arguments.get("open", true)) {
			EditorInterface::get_singleton()->open_scene_from_path(resolved.res_path);
			Node *edited = EditorInterface::get_singleton()->get_edited_scene_root();
			opened = edited != nullptr;
		}

		Dictionary result;
		result["path"] = resolved.res_path;
		result["root_name"] = root_name;
		result["root_type"] = root_type;
		result["opened"] = opened;
		return result;
	}
};

} // namespace

void mcp_register_scene_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(CreateSceneTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ManageNodeTool)));
	registry->register_tool(Ref<MCPTool>(memnew(UndoRedoTool(true))));
	registry->register_tool(Ref<MCPTool>(memnew(UndoRedoTool(false))));
}
