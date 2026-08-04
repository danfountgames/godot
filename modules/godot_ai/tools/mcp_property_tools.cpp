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
#include "../mcp_runtime_bridge.h"
#include "../mcp_tool_registry.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/variant/variant_parser.h"
#include "core/variant/array.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/editor_debugger_tree.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"

namespace {

// A property's *declared* type, which is not always the type of what it currently
// holds. `script` on a node with no script reads back as nil, and coercing against nil
// accepts anything - which is how a res:// path once went in as a bare String, leaving a
// scene whose script was a piece of text. The engine loaded it, ran it, and reported
// nothing, because a String is a perfectly valid Variant to have stored.
static Variant::Type declared_type(Object *p_object, const String &p_property, String &r_hint) {
	if (!p_object) {
		return Variant::NIL;
	}
	// A theme override does not exist as a property until something sets it, so it is
	// absent from get_property_list() *and* reads back as a default. Coercing against
	// either gives the wrong answer: an array meant for a Color goes in as an array, the
	// engine refuses it, and the read-back returns the default black the property already
	// had - a successful-looking write of the wrong value. Writing it twice worked, which
	// is how the shape of the bug shows. The prefix is the declaration.
	if (p_property.begins_with("theme_override_colors/")) {
		return Variant::COLOR;
	}
	if (p_property.begins_with("theme_override_constants/") ||
			p_property.begins_with("theme_override_font_sizes/")) {
		return Variant::INT;
	}
	if (p_property.begins_with("theme_override_fonts/")) {
		r_hint = "Font";
		return Variant::OBJECT;
	}
	if (p_property.begins_with("theme_override_icons/")) {
		r_hint = "Texture2D";
		return Variant::OBJECT;
	}
	if (p_property.begins_with("theme_override_styles/")) {
		r_hint = "StyleBox";
		return Variant::OBJECT;
	}
	List<PropertyInfo> properties;
	p_object->get_property_list(&properties);
	for (const PropertyInfo &info : properties) {
		if (info.name == p_property) {
			r_hint = info.hint_string;
			return info.type;
		}
	}
	return Variant::NIL;
}

// A res:// path standing in for a resource. Anything that takes an Object - a script, a
// texture, a material - is far more naturally addressed by path than by anything JSON
// can express, so a string is accepted and *loaded*, or refused with the reason.
static bool coerce_resource(const Variant &p_incoming, const String &p_hint, Variant &r_out, String &r_error) {
	if (p_incoming.get_type() == Variant::NIL) {
		r_out = Variant();
		return true;
	}
	if (p_incoming.get_type() != Variant::STRING) {
		r_error = "this property holds a resource; give it a res:// path, or null to clear it";
		return false;
	}
	const String path = String(p_incoming).strip_edges();
	if (path.is_empty()) {
		r_out = Variant();
		return true;
	}
	if (!path.begins_with("res://")) {
		r_error = vformat("'%s' is not a res:// path, and this property holds a resource rather "
						  "than text",
				path);
		return false;
	}
	if (!ResourceLoader::exists(path)) {
		r_error = vformat("no resource at '%s'", path);
		return false;
	}
	const Ref<Resource> resource = ResourceLoader::load(path);
	if (resource.is_null()) {
		r_error = vformat("'%s' could not be loaded; it may be broken or reference something "
						  "missing",
				path);
		return false;
	}
	if (!p_hint.is_empty() && !resource->is_class(p_hint)) {
		r_error = vformat("'%s' is a %s, but this property holds a %s", path,
				resource->get_class(), p_hint);
		return false;
	}
	r_out = resource;
	return true;
}

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

		const Variant incoming = p_arguments.has("value") ? p_arguments["value"] : Variant();
		Variant value;
		String coercion_error;
		// The declared type, not the current one: a null `script` accepts anything if you
		// only look at what it holds now.
		String hint;
		const Variant::Type declared = declared_type(node, property, hint);
		if (declared == Variant::OBJECT) {
			if (!coerce_resource(incoming, hint, value, coercion_error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, coercion_error);
				return Dictionary();
			}
		} else {
			// Against the declared type where there is one. The current value is only a
			// fallback, and a misleading one for anything that is absent until written.
			Variant target = current;
			if (declared != Variant::NIL && declared != current.get_type()) {
				Callable::CallError call_error;
				Variant::construct(declared, target, nullptr, 0, call_error);
			}
			if (!coerce_value(target, incoming, value, coercion_error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, coercion_error);
				return Dictionary();
			}
		}

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(vformat("Godot AI: set %s.%s", node->get_name(), property));
		undo_redo->add_do_property(node, property, value);
		undo_redo->add_undo_property(node, property, current);
		undo_redo->commit_action();

