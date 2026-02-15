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

#include "mcp_scene_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

// ============================================================================
// Main-thread dispatch for scene mutation tools
// ============================================================================
//
// Scene mutation tools (add_node, remove_node, set_property, etc.) use
// EditorUndoRedoManager which calls add_child/remove_child on the scene tree.
// These operations MUST run on the main thread. When the MCP server runs in
// threaded mode (use_thread=true, the default), tool handlers execute on the
// poll thread. This causes silent failures:
//   ERROR: Adding children to a node inside the SceneTree is only allowed
//          from the main thread. Use call_deferred("add_child", node).
//
// Fix: Use the same deferred semaphore pattern as mcp_editor_tools.cpp.
// The poll thread defers the actual work to the main thread via
// call_deferred() and blocks on a semaphore until the main thread completes.

static Semaphore _scene_deferred_semaphore;
static Dictionary _scene_deferred_result;
static Dictionary _scene_deferred_args;

// Type alias for scene tool handler functions.
typedef Dictionary (*SceneToolHandler)(const Dictionary &);

// Currently dispatched handler (only one request at a time on the poll thread).
static SceneToolHandler _scene_deferred_handler = nullptr;

// Main-thread trampoline: executes the stored handler and posts the semaphore.
void MCPSceneTools::_do_scene_tool_main() {
	_scene_deferred_result = _scene_deferred_handler(_scene_deferred_args);
	_scene_deferred_semaphore.post();
}

// Wrapper that ensures a scene tool handler runs on the main thread.
// If already on the main thread, calls directly. Otherwise, defers and waits.
Dictionary MCPSceneTools::_run_on_main_thread(SceneToolHandler p_handler, const Dictionary &p_args) {
	if (Thread::is_main_thread()) {
		return p_handler(p_args);
	}

	// Store the handler and args for the main-thread trampoline.
	_scene_deferred_handler = p_handler;
	_scene_deferred_args = p_args;

	callable_mp_static(&MCPSceneTools::_do_scene_tool_main)
			.call_deferred();
	_scene_deferred_semaphore.wait();

	// Clear references to allow garbage collection.
	_scene_deferred_args = Dictionary();
	_scene_deferred_handler = nullptr;

	return _scene_deferred_result;
}

// ============================================================================
// Helpers
// ============================================================================

Node *MCPSceneTools::_get_scene_root() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return nullptr;
	}
	return ei->get_edited_scene_root();
}

Node *MCPSceneTools::_find_node(const String &p_path) {
	Node *root = _get_scene_root();
	if (!root) {
		return nullptr;
	}
	if (p_path.is_empty()) {
		return root;
	}
	return root->get_node_or_null(NodePath(p_path));
}

// ============================================================================
// Helper: count all nodes in the subtree (including p_node itself)
// ============================================================================

static int _count_nodes_recursive(Node *p_node) {
	int count = 1;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		count += _count_nodes_recursive(p_node->get_child(i));
	}
	return count;
}

// ============================================================================
// Helper: _format_tree_recursive
//
// Builds both a visual tree string and a structured Dictionary simultaneously.
// The structured dict has: {name, type, script, children[]}.
// ============================================================================

void MCPSceneTools::_format_tree_recursive(Node *p_node, String &r_text,
		Dictionary &r_structured, const String &p_prefix, bool p_is_last,
		int p_depth, int p_max_depth) {
	// Build the visual line for this node.
	String connector = p_depth == 0 ? "" : (p_is_last ? String::utf8("\u2514\u2500\u2500 ") : String::utf8("\u251C\u2500\u2500 "));
	String line = p_prefix + connector;

	// Node name and type.
	line += p_node->get_name();
	line += " (" + p_node->get_class() + ")";

	// Script indicator.
	Ref<Script> script = p_node->get_script();
	String script_path;
	if (script.is_valid() && !script->get_path().is_empty()) {
		script_path = script->get_path();
		line += " [" + script_path + "]";
	}

	r_text += line + "\n";

	// Build structured entry.
	r_structured["name"] = p_node->get_name();
	r_structured["type"] = p_node->get_class();
	if (!script_path.is_empty()) {
		r_structured["script"] = script_path;
	}

	// Recurse into children if within depth limit.
	Array children_structured;
	int child_count = p_node->get_child_count();

	if (p_depth < p_max_depth && child_count > 0) {
		String child_prefix = p_prefix;
		if (p_depth > 0) {
			child_prefix += p_is_last ? "    " : String::utf8("\u2502   ");
		}

		for (int i = 0; i < child_count; i++) {
			Node *child = p_node->get_child(i);
			bool is_last_child = (i == child_count - 1);

			Dictionary child_dict;
			_format_tree_recursive(child, r_text, child_dict, child_prefix,
					is_last_child, p_depth + 1, p_max_depth);
			children_structured.push_back(child_dict);
		}
	} else if (child_count > 0 && p_depth >= p_max_depth) {
		// Indicate truncated children.
		String child_prefix = p_prefix;
		if (p_depth > 0) {
			child_prefix += p_is_last ? "    " : String::utf8("\u2502   ");
		}
		r_text += child_prefix + String::utf8("\u2514\u2500\u2500 ") +
				"... (" + itos(child_count) + " children, use max_depth to see more)\n";
	}

	if (children_structured.size() > 0) {
		r_structured["children"] = children_structured;
	}
	r_structured["child_count"] = child_count;
}

// ============================================================================
// Helper: _build_node_detail
//
// Builds a detailed view of a single node including properties, transform,
// signals, groups, and script information.
// ============================================================================

