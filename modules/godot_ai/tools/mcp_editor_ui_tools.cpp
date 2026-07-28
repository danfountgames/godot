/**************************************************************************/
/*  mcp_editor_ui_tools.cpp                                               */
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

// Finding things in the editor's own interface.
//
// Everything else that acts on the editor addresses it by name: a scene path, a node
// path, a setting key. Its *interface* had no such addressing at all, so any attempt to
// drive it came down to a coordinate someone measured off a screenshot - which stops
// being true the moment a theme, a font size or a window size changes. This turns "the
// button that says Approve" into a rectangle, on screen, right now.
//
// Rows come with it, not just nodes. An approvals list is a Tree, and its rows and the
// buttons inside them are not nodes at all; a tool that could only find Controls would
// find the Tree and leave the caller to guess where row three starts. That guess has
// already cost this project a session.

#include "mcp_builtin_tools.h"

#include "../mcp_tool_registry.h"

#include "core/object/object.h"
#include "core/os/os.h"
#include "core/variant/array.h"
#include "scene/gui/control.h"
#include "scene/gui/item_list.h"
#include "scene/gui/tree.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display_server.h"

#include "editor/editor_interface.h"
#include "editor/editor_node.h"

namespace {

// The nearest Window ancestor: which dialog or panel a control belongs to.
Window *containing_window(Node *p_node) {
	for (Node *node = p_node; node; node = node->get_parent()) {
		Window *window = Object::cast_to<Window>(node);
		if (window) {
			return window;
		}
	}
	return nullptr;
}

// What has to be added to a control's screen transform to get true screen coordinates.
//
// `CanvasItem::get_screen_transform()` runs through `Window::get_popup_base_transform()`,
// which folds in each window's own placement and walks out through embedders - so it is
// usually already absolute, and adding a window origin on top would double-count it.
// The exception is a window that embeds its subwindows: that transform returns identity
// for it, deliberately, so everything inside is reported in *its client area's*
// coordinates and the window's placement is the missing term.
//
// Both arrangements occur here. The editor puts its dialogs in real OS windows on a
// normal desktop, and embeds them when single-window mode is on. Getting this wrong is
// silent: the numbers look entirely plausible and the pointer lands somewhere else.
Point2i screen_origin_correction(Node *p_node) {
	for (Node *node = p_node; node; node = node->get_parent()) {
		Window *window = Object::cast_to<Window>(node);
		if (window && !window->is_embedded()) {
			return window->is_embedding_subwindows() ? window->get_position() : Point2i();
		}
	}
	return Point2i();
}

String control_text(Control *p_control) {
	// Controls do not share a `text` property - Button, Label and LineEdit each declare
	// their own - so this asks the object rather than testing for a list of classes that
	// would go stale the moment a plugin added a fourth.
	bool valid = false;
	const Variant value = p_control->get(SNAME("text"), &valid);
	if (valid && value.get_type() == Variant::STRING) {
		return value;
	}
	return String();
}

// `Control::get_tooltip_text()` is private in this engine version, so the tooltip is
// read the same way the text is: through the property, which is public by definition.
String control_tooltip(Control *p_control) {
	bool valid = false;
	const Variant value = p_control->get(SNAME("tooltip_text"), &valid);
	if (valid && value.get_type() == Variant::STRING) {
		return value;
	}
	return String();
}

bool control_disabled(Control *p_control) {
	bool valid = false;
	const Variant value = p_control->get(SNAME("disabled"), &valid);
	return valid && value.get_type() == Variant::BOOL && (bool)value;
}

// A row's own coordinates lifted into the window's, the way the engine does it for its
// own row popups (see `AnimationLibraryEditor`): through the control's screen transform,
// so a scaled editor theme is carried along rather than silently dropped.
Rect2 to_window_space(Control *p_control, const Rect2 &p_rect) {
	const Transform2D transform = p_control->get_screen_transform();
	return Rect2(transform.xform(p_rect.position), transform.get_scale() * p_rect.size);
}

void write_rect(Dictionary &r_entry, const Rect2 &p_rect, const Point2i &p_origin) {
	const Point2 position = p_rect.position + p_origin;
	r_entry["x"] = (int)Math::round(position.x);
	r_entry["y"] = (int)Math::round(position.y);
	r_entry["width"] = (int)Math::round(p_rect.size.width);
	r_entry["height"] = (int)Math::round(p_rect.size.height);
	r_entry["center_x"] = (int)Math::round(position.x + p_rect.size.width * 0.5);
	r_entry["center_y"] = (int)Math::round(position.y + p_rect.size.height * 0.5);
}

struct Query {
	String text;
	String name;
	String class_name;
	String tooltip;
	String window;
	bool visible_only = true;
	int limit = 20;