		// Read back before answering. A write that did not take is the failure mode this
		// whole tool family exists to make impossible to report as success.
		bool after_valid = false;
		const Variant after = node->get(property, &after_valid);
		if (!after_valid) {
			r_error.set(MCPToolError::FAILED,
					vformat("'%s' vanished after being written", property));
			return Dictionary();
		}
		// Compare against what was *intended*, not merely that something is there. The
		// earlier version read back the value it had just failed to change and reported
		// success, which is the one thing a read-back exists to prevent.
		if (after != value) {
			r_error.set(MCPToolError::FAILED,
					vformat("'%s' did not take the value it was given; it holds %s instead. "
							"The property may not accept that type.",
							property, String(after)));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = String(root->get_path_to(node));
		result["property"] = property;
		result["persistent"] = true;
		String written;
		VariantWriter::write_to_string(after, written);
		result["value_written"] = written;
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

	// A cheap identity for "which tree is this", so a stale one can be told from a
	// fresh one. Names and shape are enough: two different trees with identical names
	// at identical depths are the same tree for this purpose.
	String _fingerprint() const {
		const Array nodes = _build(32)["nodes"];
		String out;
		for (int i = 0; i < nodes.size(); i++) {
			const Dictionary node = nodes[i];
			out += String(node["name"]) + ":" + String(node["depth"]) + ";";
		}
		return out;
	}

	Variant _poll() {
		if (!remote_tree_ready()) {
			return Variant();
		}
		// Wait for a tree that arrived *after* the request, so a caller polling while
		// the game boots sees the scene appear instead of the same bare root forever.
		if (_fingerprint() != fingerprint_at_request) {
			return _build(pending_max_depth);
		}
		// ...but a tree that genuinely has not changed must not hang the call. Once the
		// game has had time to answer, the unchanged tree is the honest answer.
		if (OS::get_singleton()->get_ticks_msec() - request_msec > SETTLE_MSEC) {
			return _build(pending_max_depth);
		}
		return Variant();
	}

	static const uint64_t SETTLE_MSEC = 1500;

	int pending_max_depth = 32;
	uint64_t request_msec = 0;
	String fingerprint_at_request;

public:
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		EditorDebuggerTree *tree = get_remote_tree(r_error, get_tool_name());
		if (!tree) {
			return Dictionary();
		}
		// Deliberately not short-circuiting on remote_tree_ready(). The editor keeps the
		// last tree it was sent, and the first arrives as soon as the game is up - often
		// before the main scene has been instantiated. Returning that because it "is
		// ready" makes every later call hand back the same bare root, so an agent
		// polling for its scene waits forever on a cached answer.
		pending_max_depth = (int)p_arguments["max_depth"];
		fingerprint_at_request = remote_tree_ready() ? _fingerprint() : String();
		request_msec = OS::get_singleton()->get_ticks_msec();
		request_remote_tree();
		return MCPDeferred::make_deferred_result(
				MCPDeferred::begin_polled(10.0, callable_mp(this, &GetRuntimeSceneTreeTool::_poll)));
	}
};

class SetRuntimePropertyTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SetRuntimeProperty"; }
	virtual String get_description() const override {
		return "Set a property on a node in the *running* game. The change affects the running "
			   "game only and is discarded when it stops - it is not written to the project. "
			   "Use Godot_SetSceneProperty for a change that must survive.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property(
				"Node path in the running game, such as /root/Main/Player.");
		properties["property"] = MCPSchema::string_property("Property name, such as position.");
		properties["value"] = MCPSchema::any_property(
				"New value. A structured type may be given as an array - [64, 32] for a "
				"Vector2 - and is converted to whatever the property actually holds.");
		Vector<String> required;
		required.push_back("path");
		required.push_back("property");
		required.push_back("value");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node that was changed.");
		properties["property"] = MCPSchema::string_property("Property that was changed.");
		properties["persistent"] = MCPSchema::bool_property("Always false: runtime edits do not persist.");
		properties["type"] = MCPSchema::string_property("Godot type the property holds.");
		properties["text"] = MCPSchema::string_property("The value the property now holds, read back.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no game is running; start one with Godot_PlayCurrentScene or "
					"Godot_PlayMainScene first");
			return Dictionary();
		}
		// Sent through the runtime agent rather than the debugger's generic
		// set_object_property, which hands the value to Object::set unconverted. A
		// Vector2 property given a JSON [64, 32] is simply refused there, silently, and
		// the tool used to report success anyway. The agent knows the property's real
		// type, converts to it, and reads the value back before answering.
		return MCPDeferred::make_deferred_result(bridge->send("set_property", p_arguments));
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