Dictionary MCPSceneTools::_build_node_detail(Node *p_node, const String &p_categories,
		bool p_include_defaults) {
	// Parse categories filter.
	bool show_all = (p_categories.is_empty() || p_categories == "all");
	HashSet<String> categories;
	if (!show_all) {
		Vector<String> parts = p_categories.split(",");
		for (int i = 0; i < parts.size(); i++) {
			categories.insert(parts[i].strip_edges().to_lower());
		}
	}

	auto want = [&](const String &s) { return show_all || categories.has(s); };

	String text;
	Dictionary structured;

	// --- Header ---
	String node_name = p_node->get_name();
	String node_class = p_node->get_class();
	String node_path = String(p_node->get_path());

	Ref<Script> script = p_node->get_script();
	String script_path;
	if (script.is_valid() && !script->get_path().is_empty()) {
		script_path = script->get_path();
	}

	int child_count = p_node->get_child_count();

	text += "# " + node_name + " (" + node_class + ")\n";
	text += "Path: " + node_path + "\n";
	if (!script_path.is_empty()) {
		text += "Script: " + script_path + "\n";
	}
	text += "Children: " + itos(child_count) + "\n\n";

	structured["name"] = node_name;
	structured["type"] = node_class;
	structured["path"] = node_path;
	if (!script_path.is_empty()) {
		structured["script"] = script_path;
	}
	structured["child_count"] = child_count;

	// --- Properties ---
	if (want("properties") || want("transform") || want("script")) {
		List<PropertyInfo> plist;
		p_node->get_property_list(&plist);

		Dictionary props_structured;
		Dictionary transform_structured;
		Dictionary script_vars_structured;
		String props_text;
		String transform_text;
		String script_text;

		for (const PropertyInfo &pi : plist) {
			// Skip internal/private properties.
			if (pi.name.begins_with("_")) {
				continue;
			}

			// Must have PROPERTY_USAGE_STORAGE to be meaningful.
			if (!(pi.usage & PROPERTY_USAGE_STORAGE)) {
				continue;
			}

			// Skip editor-only properties that aren't stored.
			if (!(pi.usage & (PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR))) {
				continue;
			}

			Variant value = p_node->get(pi.name);

			// Check if it matches the class default (skip if it does, unless include_defaults).
			if (!p_include_defaults) {
				bool is_default = false;
				Variant default_val;

				// Check class defaults via ClassDB.
				bool valid = false;
				default_val = ClassDB::class_get_default_property_value(p_node->get_class(), pi.name, &valid);
				if (valid && value == default_val) {
					is_default = true;
				}

				if (is_default) {
					continue;
				}
			}

			// Convert value to string for text output.
			String val_str = String(value);

			// Truncate very long values.
			if (val_str.length() > 200) {
				val_str = val_str.substr(0, 200) + "...";
			}

			// Determine category.
			bool is_script_var = (pi.usage & PROPERTY_USAGE_SCRIPT_VARIABLE);
			bool is_transform = (pi.name == "position" || pi.name == "rotation" ||
					pi.name == "scale" || pi.name == "transform" ||
					pi.name == "global_position" || pi.name == "global_rotation" ||
					pi.name == "global_transform" || pi.name == "global_scale" ||
					pi.name == "rotation_degrees" || pi.name == "skew" ||
					pi.name == "basis" || pi.name == "quaternion");

			if (is_transform && want("transform")) {
				transform_text += "  " + pi.name + " = " + val_str + "\n";
				transform_structured[pi.name] = value;
			} else if (is_script_var && want("script")) {
				script_text += "  " + pi.name + ": " + Variant::get_type_name(pi.type) + " = " + val_str + "\n";
				Dictionary var_info;
				var_info["type"] = Variant::get_type_name(pi.type);
				var_info["value"] = value;
				script_vars_structured[pi.name] = var_info;
			} else if (want("properties") && !is_transform) {
				props_text += "  " + pi.name + ": " + Variant::get_type_name(pi.type) + " = " + val_str + "\n";
				Dictionary prop_info;
				prop_info["type"] = Variant::get_type_name(pi.type);
				prop_info["value"] = value;
				props_structured[pi.name] = prop_info;
			}
		}

		if (want("properties") && !props_text.is_empty()) {
			text += "## Properties\n" + props_text + "\n";
			structured["properties"] = props_structured;
		}

		if (want("transform") && !transform_text.is_empty()) {
			text += "## Transform\n" + transform_text + "\n";
			structured["transform"] = transform_structured;
		}

		if (want("script") && !script_text.is_empty()) {
			text += "## Script Variables\n" + script_text + "\n";
			structured["script_variables"] = script_vars_structured;
		}
	}

	// --- Signal Connections ---
	if (want("signals")) {
		List<Object::Connection> connections;
		p_node->get_all_signal_connections(&connections);

		if (connections.size() > 0) {
			String signals_text;
			Array signals_structured;

			for (const Object::Connection &conn : connections) {
				String sig_name = conn.signal.get_name();
				Object *target_obj = conn.callable.get_object();
				String target_path;
				if (target_obj) {
					Node *target_node = Object::cast_to<Node>(target_obj);
					if (target_node) {
						target_path = String(target_node->get_path());
					} else {
						target_path = "<non-node object>";
					}
				} else {
					target_path = "<null>";
				}
				String method_name = conn.callable.get_method();

				signals_text += "  " + sig_name + " -> " + target_path + "::" + method_name + "\n";

				Dictionary conn_dict;
				conn_dict["signal"] = sig_name;
				conn_dict["target"] = target_path;
				conn_dict["method"] = method_name;
				signals_structured.push_back(conn_dict);
			}

			text += "## Signal Connections\n" + signals_text + "\n";
			structured["signal_connections"] = signals_structured;
		} else {
			if (show_all || categories.has("signals")) {
				text += "## Signal Connections\n  (none)\n\n";
			}
		}

		// Also list available signals (not connected, just declared).
		List<MethodInfo> signal_list;
		p_node->get_signal_list(&signal_list);

		if (signal_list.size() > 0) {
			String available_text;
			Array available_structured;

			for (const MethodInfo &mi : signal_list) {
				String sig_line = "  " + mi.name + "(";
				for (int a = 0; a < mi.arguments.size(); a++) {
					if (a > 0) {
						sig_line += ", ";
					}
					sig_line += mi.arguments[a].name + ": " + Variant::get_type_name(mi.arguments[a].type);
				}
				sig_line += ")\n";
				available_text += sig_line;

				Dictionary sig_dict;
				sig_dict["name"] = mi.name;
				Array args;
				for (int a = 0; a < mi.arguments.size(); a++) {
					Dictionary arg_dict;
					arg_dict["name"] = mi.arguments[a].name;
					arg_dict["type"] = Variant::get_type_name(mi.arguments[a].type);
					args.push_back(arg_dict);
				}
				sig_dict["arguments"] = args;
				available_structured.push_back(sig_dict);
			}

			text += "## Available Signals\n" + available_text + "\n";
			structured["available_signals"] = available_structured;
		}
	}

	// --- Groups ---
	if (want("groups")) {
		List<Node::GroupInfo> groups;
		p_node->get_groups(&groups);

		if (groups.size() > 0) {
			String groups_text;
			Array groups_structured;

			for (const Node::GroupInfo &gi : groups) {
				// Skip internal groups (prefixed with _).
				if (String(gi.name).begins_with("_")) {
					continue;
				}
				groups_text += "  " + String(gi.name);
				if (gi.persistent) {
					groups_text += " (persistent)";
				}
				groups_text += "\n";

				Dictionary group_dict;
				group_dict["name"] = gi.name;
				group_dict["persistent"] = gi.persistent;
				groups_structured.push_back(group_dict);
			}

			if (!groups_text.is_empty()) {
				text += "## Groups\n" + groups_text + "\n";
				structured["groups"] = groups_structured;
			}
		}
	}

	// --- Children list (always shown for context) ---
	if (child_count > 0) {
		String children_text;
		Array children_structured;

		for (int i = 0; i < child_count; i++) {
			Node *child = p_node->get_child(i);
			String child_line = "  " + String(child->get_name()) + " (" + child->get_class() + ")";
			Ref<Script> child_script = child->get_script();
			if (child_script.is_valid() && !child_script->get_path().is_empty()) {
				child_line += " [" + child_script->get_path() + "]";
			}
			children_text += child_line + "\n";

			Dictionary child_dict;
			child_dict["name"] = child->get_name();
			child_dict["type"] = child->get_class();
			if (child_script.is_valid() && !child_script->get_path().is_empty()) {
				child_dict["script"] = child_script->get_path();
			}
			children_structured.push_back(child_dict);
		}

		text += "## Children\n" + children_text;
		structured["children"] = children_structured;
	}

	// Package the result.
	Dictionary result;
	result["text"] = text;
	result["structured"] = structured;
	return result;
}

// ============================================================================
// Helper: _coerce_json_to_variant
//
// Converts a JSON value (from MCP arguments) to a typed Godot Variant.
// Used by mutation tools (set_property, etc.) in later phases.
// ============================================================================

