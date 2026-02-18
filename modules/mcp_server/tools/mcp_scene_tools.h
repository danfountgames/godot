/**************************************************************************/
/*  mcp_scene_tools.h                                                     */
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

#pragma once

#include "core/object/class_db.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;
class Node;

class MCPSceneTools {
public:
	static void register_tools(MCPToolRegistry *p_registry);

private:
	// Tool handlers.
	static Dictionary handle_browse_tree(const Dictionary &p_args);
	static Dictionary handle_set_property(const Dictionary &p_args);
	static Dictionary handle_add_node(const Dictionary &p_args);
	static Dictionary handle_remove_node(const Dictionary &p_args);
	static Dictionary handle_rename_node(const Dictionary &p_args);
	static Dictionary handle_move_node(const Dictionary &p_args);
	static Dictionary handle_duplicate_node(const Dictionary &p_args);
	static Dictionary handle_instance_scene(const Dictionary &p_args);
	static Dictionary handle_connect_signal(const Dictionary &p_args);
	static Dictionary handle_disconnect_signal(const Dictionary &p_args);
	static Dictionary handle_attach_script(const Dictionary &p_args);
	static Dictionary handle_save(const Dictionary &p_args);
	static Dictionary handle_set_anchor_preset(const Dictionary &p_args);

	// Helpers.
	static void _format_tree_recursive(Node *p_node, String &r_text, Dictionary &r_structured,
			const String &p_prefix, bool p_is_last, int p_depth, int p_max_depth);
	static Dictionary _build_node_detail(Node *p_node, const String &p_categories, bool p_include_defaults);
	static Variant _coerce_json_to_variant(const Variant &p_json_value, Variant::Type p_target_type);
	static Node *_get_scene_root();
	static Node *_find_node(const String &p_path);

	// Main-thread dispatch for scene mutation tools.
	// Scene tree modifications (add_child, remove_child, etc.) must run on
	// the main thread. These helpers ensure correct dispatch when the MCP
	// server runs in threaded mode.
	typedef Dictionary (*SceneToolHandler)(const Dictionary &);
	static Dictionary _run_on_main_thread(SceneToolHandler p_handler, const Dictionary &p_args);
	static void _do_scene_tool_main();

	// Internal implementations of mutation tools (run on main thread).
	static Dictionary _handle_set_property_impl(const Dictionary &p_args);
	static Dictionary _handle_add_node_impl(const Dictionary &p_args);
	static Dictionary _handle_remove_node_impl(const Dictionary &p_args);
	static Dictionary _handle_rename_node_impl(const Dictionary &p_args);
	static Dictionary _handle_move_node_impl(const Dictionary &p_args);
	static Dictionary _handle_duplicate_node_impl(const Dictionary &p_args);
	static Dictionary _handle_instance_scene_impl(const Dictionary &p_args);
	static Dictionary _handle_connect_signal_impl(const Dictionary &p_args);
	static Dictionary _handle_disconnect_signal_impl(const Dictionary &p_args);
	static Dictionary _handle_attach_script_impl(const Dictionary &p_args);
	static Dictionary _handle_save_impl(const Dictionary &p_args);
	static Dictionary _handle_set_anchor_preset_impl(const Dictionary &p_args);
};
