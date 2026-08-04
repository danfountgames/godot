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
//
// And then acting on what was found. Editor input is kept in its own tool, with its own
// name and its own place in the documentation, for the same reason runtime property
// edits are kept apart from persistent ones: clicking the editor changes the project,
// clicking the game does not, and a caller must never have to infer which it just did.

#include "mcp_builtin_tools.h"

#include "../mcp_tool_registry.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/object/object.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/variant/array.h"
#include "scene/gui/control.h"
#include "scene/gui/item_list.h"
#include "scene/gui/tree.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"

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

// The screen rectangle a window occupies, in the same coordinates Godot_FindControl
// reports. An embedded window is drawn inside its parent, so its own position is
// relative to that parent's client area and the native origin has to be folded in.
Rect2i window_screen_rect(Window *p_window) {
	Point2i origin = p_window->get_position();
	if (p_window->is_embedded()) {
		for (Node *node = p_window->get_parent(); node; node = node->get_parent()) {
			Window *parent = Object::cast_to<Window>(node);
			if (parent && !parent->is_embedded()) {
				origin += parent->get_position();
				break;
			}
		}
	}
	return Rect2i(origin, p_window->get_size());
}

// Which window a screen point belongs to, and where in the *native* window's client
// area it lands.
//
// Native windows and embedded ones need opposite treatment, and both occur: the editor
// gives its dialogs real OS windows normally and embeds them in single-window mode.
// A native dialog is addressed by its own window id with coordinates relative to
// itself; an embedded one has no window id of its own, so the event is addressed to the
// window it is embedded in and the root viewport forwards it on by position.
struct Target {
	Window *window = nullptr;
	DisplayServerEnums::WindowID window_id = DisplayServerEnums::INVALID_WINDOW_ID;
	Point2i local;
};

void collect_windows(Node *p_node, Vector<Window *> &r_windows) {
	Window *window = Object::cast_to<Window>(p_node);
	if (window && window->is_visible()) {
		r_windows.push_back(window);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_windows(p_node->get_child(i), r_windows);
	}
}

bool resolve_target(const Point2i &p_point, const String &p_window_title, Target &r_target,
		String &r_error) {
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	if (!tree || !tree->get_root()) {
		r_error = "the editor has no scene tree";
		return false;
	}

	Vector<Window *> windows;
	collect_windows(tree->get_root(), windows);

	Window *hit = nullptr;
	Vector<String> titles;
	for (Window *window : windows) {
		const Rect2i rect = window_screen_rect(window);
		titles.push_back(vformat("'%s' at (%d, %d) %dx%d", window->get_title(), rect.position.x,
				rect.position.y, rect.size.width, rect.size.height));
		if (!p_window_title.is_empty() && window->get_title() != p_window_title) {
			continue;
		}
		if (rect.has_point(p_point)) {
			// Later in tree order wins. Dialogs are created after the editor they open
			// over, so this picks the dialog rather than what is behind it - and a click
			// meant for a dialog that landed behind it would be worse than no click,
			// because it would be a change to the project nobody asked for.
			hit = window;
		}
	}

	if (!hit) {
		if (!p_window_title.is_empty()) {
			r_error = vformat("(%d, %d) is not inside a visible window titled '%s'", p_point.x,
					p_point.y, p_window_title);
		} else {
			r_error = vformat("(%d, %d) is not inside any of the editor's windows: %s", p_point.x,
					p_point.y, String(", ").join(titles));
		}
		return false;
	}

	// Walk out to the window that owns an OS window: that is the one the display server
	// can deliver to, and the coordinate space the event has to be expressed in.
	Window *native = hit;
	while (native && native->is_embedded()) {
		native = containing_window(native->get_parent());
	}
	if (!native) {
		r_error = "the window at that point is not attached to anything the display server owns";
		return false;
	}

	r_target.window = hit;
	r_target.window_id = native->get_window_id();
	r_target.local = p_point - native->get_position();
	return true;
}

class SendEditorInputTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SendEditorInput"; }
	virtual String get_description() const override {
		return "Send a real pointer or keyboard event to the *editor's* interface - a dialog "
			   "button, a menu, a panel. Coordinates are screen coordinates, exactly as "
			   "Godot_FindControl and Godot_ListWindows report them. This drives the editor, "
			   "not the running game: for the game use Godot_SendPointerInput and "
			   "Godot_SendKeyInput, which are deliberately separate tools. Treat it as you "
			   "would a hand on the mouse - it can open menus and confirm dialogs, and a click "
			   "aimed at a stale coordinate lands on whatever is there now.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_SIMULATE_INPUT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property(
				"move, press, release, click, drag, scroll, type, key_press, key_release or "
				"key_tap.",
				"click");
		properties["x"] = MCPSchema::integer_property("Screen x, for the pointer actions.", 0);
		properties["y"] = MCPSchema::integer_property("Screen y, for the pointer actions.", 0);
		properties["to_x"] = MCPSchema::integer_property("Screen x where a drag ends.", 0);
		properties["to_y"] = MCPSchema::integer_property("Screen y where a drag ends.", 0);
		properties["steps"] = MCPSchema::integer_property(
				"How many motion events a drag is broken into.", 8);
		Vector<String> directions;
		directions.push_back("up");
		directions.push_back("down");
		directions.push_back("left");
		directions.push_back("right");
		properties["direction"] = MCPSchema::enum_property("Which way to scroll.", directions, "down");
		properties["amount"] = MCPSchema::integer_property("How many wheel notches to send.", 3);
		properties["button"] = MCPSchema::integer_property(
				"Mouse button: 1 left, 2 right, 3 middle.", 1);
		properties["text"] = MCPSchema::string_property("Text to type, for 'type'.");
		properties["key"] = MCPSchema::string_property(
				"Key name for the key actions, such as Enter, Escape or F5.");
		properties["window"] = MCPSchema::string_property(
				"Require the point to be inside the window with this title. Without it the "
				"topmost window containing the point receives the event.");
		properties["shift"] = MCPSchema::bool_property("Hold Shift.", false);
		properties["ctrl"] = MCPSchema::bool_property("Hold Ctrl.", false);
		properties["alt"] = MCPSchema::bool_property("Hold Alt.", false);
		properties["meta"] = MCPSchema::bool_property("Hold Meta/Super/Command.", false);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property("The action that was performed.");
		properties["events"] = MCPSchema::integer_property("How many input events were delivered.");
		properties["window"] = MCPSchema::string_property("Title of the window that received them.");
		properties["window_x"] = MCPSchema::integer_property(
				"Where the event landed inside the receiving native window.");
		properties["window_y"] = MCPSchema::integer_property(
				"Where the event landed inside the receiving native window.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED,
					"'Godot_SendEditorInput' needs a running Godot editor");
			return Dictionary();
		}
		DisplayServer *display = DisplayServer::get_singleton();
		if (!display || display->get_name() == "headless") {
			// A headless editor has no display server dispatch function, so events would
			// vanish silently. Saying so beats reporting a delivery that never happened.
			r_error.set(MCPToolError::UNSUPPORTED,
					"this editor is running headless, so nothing can receive input");
			return Dictionary();
		}
		Input *input = Input::get_singleton();
		if (!input) {
			r_error.set(MCPToolError::UNSUPPORTED, "this editor has no input singleton");
			return Dictionary();
		}

		const String action = String(p_arguments.get("action", "click")).strip_edges();
		const bool shift = (bool)p_arguments.get("shift", false);
		const bool ctrl = (bool)p_arguments.get("ctrl", false);
		const bool alt = (bool)p_arguments.get("alt", false);
		const bool meta = (bool)p_arguments.get("meta", false);
		const String window_title = String(p_arguments.get("window", String())).strip_edges();

		const bool is_pointer = action == "move" || action == "press" || action == "release" ||
				action == "click" || action == "drag" || action == "scroll";
		const bool is_key = action == "type" || action == "key_press" ||
				action == "key_release" || action == "key_tap";
		if (!is_pointer && !is_key) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("unknown action '%s'; expected move, press, release, click, drag, "
							"scroll, type, key_press, key_release or key_tap",
							action));
			return Dictionary();
		}

		Dictionary result;
		result["action"] = action;
		int events = 0;

		if (is_pointer) {
			const Point2i point((int)p_arguments.get("x", 0), (int)p_arguments.get("y", 0));
			Target target;
			String error;
			if (!resolve_target(point, window_title, target, error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
			}
			const int button = (int)p_arguments.get("button", 1);
			if (button < 1 || button > 3) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS,
						vformat("button %d is not 1 (left), 2 (right) or 3 (middle)", button));
				return Dictionary();
			}

			// A drag's destination is resolved the same way its start was, so it is
			// checked against the same window rather than assumed to be in it.
			Point2i local_end = target.local;
			if (action == "drag") {
				const Point2i end((int)p_arguments.get("to_x", 0), (int)p_arguments.get("to_y", 0));
				Target destination;
				String end_error;
				if (!resolve_target(end, window_title, destination, end_error)) {
					r_error.set(MCPToolError::INVALID_ARGUMENTS, end_error);
					return Dictionary();
				}
				if (destination.window_id != target.window_id) {
					r_error.set(MCPToolError::INVALID_ARGUMENTS,
							"a drag has to start and end in the same window");
					return Dictionary();
				}
				local_end = destination.local;
			}

			// Tracked locally, not read back from Input: `relative` is the whole content
			// of a drag, and a delta against a position that has not caught up is zero.
			Vector2 previous = input->get_mouse_position();
			BitField<MouseButtonMask> held;

			auto motion_to = [&](const Vector2 &p_to) {
				Ref<InputEventMouseMotion> event;
				event.instantiate();
				event->set_window_id(target.window_id);
				event->set_position(p_to);
				event->set_global_position(p_to);
				event->set_relative(p_to - previous);
				event->set_button_mask(held);
				input->parse_input_event(event);
				previous = p_to;
			};
			auto press_at = [&](const Vector2 &p_at, bool p_pressed) {
				held.clear();
				if (p_pressed) {
					held.set_flag(mouse_button_to_mask((MouseButton)button));
				}
				Ref<InputEventMouseButton> event;
				event.instantiate();
				event->set_window_id(target.window_id);
				event->set_position(p_at);
				event->set_global_position(p_at);
				event->set_button_index((MouseButton)button);
				event->set_pressed(p_pressed);
				event->set_button_mask(held);
				event->set_shift_pressed(shift);
				event->set_ctrl_pressed(ctrl);
				event->set_alt_pressed(alt);
				event->set_meta_pressed(meta);
				input->parse_input_event(event);
			};
			const Vector2 local = target.local;
			auto motion = [&]() { motion_to(local); };
			auto press = [&](bool p_pressed) { press_at(local, p_pressed); };

			if (action == "move") {
				motion();
				events = 1;
			} else if (action == "press") {
				motion();
				press(true);
				events = 2;
			} else if (action == "release") {
				press(false);
				events = 1;
			} else if (action == "drag") {
				const int steps = MAX(1, (int)p_arguments.get("steps", 8));
				// Positioning the cursor at the gesture's start is setup, not part of
				// the drag distance. On Godot 4.8 the GUI pipeline can apply the held
				// state before this queued motion reaches the Control, so make that
				// setup event explicitly zero-distance as well as unpressed.
				previous = local;
				motion();
				// Settle the unpressed positioning event before changing the mouse
				// state. Godot 4.8 otherwise buffers it until the first interpolated
				// motion and replays the cursor jump as part of the held drag.
				input->flush_buffered_events();
				press(true);
				for (int step = 1; step <= steps; step++) {
					motion_to(local.lerp(Vector2(local_end), (float)step / (float)steps));
					// Flushed each time, or the engine coalesces the whole drag into one
					// motion event with the summed delta and `steps` means nothing.
					input->flush_buffered_events();
				}
				press_at(local_end, false);
				events = 3 + steps;
			} else if (action == "scroll") {
				const String direction = String(p_arguments.get("direction", "down"));
				const int amount = MAX(1, (int)p_arguments.get("amount", 3));
				MouseButton wheel = MouseButton::WHEEL_DOWN;
				if (direction == "up") {
					wheel = MouseButton::WHEEL_UP;
				} else if (direction == "down") {
					wheel = MouseButton::WHEEL_DOWN;
				} else if (direction == "left") {
					wheel = MouseButton::WHEEL_LEFT;
				} else if (direction == "right") {
					wheel = MouseButton::WHEEL_RIGHT;
				} else {
					r_error.set(MCPToolError::INVALID_ARGUMENTS,
							vformat("unknown scroll direction '%s'; expected up, down, left or right",
									direction));
					return Dictionary();
				}
				motion();
				// A notch is a press and a release of a wheel button; a press alone
				// leaves every scroll handler waiting for the other half.
				for (int tick = 0; tick < amount; tick++) {
					for (int pressed = 1; pressed >= 0; pressed--) {
						Ref<InputEventMouseButton> event;
						event.instantiate();
						event->set_window_id(target.window_id);
						event->set_position(local);
						event->set_global_position(local);
						event->set_button_index(wheel);
						event->set_pressed(pressed == 1);
						event->set_factor(1.0f);
						BitField<MouseButtonMask> wheel_mask;
						if (pressed == 1) {
							wheel_mask.set_flag(mouse_button_to_mask(wheel));
						}
						event->set_button_mask(wheel_mask);
						input->parse_input_event(event);
					}
				}
				events = 1 + amount * 2;
			} else {
				// A click is a press and a release. Sending only the press leaves the
				// control latched, and every later interaction inherits that.
				motion();
				press(true);
				press(false);
				events = 3;
			}

			result["window"] = target.window->get_title();
			result["window_x"] = target.local.x;
			result["window_y"] = target.local.y;
		} else {
			// Keyboard input follows focus, not coordinates: whichever control the editor
			// considers focused receives it. Addressing it to a window the user is not
			// typing in would be a lie dressed as precision.
			DisplayServerEnums::WindowID window_id = display->get_focused_window();
			if (window_id == DisplayServerEnums::INVALID_WINDOW_ID) {
				window_id = DisplayServerEnums::MAIN_WINDOW_ID;
			}

			auto key_event = [&](Key p_keycode, char32_t p_unicode, bool p_pressed) {
				Ref<InputEventKey> event;
				event.instantiate();
				event->set_window_id(window_id);
				event->set_keycode(p_keycode);
				event->set_physical_keycode(p_keycode);
				event->set_unicode(p_unicode);
				event->set_pressed(p_pressed);
				event->set_shift_pressed(shift);
				event->set_ctrl_pressed(ctrl);
				event->set_alt_pressed(alt);
				event->set_meta_pressed(meta);
				input->parse_input_event(event);
			};

			if (action == "type") {
				const String text = p_arguments.get("text", String());
				if (text.is_empty()) {
					r_error.set(MCPToolError::INVALID_ARGUMENTS, "typing needs some text");
					return Dictionary();
				}
				for (int i = 0; i < text.length(); i++) {
					const char32_t character = text[i];
					Key keycode = Key::NONE;
					if (character >= 'a' && character <= 'z') {
						keycode = (Key)((char32_t)Key::A + (character - 'a'));
					} else if (character >= 'A' && character <= 'Z') {
						keycode = (Key)((char32_t)Key::A + (character - 'A'));
					} else if (character >= '0' && character <= '9') {
						keycode = (Key)((char32_t)Key::KEY_0 + (character - '0'));
					} else if (character == ' ') {
						keycode = Key::SPACE;
					}
					// The unicode value is what a LineEdit reads; the keycode is what a
					// shortcut matches. Both are needed, for the same reason as in the
					// game-side tool.
					key_event(keycode, character, true);
					key_event(keycode, character, false);
					events += 2;
				}
				result["text"] = text;
			} else {
				const String key_name = String(p_arguments.get("key", String())).strip_edges();
				if (key_name.is_empty()) {
					r_error.set(MCPToolError::INVALID_ARGUMENTS,
							"the key actions need a key name, such as Enter or Escape");
					return Dictionary();
				}
				Key keycode = find_keycode(key_name);
				if (keycode == Key::NONE) {
					// The engine calls it Enter, not Return; Escape, not Esc. Accept both.
					const String lowered = key_name.to_lower();
					if (lowered == "return") {
						keycode = Key::ENTER;
					} else if (lowered == "esc") {
						keycode = Key::ESCAPE;
					} else if (lowered == "del") {
						keycode = Key::KEY_DELETE;
					}
				}
				if (keycode == Key::NONE) {
					r_error.set(MCPToolError::INVALID_ARGUMENTS,
							vformat("'%s' is not a key name this engine recognises; try Enter, "
									"Escape, Space, Tab, Backspace, Delete or a letter",
									key_name));
					return Dictionary();
				}
				if (action == "key_press") {
					key_event(keycode, 0, true);
					events = 1;
				} else if (action == "key_release") {
					key_event(keycode, 0, false);
					events = 1;
				} else {
					key_event(keycode, 0, true);
					key_event(keycode, 0, false);
					events = 2;
				}
				result["key"] = key_name;
			}

			Window *focused = nullptr;
			SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
			if (tree && tree->get_root()) {
				Vector<Window *> windows;
				collect_windows(tree->get_root(), windows);
				for (Window *window : windows) {
					if (window->get_window_id() == window_id) {
						focused = window;
					}
				}
			}
			result["window"] = focused ? focused->get_title() : String();
			result["window_x"] = 0;
			result["window_y"] = 0;
		}

		result["events"] = events;
		return result;
	}
};

} // namespace

void mcp_register_editor_ui_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(FindControlTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SendEditorInputTool)));
}