Variant MCPSceneTools::_coerce_json_to_variant(const Variant &p_json_value,
		Variant::Type p_target_type) {
	if (p_target_type == Variant::NIL) {
		return p_json_value;
	}

	switch (p_target_type) {
		case Variant::BOOL: {
			return p_json_value.operator bool();
		}
		case Variant::INT: {
			return p_json_value.operator int64_t();
		}
		case Variant::FLOAT: {
			return p_json_value.operator double();
		}
		case Variant::STRING: {
			return p_json_value.operator String();
		}
		case Variant::NODE_PATH: {
			return NodePath(p_json_value.operator String());
		}
		case Variant::VECTOR2: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 2) {
					return Vector2(arr[0].operator double(), arr[1].operator double());
				}
			}
			return p_json_value;
		}
		case Variant::VECTOR2I: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 2) {
					return Vector2i(arr[0].operator int64_t(), arr[1].operator int64_t());
				}
			}
			return p_json_value;
		}
		case Variant::VECTOR3: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 3) {
					return Vector3(arr[0].operator double(), arr[1].operator double(), arr[2].operator double());
				}
			}
			return p_json_value;
		}
		case Variant::VECTOR3I: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 3) {
					return Vector3i(arr[0].operator int64_t(), arr[1].operator int64_t(), arr[2].operator int64_t());
				}
			}
			return p_json_value;
		}
		case Variant::VECTOR4: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 4) {
					return Vector4(arr[0].operator double(), arr[1].operator double(),
							arr[2].operator double(), arr[3].operator double());
				}
			}
			return p_json_value;
		}
		case Variant::COLOR: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 4) {
					return Color(arr[0].operator double(), arr[1].operator double(),
							arr[2].operator double(), arr[3].operator double());
				} else if (arr.size() == 3) {
					return Color(arr[0].operator double(), arr[1].operator double(),
							arr[2].operator double(), 1.0);
				}
			} else if (p_json_value.get_type() == Variant::STRING) {
				// Support color names or hex strings like "#ff0000".
				return Color::html(p_json_value.operator String());
			}
			return p_json_value;
		}
		case Variant::RECT2: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 4) {
					return Rect2(arr[0].operator double(), arr[1].operator double(),
							arr[2].operator double(), arr[3].operator double());
				}
			}
			return p_json_value;
		}
		case Variant::RECT2I: {
			if (p_json_value.get_type() == Variant::ARRAY) {
				Array arr = p_json_value;
				if (arr.size() >= 4) {
					return Rect2i(arr[0].operator int64_t(), arr[1].operator int64_t(),
							arr[2].operator int64_t(), arr[3].operator int64_t());
				}
			}
			return p_json_value;
		}
		default: {
			// For other types, return as-is and let Godot's internal conversion handle it.
			return p_json_value;
		}
	}
}

// ============================================================================
// Tool: scene/browse_tree (Phase 1 -- fully implemented)
//
// 2-tier tool:
//   - Coarse mode (no node_path): Visual tree of the entire edited scene.
//   - Detail mode (node_path set): Detailed property/signal/group view of
//     a specific node.
// ============================================================================

Dictionary MCPSceneTools::handle_browse_tree(const Dictionary &p_args) {
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error(
				"No scene is currently open in the editor.\n\n"
				"Open a scene first (File > Open Scene or File > New Scene), then try again.");
	}

	String node_path = String(p_args.get("node_path", "")).strip_edges();

	// ---- Detail mode: specific node ----
	if (!node_path.is_empty()) {
		Node *target = _find_node(node_path);
		if (!target) {
			// Try to provide helpful context: list children of the closest existing parent.
			String suggestion;

			// Walk up the path to find the closest ancestor that exists.
			String search_path = node_path;
			while (!search_path.is_empty()) {
				int last_slash = search_path.rfind("/");
				if (last_slash == -1) {
					// We're at the root-relative level.
					search_path = "";
					break;
				}
				search_path = search_path.substr(0, last_slash);
				Node *ancestor = _find_node(search_path);
				if (ancestor) {
					suggestion = "\n\nClosest existing ancestor: " + search_path;
					suggestion += "\nIts children:";
					for (int i = 0; i < ancestor->get_child_count(); i++) {
						suggestion += "\n  - " + String(ancestor->get_child(i)->get_name());
					}
					break;
				}
			}

			// If nothing found, list root's children.
			if (suggestion.is_empty()) {
				suggestion = "\n\nScene root children:";
				for (int i = 0; i < scene_root->get_child_count(); i++) {
					suggestion += "\n  - " + String(scene_root->get_child(i)->get_name());
				}
			}

			return make_tool_error(
					"Node not found: " + node_path + "\n\n"
					"Path must be relative to the scene root ('" + String(scene_root->get_name()) + "'). "
					"Use empty node_path first to see the full tree, then use a path like "
					"'ChildName' or 'ChildName/GrandChild'." +
					suggestion);
		}

		String categories = String(p_args.get("categories", "all")).strip_edges();
		bool include_defaults = (bool)p_args.get("include_defaults", false);

		Dictionary detail_result = _build_node_detail(target, categories, include_defaults);
		String text = detail_result["text"];
		Dictionary structured = detail_result["structured"];

		return make_tool_result(text, structured);
	}

	// ---- Coarse mode: full tree ----
	int max_depth = CLAMP((int)p_args.get("max_depth", 3), 1, 20);

	// Count total nodes for the header.
	int total_nodes = _count_nodes_recursive(scene_root);

	// Get scene file path.
	String scene_path = scene_root->get_scene_file_path();
	if (scene_path.is_empty()) {
		scene_path = "<unsaved>";
	}

	// Build the tree.
	String text;
	text += "# Scene: " + scene_path + " (" + itos(total_nodes) + " nodes)\n\n";

	Dictionary root_structured;
	_format_tree_recursive(scene_root, text, root_structured, "", true, 0, max_depth);

	// Add usage hint.
	text += "\n---\n";
	text += "Use node_path to inspect a specific node in detail. ";
	text += "Paths are relative to the scene root ('" + String(scene_root->get_name()) + "').\n";
	text += "Examples: node_path=\"\" (root itself), node_path=\"Player\", node_path=\"UI/HealthBar\"\n";

	Dictionary structured;
	structured["scene_path"] = scene_path;
	structured["total_nodes"] = total_nodes;
	structured["max_depth"] = max_depth;
	structured["tree"] = root_structured;

	return make_tool_result(text, structured);
}

// ============================================================================
// Mutation tool handlers
// ============================================================================

Dictionary MCPSceneTools::handle_set_property(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_set_property_impl, p_args);
}