	bool matches_text(const String &p_candidate) const {
		return !text.is_empty() && p_candidate.strip_edges().to_lower() == text;
	}
};

class FindControlTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_FindControl"; }
	virtual String get_description() const override {
		return "Find parts of the editor's own interface by what they say, and report where "
			   "they are on screen. Searches Controls by text, name, class or tooltip, and also "
			   "searches the rows of Tree and ItemList widgets - an approvals list's rows and "
			   "the buttons inside them are not nodes, so nothing else can locate them. "
			   "Returns screen rectangles and centres, ready to aim a pointer at. This is for "
			   "the editor's UI; to find something in the running game use "
			   "Godot_GetRuntimeNodeInfo.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["text"] = MCPSchema::string_property(
				"Visible text to match, case-insensitively and in full. Also matches a Tree or "
				"ItemList row's text.");
		properties["name"] = MCPSchema::string_property("Node name to match exactly.");
		properties["class"] = MCPSchema::string_property(
				"Class to match, including subclasses - 'Button' also finds a CheckBox.");
		properties["tooltip"] = MCPSchema::string_property(
				"Tooltip text to match, case-insensitively and in full. Often the only label an "
				"icon-only button has.");
		properties["window"] = MCPSchema::string_property(
				"Restrict the search to the window with this title, as reported by "
				"Godot_ListWindows.");
		properties["visible_only"] = MCPSchema::bool_property(
				"Skip anything not currently on screen. Defaults to true.", true);
		properties["limit"] = MCPSchema::integer_property("Most matches to return.", 20);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["matches"] = MCPSchema::array_property(
				"What was found, in tree order, with screen coordinates.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["count"] = MCPSchema::integer_property("How many matches are reported.");
		properties["truncated"] = MCPSchema::bool_property("True when `limit` cut the list short.");
		return MCPSchema::object_schema(properties);
	}

	static void search_tree_items(Tree *p_tree, TreeItem *p_item, const Query &p_query,
			const Point2i &p_origin, const String &p_window, Array &r_matches) {
		for (TreeItem *item = p_item; item; item = item->get_next_visible()) {
			for (int column = 0; column < p_tree->get_columns(); column++) {
				const String text = item->get_text(column);
				if (!p_query.matches_text(text)) {
					continue;
				}
				Dictionary entry;
				entry["kind"] = "tree_item";
				entry["node_path"] = String(p_tree->get_path());
				entry["class"] = p_tree->get_class();
				entry["text"] = text;
				entry["column"] = column;
				entry["window"] = p_window;
				write_rect(entry, to_window_space(p_tree, p_tree->get_item_rect(item, column)),
						p_origin);

				// The buttons drawn inside a row are the reason this exists. They are not
				// nodes, they carry no text, and their label is only a tooltip - so
				// without their rectangles the only way to press one is to guess at an
				// offset into the row.
				//
				// They are collected from every column, not from the one whose text
				// matched. A row is found by what it says in its Name column and acted
				// on through its Action column; looking for buttons only where the text
				// matched finds none, every time.
				Array buttons;
				for (int other = 0; other < p_tree->get_columns(); other++) {
					for (int index = 0; index < item->get_button_count(other); index++) {
						Dictionary button;
						button["column"] = other;
						button["index"] = index;
						button["tooltip"] = item->get_button_tooltip_text(other, index);
						write_rect(button,
								to_window_space(p_tree, p_tree->get_item_rect(item, other, index)),
								p_origin);
						buttons.push_back(button);
					}
				}
				entry["buttons"] = buttons;
				r_matches.push_back(entry);
				break;
			}
		}
	}

	static void search_list_items(ItemList *p_list, const Query &p_query, const Point2i &p_origin,
			const String &p_window, Array &r_matches) {
		for (int index = 0; index < p_list->get_item_count(); index++) {
			const String text = p_list->get_item_text(index);
			if (!p_query.matches_text(text)) {
				continue;
			}
			Dictionary entry;
			entry["kind"] = "list_item";
			entry["node_path"] = String(p_list->get_path());
			entry["class"] = p_list->get_class();
			entry["text"] = text;
			entry["index"] = index;
			entry["window"] = p_window;
			write_rect(entry, to_window_space(p_list, p_list->get_item_rect(index)), p_origin);
			r_matches.push_back(entry);
		}
	}