Dictionary MCPSceneTools::_handle_set_property_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	String property = String(p_args.get("property", "")).strip_edges();
	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}
	if (property.is_empty()) {
		return make_tool_error("Missing required parameter: property");
	}
	if (!p_args.has("value")) {
		return make_tool_error("Missing required parameter: value");
	}
	Variant json_value = p_args["value"];

	// Find the target node.
	Node *node = _find_node(node_path);
	if (!node) {
		Node *root = _get_scene_root();
		if (!root) {
			return make_tool_error("No scene is currently open in the editor.");
		}
		return make_tool_error(vformat(
				"Node not found: %s\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(root->get_name())));
	}

	// Find the property in the node's property list to get its type.
	List<PropertyInfo> plist;
	node->get_property_list(&plist);

	bool property_found = false;
	PropertyInfo target_pi;
	for (const PropertyInfo &pi : plist) {
		if (pi.name == property) {
			property_found = true;
			target_pi = pi;
			break;
		}
	}

	if (!property_found) {
		return make_tool_error(vformat(
				"Property '%s' not found on node '%s' (type: %s).\n\n"
				"Use scene/browse_tree with node_path='%s' and include_defaults=true "
				"to see all available properties.",
				property, node_path, node->get_class(), node_path));
	}

	// Get the old value for undo.
	Variant old_value = node->get(property);

	// Coerce the JSON value to the correct Variant type.
	Variant new_value = _coerce_json_to_variant(json_value, target_pi.type);

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action(vformat("Set %s on %s", property, String(node->get_name())));
	undo_redo->add_do_property(node, property, new_value);
	undo_redo->add_undo_property(node, property, old_value);
	undo_redo->commit_action();

	// Build response.
	String text = vformat("Set '%s' on '%s' (%s)\n  Old: %s\n  New: %s",
			property, node_path, node->get_class(),
			String(old_value), String(new_value));

	Dictionary structured;
	structured["node_path"] = node_path;
	structured["node_type"] = node->get_class();
	structured["property"] = property;
	structured["old_value"] = old_value;
	structured["new_value"] = new_value;
	structured["property_type"] = Variant::get_type_name(target_pi.type);

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_add_node(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_add_node_impl, p_args);
}

Dictionary MCPSceneTools::_handle_add_node_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String type = String(p_args.get("type", "")).strip_edges();
	if (type.is_empty()) {
		return make_tool_error("Missing required parameter: type");
	}

	// Validate the type exists and is a Node subclass.
	if (!ClassDB::class_exists(type)) {
		return make_tool_error(vformat(
				"Unknown class: '%s'\n\n"
				"The class must be a valid Godot class name (e.g., 'Sprite2D', "
				"'CharacterBody3D', 'Label', 'Node2D').",
				type));
	}
	if (!ClassDB::is_parent_class(type, "Node")) {
		return make_tool_error(vformat(
				"Class '%s' is not a Node subclass.\n\n"
				"Only Node-derived classes can be added to the scene tree.",
				type));
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find parent node.
	String parent_path = String(p_args.get("parent_path", "")).strip_edges();
	Node *parent = parent_path.is_empty() ? scene_root : _find_node(parent_path);
	if (!parent) {
		return make_tool_error(vformat(
				"Parent node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				parent_path, String(scene_root->get_name())));
	}

	// Instantiate the node.
	Object *obj = ClassDB::instantiate(type);
	if (!obj) {
		return make_tool_error(vformat(
				"Failed to instantiate class '%s'. It may be abstract or not instantiable.",
				type));
	}
	Node *new_node = Object::cast_to<Node>(obj);
	if (!new_node) {
		memdelete(obj);
		return make_tool_error(vformat(
				"Failed to cast instantiated '%s' to Node.", type));
	}

	// Set the name if provided.
	String name = String(p_args.get("name", "")).strip_edges();
	if (!name.is_empty()) {
		new_node->set_name(name);
	}

	// Add the node via undo/redo.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		memdelete(new_node);
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action(vformat("Add %s", type));
	undo_redo->add_do_method(parent, "add_child", new_node);
	undo_redo->add_do_method(new_node, "set_owner", scene_root);
	undo_redo->add_do_reference(new_node);
	undo_redo->add_undo_method(parent, "remove_child", new_node);
	undo_redo->commit_action();

	// Set optional properties after adding to the tree.
	if (p_args.has("properties")) {
		Variant props_var = p_args["properties"];
		if (props_var.get_type() == Variant::DICTIONARY) {
			Dictionary props = props_var;
			LocalVector<Variant> keys = props.get_key_list();
			for (const Variant &key : keys) {
				String prop_name = key.operator String();
				Variant prop_value = props[key];

				// Find the property type for coercion.
				List<PropertyInfo> plist;
				new_node->get_property_list(&plist);
				Variant::Type prop_type = Variant::NIL;
				for (const PropertyInfo &pi : plist) {
					if (pi.name == prop_name) {
						prop_type = pi.type;
						break;
					}
				}

				Variant coerced = _coerce_json_to_variant(prop_value, prop_type);
				new_node->set(prop_name, coerced);
			}
		}
	}

	// Build response.
	String actual_name = String(new_node->get_name());
	String node_path_str = parent_path.is_empty() ? actual_name : (parent_path + "/" + actual_name);

	String text = vformat("Added %s node '%s' under '%s'\nPath: %s",
			type, actual_name,
			parent_path.is_empty() ? String(scene_root->get_name()) : parent_path,
			node_path_str);

	Dictionary structured;
	structured["type"] = type;
	structured["name"] = actual_name;
	structured["parent_path"] = parent_path.is_empty() ? String(scene_root->get_name()) : parent_path;
	structured["node_path"] = node_path_str;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_remove_node(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_remove_node_impl, p_args);
}

Dictionary MCPSceneTools::_handle_remove_node_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find the target node.
	Node *node = _find_node(node_path);
	if (!node) {
		return make_tool_error(vformat(
				"Node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(scene_root->get_name())));
	}

	// Cannot remove the scene root.
	if (node == scene_root) {
		return make_tool_error(
				"Cannot remove the scene root node.\n\n"
				"To replace the root, create a new scene instead.");
	}

	// Get parent and child index for undo.
	Node *parent = node->get_parent();
	if (!parent) {
		return make_tool_error("Node has no parent (unexpected state).");
	}
	int index = node->get_index();
	String node_name = String(node->get_name());
	String node_class = node->get_class();
	int child_count = _count_nodes_recursive(node);

	// Remove via undo/redo.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action(vformat("Remove %s", node_name));
	undo_redo->add_do_method(parent, "remove_child", node);
	undo_redo->add_undo_method(parent, "add_child", node);
	undo_redo->add_undo_method(parent, "move_child", node, index);
	undo_redo->add_undo_method(node, "set_owner", scene_root);
	undo_redo->add_undo_reference(node);
	undo_redo->commit_action();

	// Build response.
	String text = vformat("Removed '%s' (%s) from the scene.\n"
						   "  Was child of: %s\n"
						   "  Nodes removed: %d (including children)\n\n"
						   "This can be undone via Edit > Undo in the editor.",
			node_name, node_class,
			node_path.get_base_dir().is_empty() ? String(scene_root->get_name()) : node_path.get_base_dir(),
			child_count);

	Dictionary structured;
	structured["removed_node"] = node_name;
	structured["removed_type"] = node_class;
	structured["node_path"] = node_path;
	structured["child_index"] = index;
	structured["total_nodes_removed"] = child_count;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_rename_node(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_rename_node_impl, p_args);
}

Dictionary MCPSceneTools::_handle_rename_node_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	String new_name = String(p_args.get("new_name", "")).strip_edges();
	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}
	if (new_name.is_empty()) {
		return make_tool_error("Missing required parameter: new_name");
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find the target node.
	Node *node = _find_node(node_path);
	if (!node) {
		return make_tool_error(vformat(
				"Node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(scene_root->get_name())));
	}

	// Cannot rename the scene root.
	if (node == scene_root) {
		return make_tool_error(
				"Cannot rename the scene root node.\n\n"
				"The scene root name is tied to the scene file. "
				"Use scene/save with a different path instead.");
	}

	// Store old name for undo.
	String old_name = String(node->get_name());

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Rename " + old_name + " to " + new_name);
	undo_redo->add_do_method(node, "set_name", new_name);
	undo_redo->add_undo_method(node, "set_name", old_name);
	undo_redo->commit_action();

	// Read back actual name (Godot may auto-suffix for uniqueness).
	String actual_name = String(node->get_name());

	// Build response.
	String text = vformat("Renamed '%s' to '%s'", old_name, actual_name);
	if (actual_name != new_name) {
		text += vformat(" (requested '%s', Godot adjusted for uniqueness)", new_name);
	}

	Dictionary structured;
	structured["old_name"] = old_name;
	structured["new_name"] = actual_name;
	structured["requested_name"] = new_name;
	structured["node_path"] = node_path;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_move_node(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_move_node_impl, p_args);
}

Dictionary MCPSceneTools::_handle_move_node_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	String new_parent_path = String(p_args.get("new_parent_path", "")).strip_edges();
	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}

	int index = (int)p_args.get("index", -1);

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find the target node.
	Node *node = _find_node(node_path);
	if (!node) {
		return make_tool_error(vformat(
				"Node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(scene_root->get_name())));
	}

	// Cannot move the scene root.
	if (node == scene_root) {
		return make_tool_error(
				"Cannot move the scene root node.\n\n"
				"The scene root is the top-level node and cannot be reparented.");
	}

	// Find the new parent.
	Node *new_parent = new_parent_path.is_empty() ? scene_root : _find_node(new_parent_path);
	if (!new_parent) {
		return make_tool_error(vformat(
				"New parent node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				new_parent_path, String(scene_root->get_name())));
	}

	// Check for cycle: cannot move a node to be a child of itself or its descendants.
	Node *check = new_parent;
	while (check) {
		if (check == node) {
			return make_tool_error(vformat(
					"Cannot move '%s' under '%s': would create a cycle.\n\n"
					"A node cannot become a descendant of itself.",
					node_path, new_parent_path.is_empty() ? String(scene_root->get_name()) : new_parent_path));
		}
		check = check->get_parent();
	}

	// Store old parent and index for undo.
	Node *old_parent = node->get_parent();
	int old_index = node->get_index();
	String node_name = String(node->get_name());
	String old_parent_path = old_parent == scene_root ? String(scene_root->get_name()) : String(old_parent->get_path()).replace(String(scene_root->get_path()) + "/", "");

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Move " + node_name);
	undo_redo->add_do_method(old_parent, "remove_child", node);
	undo_redo->add_do_method(new_parent, "add_child", node);
	if (index >= 0) {
		undo_redo->add_do_method(new_parent, "move_child", node, index);
	}
	undo_redo->add_do_method(node, "set_owner", scene_root);
	// Undo: reverse the operation.
	undo_redo->add_undo_method(new_parent, "remove_child", node);
	undo_redo->add_undo_method(old_parent, "add_child", node);
	undo_redo->add_undo_method(old_parent, "move_child", node, old_index);
	undo_redo->add_undo_method(node, "set_owner", scene_root);
	undo_redo->commit_action();

	// Build response.
	String new_parent_display = new_parent_path.is_empty() ? String(scene_root->get_name()) : new_parent_path;
	String text = vformat("Moved '%s' from '%s' to '%s'",
			node_name, old_parent_path, new_parent_display);
	if (index >= 0) {
		text += vformat(" at index %d", index);
	}

	Dictionary structured;
	structured["node_name"] = node_name;
	structured["old_parent"] = old_parent_path;
	structured["new_parent"] = new_parent_display;
	structured["old_index"] = old_index;
	structured["new_index"] = index;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_duplicate_node(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_duplicate_node_impl, p_args);
}

Dictionary MCPSceneTools::_handle_duplicate_node_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find the target node.
	Node *node = _find_node(node_path);
	if (!node) {
		return make_tool_error(vformat(
				"Node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(scene_root->get_name())));
	}

	// Cannot duplicate the scene root.
	if (node == scene_root) {
		return make_tool_error(
				"Cannot duplicate the scene root node.\n\n"
				"To copy the entire scene, use scene/save with a different path.");
	}

	// Duplicate the node (with all default flags: signals, groups, scripts, instantiation).
	Node *dup = node->duplicate();
	if (!dup) {
		return make_tool_error(vformat(
				"Failed to duplicate node '%s'. The node may not support duplication.",
				node_path));
	}

	// Set name if provided.
	String new_name = String(p_args.get("new_name", "")).strip_edges();
	if (!new_name.is_empty()) {
		dup->set_name(new_name);
	}

	String node_name = String(node->get_name());
	Node *parent = node->get_parent();

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		memdelete(dup);
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Duplicate " + node_name);
	undo_redo->add_do_method(parent, "add_child", dup);
	undo_redo->add_do_method(dup, "set_owner", scene_root);
	undo_redo->add_do_reference(dup);
	undo_redo->add_undo_method(parent, "remove_child", dup);
	undo_redo->commit_action();

	// Read back the actual name and path.
	String actual_name = String(dup->get_name());
	String parent_path_str;
	if (parent == scene_root) {
		parent_path_str = "";
	} else {
		parent_path_str = String(parent->get_path()).replace(String(scene_root->get_path()) + "/", "");
	}
	String dup_path = parent_path_str.is_empty() ? actual_name : (parent_path_str + "/" + actual_name);

	// Build response.
	String text = vformat("Duplicated '%s' as '%s'\nPath: %s",
			node_name, actual_name, dup_path);

	Dictionary structured;
	structured["original_node"] = node_name;
	structured["original_path"] = node_path;
	structured["duplicate_name"] = actual_name;
	structured["duplicate_path"] = dup_path;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_instance_scene(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_instance_scene_impl, p_args);
}

Dictionary MCPSceneTools::_handle_instance_scene_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String scene_path = String(p_args.get("scene_path", "")).strip_edges();
	if (scene_path.is_empty()) {
		return make_tool_error("Missing required parameter: scene_path");
	}

	// Validate the scene path format.
	if (!scene_path.begins_with("res://")) {
		return make_tool_error(vformat(
				"Invalid scene path: '%s'\n\n"
				"Scene paths must start with 'res://' (e.g., 'res://scenes/enemy.tscn').",
				scene_path));
	}

	String ext = scene_path.get_extension().to_lower();
	if (ext != "tscn" && ext != "scn") {
		return make_tool_error(vformat(
				"Invalid scene file: '%s'\n\n"
				"Only .tscn and .scn scene files can be instanced.",
				scene_path));
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Load the PackedScene.
	Ref<PackedScene> packed = ResourceLoader::load(scene_path, "PackedScene");
	if (packed.is_null()) {
		return make_tool_error(vformat(
				"Failed to load scene: '%s'\n\n"
				"Make sure the file exists and is a valid scene file. "
				"Use your native file tools to find available .tscn scenes.",
				scene_path));
	}

	// Instance the scene.
	Node *instance = packed->instantiate();
	if (!instance) {
		return make_tool_error(vformat(
				"Failed to instantiate scene: '%s'\n\n"
				"The scene file may be corrupted or have unresolved dependencies.",
				scene_path));
	}

	// Set the name if provided.
	String name = String(p_args.get("name", "")).strip_edges();
	if (!name.is_empty()) {
		instance->set_name(name);
	}

	// Find parent node.
	String parent_path = String(p_args.get("parent_path", "")).strip_edges();
	Node *parent = parent_path.is_empty() ? scene_root : _find_node(parent_path);
	if (!parent) {
		memdelete(instance);
		return make_tool_error(vformat(
				"Parent node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				parent_path, String(scene_root->get_name())));
	}

	// Add to scene via undo/redo.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		memdelete(instance);
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action(vformat("Instance %s", scene_path.get_file()));
	undo_redo->add_do_method(parent, "add_child", instance);
	undo_redo->add_do_method(instance, "set_owner", scene_root);
	undo_redo->add_do_reference(instance);
	undo_redo->add_undo_method(parent, "remove_child", instance);
	undo_redo->commit_action();

	// Build response.
	String actual_name = String(instance->get_name());
	String node_path_str = parent_path.is_empty() ? actual_name : (parent_path + "/" + actual_name);

	String text = vformat("Instanced '%s' as '%s' under '%s'\nPath: %s",
			scene_path, actual_name,
			parent_path.is_empty() ? String(scene_root->get_name()) : parent_path,
			node_path_str);

	Dictionary structured;
	structured["scene_path"] = scene_path;
	structured["instance_name"] = actual_name;
	structured["parent_path"] = parent_path.is_empty() ? String(scene_root->get_name()) : parent_path;
	structured["node_path"] = node_path_str;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_connect_signal(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_connect_signal_impl, p_args);
}

Dictionary MCPSceneTools::_handle_connect_signal_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String source_path = String(p_args.get("source_path", "")).strip_edges();
	String signal_name = String(p_args.get("signal_name", "")).strip_edges();
	String target_path = String(p_args.get("target_path", "")).strip_edges();
	String method_name = String(p_args.get("method_name", "")).strip_edges();
	if (source_path.is_empty()) {
		return make_tool_error("Missing required parameter: source_path");
	}
	if (signal_name.is_empty()) {
		return make_tool_error("Missing required parameter: signal_name");
	}
	if (target_path.is_empty()) {
		return make_tool_error("Missing required parameter: target_path");
	}
	if (method_name.is_empty()) {
		return make_tool_error("Missing required parameter: method_name");
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find source node.
	Node *source = _find_node(source_path);
	if (!source) {
		return make_tool_error(vformat(
				"Source node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				source_path, String(scene_root->get_name())));
	}

	// Find target node.
	Node *target = _find_node(target_path);
	if (!target) {
		return make_tool_error(vformat(
				"Target node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				target_path, String(scene_root->get_name())));
	}

	// Verify the signal exists on the source node.
	if (!source->has_signal(signal_name)) {
		return make_tool_error(vformat(
				"Signal '%s' not found on node '%s' (type: %s).\n\n"
				"Use scene/browse_tree with node_path='%s' and categories='signals' "
				"to see available signals.",
				signal_name, source_path, source->get_class(), source_path));
	}

	// Build the callable.
	Callable callable(target, StringName(method_name));

	// Check if already connected.
	if (source->is_connected(StringName(signal_name), callable)) {
		return make_tool_error(vformat(
				"Signal '%s' on '%s' is already connected to '%s::%s'.\n\n"
				"Use scene/browse_tree with categories='signals' to see existing connections.",
				signal_name, source_path, target_path, method_name));
	}

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Connect " + signal_name);
	undo_redo->add_do_method(source, "connect", StringName(signal_name), callable);
	undo_redo->add_undo_method(source, "disconnect", StringName(signal_name), callable);
	undo_redo->commit_action();

	// Build response.
	String text = vformat("Connected signal '%s' on '%s' to '%s::%s'",
			signal_name, source_path, target_path, method_name);

	Dictionary structured;
	structured["source_path"] = source_path;
	structured["signal_name"] = signal_name;
	structured["target_path"] = target_path;
	structured["method_name"] = method_name;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_disconnect_signal(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_disconnect_signal_impl, p_args);
}

Dictionary MCPSceneTools::_handle_disconnect_signal_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String source_path = String(p_args.get("source_path", "")).strip_edges();
	String signal_name = String(p_args.get("signal_name", "")).strip_edges();
	String target_path = String(p_args.get("target_path", "")).strip_edges();
	String method_name = String(p_args.get("method_name", "")).strip_edges();
	if (source_path.is_empty()) {
		return make_tool_error("Missing required parameter: source_path");
	}
	if (signal_name.is_empty()) {
		return make_tool_error("Missing required parameter: signal_name");
	}
	if (target_path.is_empty()) {
		return make_tool_error("Missing required parameter: target_path");
	}
	if (method_name.is_empty()) {
		return make_tool_error("Missing required parameter: method_name");
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find source node.
	Node *source = _find_node(source_path);
	if (!source) {
		return make_tool_error(vformat(
				"Source node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				source_path, String(scene_root->get_name())));
	}

	// Find target node.
	Node *target = _find_node(target_path);
	if (!target) {
		return make_tool_error(vformat(
				"Target node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				target_path, String(scene_root->get_name())));
	}

	// Build the callable.
	Callable callable(target, StringName(method_name));

	// Verify the connection exists.
	if (!source->is_connected(StringName(signal_name), callable)) {
		return make_tool_error(vformat(
				"Signal '%s' on '%s' is not connected to '%s::%s'.\n\n"
				"Use scene/browse_tree with node_path='%s' and categories='signals' "
				"to see existing connections.",
				signal_name, source_path, target_path, method_name, source_path));
	}

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Disconnect " + signal_name);
	undo_redo->add_do_method(source, "disconnect", StringName(signal_name), callable);
	undo_redo->add_undo_method(source, "connect", StringName(signal_name), callable);
	undo_redo->commit_action();

	// Build response.
	String text = vformat("Disconnected signal '%s' on '%s' from '%s::%s'",
			signal_name, source_path, target_path, method_name);

	Dictionary structured;
	structured["source_path"] = source_path;
	structured["signal_name"] = signal_name;
	structured["target_path"] = target_path;
	structured["method_name"] = method_name;

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_attach_script(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_attach_script_impl, p_args);
}

Dictionary MCPSceneTools::_handle_attach_script_impl(const Dictionary &p_args) {
	// Validate required arguments.
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	String script_path = String(p_args.get("script_path", "")).strip_edges();
	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}
	if (script_path.is_empty()) {
		return make_tool_error("Missing required parameter: script_path");
	}

	// Validate script path format.
	if (!script_path.begins_with("res://")) {
		return make_tool_error(vformat(
				"Invalid script path: '%s'\n\n"
				"Script paths must start with 'res://' (e.g., 'res://scripts/player.gd').",
				script_path));
	}

	String ext = script_path.get_extension().to_lower();
	if (ext != "gd" && ext != "cs") {
		return make_tool_error(vformat(
				"Invalid script file extension: '%s'\n\n"
				"Only .gd (GDScript) and .cs (C#) script files are supported.",
				script_path));
	}

	// Get scene root.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	// Find the target node.
	Node *node = _find_node(node_path);
	if (!node) {
		return make_tool_error(vformat(
				"Node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(scene_root->get_name())));
	}

	// Load the script resource.
	Ref<Script> script = ResourceLoader::load(script_path, "Script");
	if (script.is_null()) {
		return make_tool_error(vformat(
				"Failed to load script: '%s'\n\n"
				"Make sure the file exists and is a valid script. "
				"Write the script with your native file tools, then call editor/scan_filesystem.",
				script_path));
	}

	// Store old script for undo.
	Variant old_script = node->get_script();

	// Create undo/redo action.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Attach Script");
	undo_redo->add_do_method(node, "set_script", script);
	undo_redo->add_undo_method(node, "set_script", old_script);
	undo_redo->commit_action();

	// Build response.
	String text = vformat("Attached script '%s' to node '%s' (%s)",
			script_path, node_path, node->get_class());

	Ref<Script> old_script_ref = old_script;
	if (old_script_ref.is_valid() && !old_script_ref->get_path().is_empty()) {
		text += vformat("\n  Previous script: %s", old_script_ref->get_path());
	}

	Dictionary structured;
	structured["node_path"] = node_path;
	structured["node_type"] = node->get_class();
	structured["script_path"] = script_path;
	if (old_script_ref.is_valid() && !old_script_ref->get_path().is_empty()) {
		structured["previous_script"] = old_script_ref->get_path();
	}

	return make_tool_result(text, structured);
}

Dictionary MCPSceneTools::handle_save(const Dictionary &p_args) {
	// NOTE: save_scene uses EditorNode::save_scene_to_path() which shows a
	// progress dialog. Progress dialogs cannot be created inside
	// call_deferred() (MessageQueue::flush). Since save only reads the
	// scene tree for serialization (no add_child/remove_child), it is safe
	// to run directly from the poll thread (which has
	// set_current_thread_safe_for_nodes(true)).
	return _handle_save_impl(p_args);
}

Dictionary MCPSceneTools::_handle_save_impl(const Dictionary &p_args) {
	// Check that a scene is open.
	Node *scene_root = _get_scene_root();
	if (!scene_root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return make_tool_error("EditorInterface not available.");
	}

	String path = String(p_args.get("path", "")).strip_edges();

	if (!path.is_empty()) {
		// Save As to specific path.
		if (!path.begins_with("res://")) {
			return make_tool_error(vformat(
					"Invalid path: '%s'\n\n"
					"Save paths must start with 'res://' (e.g., 'res://scenes/level.tscn').",
					path));
		}

		String ext = path.get_extension().to_lower();
		if (ext != "tscn" && ext != "scn") {
			return make_tool_error(vformat(
					"Invalid file extension: '%s'\n\n"
					"Scene files must have a .tscn or .scn extension.",
					path));
		}

		ei->save_scene_as(path);

		String text = vformat("Scene saved as: %s", path);
		Dictionary structured;
		structured["path"] = path;
		structured["action"] = "save_as";
		return make_tool_result(text, structured);
	} else {
		// Save to current path.
		String current_path = scene_root->get_scene_file_path();
		if (current_path.is_empty()) {
			return make_tool_error(
					"Scene has never been saved. Please provide a 'path' parameter "
					"(e.g., 'res://scenes/my_scene.tscn') for the initial save.");
		}

		Error err = ei->save_scene();
		if (err != OK) {
			return make_tool_error(vformat(
					"Failed to save scene to '%s' (error: %d).\n\n"
					"The file may be read-only or the directory may not exist.",
					current_path, (int)err));
		}

		String text = vformat("Scene saved: %s", current_path);
		Dictionary structured;
		structured["path"] = current_path;
		structured["action"] = "save";
		return make_tool_result(text, structured);
	}
}

// ============================================================================
// Anchor Preset Lookup Table
// ============================================================================

// Vector4 = (anchor_left, anchor_top, anchor_right, anchor_bottom)
static HashMap<String, Vector4> _build_anchor_presets() {
	HashMap<String, Vector4> presets;
	presets["top_left"] = Vector4(0, 0, 0, 0);
	presets["top_right"] = Vector4(1, 0, 1, 0);
	presets["bottom_left"] = Vector4(0, 1, 0, 1);
	presets["bottom_right"] = Vector4(1, 1, 1, 1);
	presets["center"] = Vector4(0.5, 0.5, 0.5, 0.5);
	presets["center_left"] = Vector4(0, 0.5, 0, 0.5);
	presets["center_right"] = Vector4(1, 0.5, 1, 0.5);
	presets["center_top"] = Vector4(0.5, 0, 0.5, 0);
	presets["center_bottom"] = Vector4(0.5, 1, 0.5, 1);
	presets["left_wide"] = Vector4(0, 0, 0, 1);
	presets["right_wide"] = Vector4(1, 0, 1, 1);
	presets["top_wide"] = Vector4(0, 0, 1, 0);
	presets["bottom_wide"] = Vector4(0, 1, 1, 1);
	presets["full_rect"] = Vector4(0, 0, 1, 1);
	presets["vcenter_wide"] = Vector4(0.5, 0, 0.5, 1);
	presets["hcenter_wide"] = Vector4(0, 0.5, 1, 0.5);
	return presets;
}

static const HashMap<String, Vector4> &_get_anchor_presets() {
	static HashMap<String, Vector4> presets = _build_anchor_presets();
	return presets;
}

// ============================================================================
// Tool: scene/set_anchor_preset
// ============================================================================

Dictionary MCPSceneTools::handle_set_anchor_preset(const Dictionary &p_args) {
	return _run_on_main_thread(&MCPSceneTools::_handle_set_anchor_preset_impl, p_args);
}

Dictionary MCPSceneTools::_handle_set_anchor_preset_impl(const Dictionary &p_args) {
	String node_path = String(p_args.get("node_path", "")).strip_edges();
	String preset = ((String)p_args.get("preset", "")).strip_edges().to_lower();

	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}
	if (preset.is_empty()) {
		return make_tool_error("Missing required parameter: preset");
	}

	const HashMap<String, Vector4> &presets = _get_anchor_presets();
	if (!presets.has(preset)) {
		return make_tool_error(
				"Unknown anchor preset: '" + preset + "'\n\n"
				"Available presets: top_left, top_right, bottom_left, bottom_right, "
				"center, center_left, center_right, center_top, center_bottom, "
				"left_wide, right_wide, top_wide, bottom_wide, full_rect, "
				"vcenter_wide, hcenter_wide");
	}

	Vector4 anchors = presets[preset];

	// Get scene root and find node.
	Node *root = _get_scene_root();
	if (!root) {
		return make_tool_error("No scene is currently open in the editor.");
	}

	Node *node = _find_node(node_path);
	if (!node) {
		return make_tool_error(vformat(
				"Node not found: '%s'\n\n"
				"Path must be relative to the scene root ('%s'). "
				"Use scene/browse_tree to discover valid node paths.",
				node_path, String(root->get_name())));
	}

	Control *control = Object::cast_to<Control>(node);
	if (!control) {
		return make_tool_error(vformat(
				"Node is not a Control: '%s' (type: %s)\n\n"
				"Anchor presets can only be applied to Control-derived nodes.",
				node_path, node->get_class()));
	}

	// Use UndoRedo for editor-undoable operation.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		return make_tool_error("EditorUndoRedoManager not available.");
	}

	undo_redo->create_action("Set Anchor Preset: " + preset);
	undo_redo->add_do_property(control, "anchor_left", anchors.x);
	undo_redo->add_do_property(control, "anchor_top", anchors.y);
	undo_redo->add_do_property(control, "anchor_right", anchors.z);
	undo_redo->add_do_property(control, "anchor_bottom", anchors.w);
	undo_redo->add_undo_property(control, "anchor_left", control->get_anchor(SIDE_LEFT));
	undo_redo->add_undo_property(control, "anchor_top", control->get_anchor(SIDE_TOP));
	undo_redo->add_undo_property(control, "anchor_right", control->get_anchor(SIDE_RIGHT));
	undo_redo->add_undo_property(control, "anchor_bottom", control->get_anchor(SIDE_BOTTOM));
	undo_redo->commit_action();

	String text = "Set anchor preset '" + preset + "' on " + node_path + "\n"
			"Anchors: left=" + String::num(anchors.x) + " top=" + String::num(anchors.y) +
			" right=" + String::num(anchors.z) + " bottom=" + String::num(anchors.w);

	Dictionary structured;
	structured["node_path"] = node_path;
	structured["preset"] = preset;
	structured["anchor_left"] = anchors.x;
	structured["anchor_top"] = anchors.y;
	structured["anchor_right"] = anchors.z;
	structured["anchor_bottom"] = anchors.w;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool Registration
// ============================================================================

void MCPSceneTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- scene/browse_tree (Phase 1 -- fully implemented) ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a specific node relative to the scene root. "
				"Empty or omitted = show the full scene tree (coarse mode). "
				"Set to a path like 'Player' or 'UI/HealthBar' = show detailed info for that node. "
				"Use empty string first to discover the tree structure.");
		props["max_depth"] = make_prop("integer",
				"Maximum tree depth for coarse mode (default 3, max 20). "
				"Ignored in detail mode.");
		props["categories"] = make_prop("string",
				"For detail mode only. Comma-separated filter: 'properties', 'transform', "
				"'signals', 'groups', 'script', or 'all' (default). "
				"Example: 'properties,transform' to see only properties and transform.");
		props["include_defaults"] = make_prop("boolean",
				"For detail mode only. Show properties even if they match the class default "
				"value (default false). Useful for seeing all available properties on a node.");
		Array required;

		p_registry->register_tool("scene/browse_tree",
				"Browse Scene Tree",
				"Inspect the currently open scene's node tree and node details.\n"
				"\n"
				"TWO MODES:\n"
				"1. TREE MODE (no node_path): Shows the full scene tree with node names, types,\n"
				"   and attached scripts. Use max_depth to control how deep the tree goes.\n"
				"2. DETAIL MODE (node_path set): Shows all properties, signal connections,\n"
				"   groups, and script variables for a specific node.\n"
				"\n"
				"WORKFLOW: Call with no args first to see the tree, then call with node_path\n"
				"to drill into specific nodes.\n"
				"\n"
				"Paths are relative to the scene root. Examples:\n"
				"- '' or omitted -> full tree\n"
				"- 'Player' -> details of the Player node (direct child of root)\n"
				"- 'World/Enemies/Boss' -> details of a deeply nested node\n"
				"\n"
				"In detail mode, non-default properties are shown by default. Set\n"
				"include_defaults=true to see ALL properties including those at class defaults.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_browse_tree));
	}

	// ---- scene/set_property ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node relative to the scene root (e.g., 'Player', 'UI/HealthBar').");
		props["property"] = make_prop("string",
				"Property name to set (e.g., 'position', 'modulate', 'visible'). "
				"Use scene/browse_tree with include_defaults=true to discover available properties.");
		props["value"] = make_prop("string",
				"New value as a JSON-compatible expression. Vectors use arrays: [100, 200] for "
				"Vector2, [1, 2, 3] for Vector3. Colors use arrays: [1, 0, 0, 1] for red. "
				"Booleans: true/false. NodePaths: string paths. Resources: res:// paths.");
		Array required;
		required.push_back("node_path");
		required.push_back("property");
		required.push_back("value");

		p_registry->register_tool("scene/set_property",
				"Set Node Property",
				"Set a property on a scene node. Supports all Godot property types including\n"
				"Vector2, Vector3, Color, NodePath, Resource references, and more.\n"
				"\n"
				"Uses the editor undo/redo system so changes can be reverted. The value is\n"
				"automatically coerced from JSON to the correct Godot type.\n"
				"\n"
				"Value format: Vector2=[x,y], Vector3=[x,y,z], Color=[r,g,b,a], bool, string.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_set_property));
	}

	// ---- scene/add_node ----
	{
		Dictionary props;
		props["parent_path"] = make_prop("string",
				"Path to the parent node. Empty string = scene root.");
		props["type"] = make_prop("string",
				"Class name of the new node (e.g., 'Sprite2D', 'CharacterBody3D', 'Label').");
		props["name"] = make_prop("string",
				"Name for the new node (optional, defaults to the type name). "
				"Godot will auto-suffix with a number if the name already exists.");
		props["properties"] = make_prop("object",
				"Optional dictionary of initial property values to set on the new node. "
				"Same value format as scene/set_property.");
		Array required;
		required.push_back("type");

		p_registry->register_tool("scene/add_node",
				"Add Node to Scene",
				"Create a new node and add it to the scene tree.\n"
				"\n"
				"The node is created with the specified type and optionally named. Initial\n"
				"property values can be set in the same call. Uses the editor undo/redo system.\n"
				"\n"
				"Optional properties dict uses same value format as scene/set_property.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPSceneTools::handle_add_node));
	}

	// ---- scene/remove_node ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node to remove, relative to the scene root.");
		Array required;
		required.push_back("node_path");

		p_registry->register_tool("scene/remove_node",
				"Remove Node from Scene",
				"Remove a node and all its children from the scene tree.\n"
				"\n"
				"Uses undo/redo. Cannot remove the scene root.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/true, /*idempotent=*/false),
				callable_mp_static(&MCPSceneTools::handle_remove_node));
	}

	// ---- scene/rename_node ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node to rename, relative to the scene root.");
		props["new_name"] = make_prop("string",
				"The new name for the node. Must be a valid Godot node name.");
		Array required;
		required.push_back("node_path");
		required.push_back("new_name");

		p_registry->register_tool("scene/rename_node",
				"Rename Node",
				"Rename a node. Uses undo/redo. Godot may adjust for sibling uniqueness.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_rename_node));
	}

	// ---- scene/move_node ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node to move, relative to the scene root.");
		props["new_parent_path"] = make_prop("string",
				"Path to the new parent node. Empty string = scene root.");
		props["index"] = make_prop("integer",
				"Child index position under the new parent (optional). "
				"-1 or omitted = append as last child.");
		Array required;
		required.push_back("node_path");
		required.push_back("new_parent_path");

		p_registry->register_tool("scene/move_node",
				"Move/Reparent Node",
				"Reparent a node to a different parent. Optional index for ordering. Uses undo/redo.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_move_node));
	}

	// ---- scene/duplicate_node ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node to duplicate, relative to the scene root.");
		props["new_name"] = make_prop("string",
				"Name for the duplicated node (optional, auto-generated if omitted).");
		Array required;
		required.push_back("node_path");

		p_registry->register_tool("scene/duplicate_node",
				"Duplicate Node",
				"Duplicate a node and all its children as a sibling. Uses undo/redo.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPSceneTools::handle_duplicate_node));
	}

	// ---- scene/instance_scene ----
	{
		Dictionary props;
		props["scene_path"] = make_prop("string",
				"Path to the .tscn file to instance (e.g., 'res://scenes/enemy.tscn').");
		props["parent_path"] = make_prop("string",
				"Path to the parent node. Empty string = scene root.");
		props["name"] = make_prop("string",
				"Name for the instanced node (optional, uses scene name if omitted).");
		Array required;
		required.push_back("scene_path");

		p_registry->register_tool("scene/instance_scene",
				"Instance Scene",
				"Instance a .tscn/.scn as a child node. Stays linked to source file. Uses undo/redo.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPSceneTools::handle_instance_scene));
	}

	// ---- scene/connect_signal ----
	{
		Dictionary props;
		props["source_path"] = make_prop("string",
				"Path to the node emitting the signal.");
		props["signal_name"] = make_prop("string",
				"Name of the signal to connect (e.g., 'pressed', 'body_entered').");
		props["target_path"] = make_prop("string",
				"Path to the node receiving the signal.");
		props["method_name"] = make_prop("string",
				"Name of the method to call on the target node.");
		Array required;
		required.push_back("source_path");
		required.push_back("signal_name");
		required.push_back("target_path");
		required.push_back("method_name");

		p_registry->register_tool("scene/connect_signal",
				"Connect Signal",
				"Connect a signal to a method on another node. Persisted in scene. Uses undo/redo.\n"
				"Use scene/browse_tree with categories='signals' to discover available signals.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_connect_signal));
	}

	// ---- scene/disconnect_signal ----
	{
		Dictionary props;
		props["source_path"] = make_prop("string",
				"Path to the node emitting the signal.");
		props["signal_name"] = make_prop("string",
				"Name of the signal to disconnect.");
		props["target_path"] = make_prop("string",
				"Path to the target node.");
		props["method_name"] = make_prop("string",
				"Name of the method to disconnect.");
		Array required;
		required.push_back("source_path");
		required.push_back("signal_name");
		required.push_back("target_path");
		required.push_back("method_name");

		p_registry->register_tool("scene/disconnect_signal",
				"Disconnect Signal",
				"Disconnect a signal connection between two nodes. Uses undo/redo.\n"
				"Use scene/browse_tree with categories='signals' to see existing connections.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/true, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_disconnect_signal));
	}

	// ---- scene/attach_script ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node to attach the script to.");
		props["script_path"] = make_prop("string",
				"Path to the .gd script file (e.g., 'res://scripts/player.gd'). "
				"The script must already exist. Write it with your native file tools and call editor/scan_filesystem first.");
		Array required;
		required.push_back("node_path");
		required.push_back("script_path");

		p_registry->register_tool("scene/attach_script",
				"Attach Script to Node",
				"Attach a .gd or .cs script to a node. Script must exist on disk.\n"
				"Uses undo/redo. Write the script with your native file tools and call editor/scan_filesystem first.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_attach_script));
	}

	// ---- scene/save ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Optional file path to save as (e.g., 'res://scenes/level.tscn'). "
				"If omitted, saves to the current scene path. If the scene has never been saved, "
				"this parameter is required.");
		Array required;

		p_registry->register_tool("scene/save",
				"Save Scene",
				"Save the current scene. With path: Save As. Without: save to existing path.\n"
				"Always save after making scene/* modifications.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_save));
	}

	// ---- scene/set_anchor_preset ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string", "Path to the Control node relative to the scene root.");
		props["preset"] = make_prop("string",
				"Anchor preset name: top_left, top_right, bottom_left, bottom_right, "
				"center, center_left, center_right, center_top, center_bottom, "
				"left_wide, right_wide, top_wide, bottom_wide, full_rect, "
				"vcenter_wide, hcenter_wide");
		Array required;
		required.push_back("node_path");
		required.push_back("preset");
		p_registry->register_tool(
				"scene/set_anchor_preset", "Set Anchor Preset",
				"Set the anchor of a Control node using a named preset. Presets set "
				"all four anchor values (left, top, right, bottom) at once. Commonly "
				"used presets: 'full_rect' (fills parent), 'center' (centered point), "
				"'top_wide' (top edge, full width). Uses undo/redo.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPSceneTools::handle_set_anchor_preset));
	}
}