	static void search(Node *p_node, const Query &p_query, Array &r_matches) {
		if (!p_node || r_matches.size() > p_query.limit) {
			return;
		}

		Control *control = Object::cast_to<Control>(p_node);
		if (control && control->is_inside_tree()) {
			const bool visible = control->is_visible_in_tree();
			if (!p_query.visible_only || visible) {
				Window *window = containing_window(control);
				const String window_title = window ? window->get_title() : String();
				if (p_query.window.is_empty() || window_title == p_query.window) {
					const Point2i origin = screen_origin_correction(control);
					const String text = control_text(control);
					const String tooltip = control_tooltip(control);

					bool matched = true;
					if (!p_query.text.is_empty()) {
						matched = matched && p_query.matches_text(text);
					}
					if (!p_query.name.is_empty()) {
						matched = matched && String(control->get_name()) == p_query.name;
					}
					if (!p_query.class_name.is_empty()) {
						matched = matched && control->is_class(p_query.class_name);
					}
					if (!p_query.tooltip.is_empty()) {
						matched = matched && tooltip.strip_edges().to_lower() == p_query.tooltip;
					}

					if (matched) {
						Dictionary entry;
						entry["kind"] = "control";
						entry["node_path"] = String(control->get_path());
						entry["name"] = String(control->get_name());
						entry["class"] = control->get_class();
						entry["text"] = text;
						entry["tooltip"] = tooltip;
						entry["visible"] = visible;
						entry["disabled"] = control_disabled(control);
						entry["window"] = window_title;
						// Not get_global_rect(): that is in the control's own viewport, which
						// for anything inside an embedded dialog is the dialog. The screen
						// position has already walked out through the embedders.
						write_rect(entry, Rect2(control->get_screen_position(), control->get_size()),
								origin);
						r_matches.push_back(entry);
					}

					// Rows are searched by text only. Matching them on class or name would
					// mean answering about the widget while pointing at one of its rows.
					if (!p_query.text.is_empty() && p_query.name.is_empty() &&
							p_query.class_name.is_empty() && p_query.tooltip.is_empty()) {
						Tree *tree = Object::cast_to<Tree>(control);
						if (tree && tree->get_root()) {
							search_tree_items(tree, tree->get_root(), p_query, origin, window_title,
									r_matches);
						}
						ItemList *list = Object::cast_to<ItemList>(control);
						if (list) {
							search_list_items(list, p_query, origin, window_title, r_matches);
						}
					}
				}
			}
		}

		for (int i = 0; i < p_node->get_child_count(); i++) {
			search(p_node->get_child(i), p_query, r_matches);
		}
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "'Godot_FindControl' needs a running Godot editor");
			return Dictionary();
		}

		Query query;
		query.text = String(p_arguments.get("text", String())).strip_edges().to_lower();
		query.name = String(p_arguments.get("name", String())).strip_edges();
		query.class_name = String(p_arguments.get("class", String())).strip_edges();
		query.tooltip = String(p_arguments.get("tooltip", String())).strip_edges().to_lower();
		query.window = String(p_arguments.get("window", String())).strip_edges();
		query.visible_only = (bool)p_arguments.get("visible_only", true);
		query.limit = MAX(1, (int)p_arguments.get("limit", 20));

		if (query.text.is_empty() && query.name.is_empty() && query.class_name.is_empty() &&
				query.tooltip.is_empty()) {
			// Without a criterion this would answer with every Control in the editor -
			// thousands of them, and an answer that large is the same as no answer.
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					"give at least one of 'text', 'name', 'class' or 'tooltip'");
			return Dictionary();
		}

		SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		if (!tree || !tree->get_root()) {
			r_error.set(MCPToolError::UNSUPPORTED, "the editor has no scene tree");
			return Dictionary();
		}

		Array matches;
		search(tree->get_root(), query, matches);

		const bool truncated = matches.size() > query.limit;
		if (truncated) {
			matches.resize(query.limit);
		}

		Dictionary result;
		result["matches"] = matches;
		result["count"] = matches.size();
		result["truncated"] = truncated;
		return result;
	}
};

} // namespace

void mcp_register_editor_ui_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(FindControlTool)));
}
