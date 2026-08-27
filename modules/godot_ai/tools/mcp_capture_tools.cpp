/**************************************************************************/
/*  mcp_capture_tools.cpp                                                 */
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

#include "../mcp_capture_metadata.h"
#include "../mcp_deferred.h"
#include "../mcp_paths.h"
#include "../mcp_tool_registry.h"

#include "core/config/engine.h"
#include "core/os/os.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/time.h"
#include "core/variant/array.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_interface.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/editor_node.h"
#include "editor/inspector/editor_properties.h"
#include "editor/scene/editor_scene_tabs.h"
#include "editor/scene/scene_tree_editor.h"
#include "editor/docks/inspector_dock.h"
#include "editor/docks/scene_tree_dock.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/tab_container.h"
#include "scene/main/viewport.h"
#include "servers/display/display_server.h"

namespace {

// How long to wait for a software-rendered editor to draw the handful of frames the
// capture tools need. Generous on purpose: their pollers answer the moment they are
// ready, so a long budget costs nothing when the machine is quick, and a short one turns
// a slow machine into a failed test. Ten seconds was enough on a developer's desktop and
// not enough on a two-core CI runner under llvmpipe, which is where this was found.
constexpr uint64_t CAPTURE_BUDGET_MSEC = 90000;

// The deferred layer's own deadline is only a backstop; the budget above fires first, so
// the caller is told which stage stalled rather than just that something did.
constexpr double DEFERRED_BACKSTOP_SECONDS = 120.0;


// Returning the image inline is what makes this useful to a model, but a screenshot
// is large; anything bigger than this is saved to disk and referenced by path only.
static const int MAX_INLINE_IMAGE_BYTES = 3 * 1024 * 1024;

static void add_inline_image(const Ref<Image> &p_image, Dictionary &r_result) {
	const Vector<uint8_t> png = p_image->save_png_to_buffer();
	if (png.is_empty() || png.size() > MAX_INLINE_IMAGE_BYTES) {
		return;
	}

	Dictionary image_content;
	image_content["type"] = "image";
	image_content["data"] = CryptoCore::b64_encode_str(png.ptr(), png.size());
	image_content["mimeType"] = "image/png";

	Array content;
	content.push_back(image_content);
	r_result["_content"] = content;
	r_result["inlined"] = true;
}

static EditorProperty *find_editor_property(Node *p_root, Object *p_object, const StringName &p_property) {
	if (!p_root) {
		return nullptr;
	}
	if (EditorProperty *editor_property = Object::cast_to<EditorProperty>(p_root)) {
		if (editor_property->get_edited_object() == p_object && editor_property->get_edited_property() == p_property) {
			return editor_property;
		}
	}
	for (int i = 0; i < p_root->get_child_count(); i++) {
		if (EditorProperty *found = find_editor_property(p_root->get_child(i), p_object, p_property)) {
			return found;
		}
	}
	return nullptr;
}

static EditorInspector *find_sub_inspector(Node *p_root, Object *p_object) {
	if (!p_root) {
		return nullptr;
	}
	if (EditorInspector *inspector = Object::cast_to<EditorInspector>(p_root)) {
		if (inspector->is_sub_inspector() && inspector->get_edited_object() == p_object) {
			return inspector;
		}
	}
	for (int i = 0; i < p_root->get_child_count(); i++) {
		if (EditorInspector *found = find_sub_inspector(p_root->get_child(i), p_object)) {
			return found;
		}
	}
	return nullptr;
}

static TabContainer *find_parent_tab_container(Node *p_node) {
	for (Node *node = p_node ? p_node->get_parent() : nullptr; node; node = node->get_parent()) {
		if (TabContainer *tabs = Object::cast_to<TabContainer>(node)) {
			return tabs;
		}
	}
	return nullptr;
}

class CaptureViewportTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CaptureViewport"; }
	virtual String get_description() const override {
		return "Capture what the editor is currently rendering, as a PNG saved in the project "
			   "and returned inline when small enough. Requires a real display: a headless "
			   "editor has nothing to capture.";
	}
	// Reads the editor's own screen, and writes the PNG into the project.
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property(
				"Where to save the PNG, as a res:// path. Defaults to a timestamped file "
				"under res://ai_screenshots/.",
				"");
		properties["inline_image"] = MCPSchema::bool_property(
				"Also return the image in the response, when it is small enough.", true);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Where the PNG was saved.");
		properties["width"] = MCPSchema::integer_property("Image width in pixels.");
		properties["height"] = MCPSchema::integer_property("Image height in pixels.");
		properties["inlined"] = MCPSchema::bool_property("True when the image is also in the response.");
		MCPCaptureMetadata::declare(properties);
		return MCPSchema::object_schema(properties);
	}

	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		// has() first: Dictionary::operator[] inserts a null for a missing key even
		// through a const reference, and these arguments have not been validated yet -
		// an inserted null would then fail schema validation as a wrongly typed value.
		const String path = p_arguments.has("path") ? String(p_arguments["path"]).strip_edges() : String();
		if (!path.is_empty()) {
			paths.push_back(path);
		}
		// A generated timestamped filename cannot collide with anything that already
		// exists, so there is nothing to snapshot in that case.
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		// The headless display server renders nothing, so a capture would be a blank
		// image presented as if it were the editor. Refuse instead - and say what to do
		// about it, because on a machine with no display this is fixable rather than
		// fatal: the repository ships a virtual display for exactly this case.
		if (DisplayServer::get_singleton() && DisplayServer::get_singleton()->get_name() == "headless") {
			r_error.set(MCPToolError::UNSUPPORTED,
					"this editor is running headless, so there is nothing on screen to capture; "
					"relaunch it with a display - on a machine without one, "
					"`python3 tools/virtual_display.py -- <godot binary> --path <project> --editor` "
					"starts a virtual display and the correct renderer for it");
			return Dictionary();
		}

		Viewport *viewport = EditorNode::get_singleton()->get_viewport();
		if (!viewport) {
			r_error.set(MCPToolError::INVALID_STATE, "the editor has no viewport to capture");
			return Dictionary();
		}
		const Ref<Image> image = viewport->get_texture().is_valid() ? viewport->get_texture()->get_image() : Ref<Image>();
		if (image.is_null() || image->is_empty()) {
			r_error.set(MCPToolError::FAILED,
					"the editor viewport produced no image; the renderer may not have drawn a frame yet");
			return Dictionary();
		}

		// get(), not subscript: reading a missing optional key off a const Dictionary
		// inserts a null into it.
		String requested = String(p_arguments.get("path", String())).strip_edges();
		if (requested.is_empty()) {
			requested = "res://ai_screenshots/" +
					Time::get_singleton()->get_datetime_string_from_system(false, false)
							.replace(":", "")
							.replace("-", "")
							.replace(" ", "-") +
					".png";
		}
		if (requested.get_extension().to_lower() != "png") {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "the capture path must end in .png");
			return Dictionary();
		}

		MCPPaths::Resolved resolved;
		String path_error;
		if (!MCPPaths::resolve(requested, resolved, path_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, path_error);
			return Dictionary();
		}

		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (dir.is_valid()) {
			dir->make_dir_recursive(resolved.absolute.get_base_dir());
		}
		if (image->save_png(resolved.absolute) != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not write '%s'", resolved.res_path));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = resolved.res_path;
		result["width"] = image->get_width();
		result["height"] = image->get_height();
		result["inlined"] = false;
		MCPCaptureMetadata::stamp(result, MCPCaptureMetadata::SOURCE_EDITOR_VIEWPORT,
				"the editor's main viewport");
		MCPCaptureMetadata::add_note(result,
				"This is the editor, not the running game. It shows nothing about how the game "
				"plays, and it does not contain dialogs or popups, which are separate windows.");

		if ((bool)p_arguments.get("inline_image", true)) {
			add_inline_image(image, result);
		}
		return result;
	}
};

class CaptureInspectorPropertyTool : public MCPTool {
	enum Stage {
		IDLE,
		WAITING_FOR_INSPECTOR,
		WAITING_FOR_LAYOUT,
		WAITING_FOR_RENDER,
	};

	struct ResourceFoldState {
		ObjectID object;
		StringName property;
		bool unfolded = false;
	};

	struct SectionFoldState {
		ObjectID section;
		bool unfolded = false;
	};

	Stage stage = IDLE;
	// When the operation started, and how many frames had been drawn then. Every stage
	// below advances on drawn frames, so a stall is measured in frames, not seconds -
	// and saying which of the two ran out is the difference between a diagnosis and
	// "timed out".
	uint64_t started_msec = 0;
	uint64_t frames_at_start = 0;
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	Ref<Resource> resource;
	Ref<Resource> original_resource;
	ObjectID inspected_object;
	Array property_chain;
	Array context_properties;
	String resource_path;
	String scene_path;
	String node_path;
	MCPPaths::Resolved output_path;
	int context_above = 120;
	int context_below = 160;
	bool inline_image = true;
	ObjectID original_object;
	int original_scene_index = -1;
	String original_selected_path;
	int original_scroll = 0;
	ObjectID original_tab_container;
	int original_tab = -1;
	bool original_docks_visible = true;
	ObjectID target_property;
	uint64_t inspect_after_frame = 0;
	uint64_t capture_after_frame = 0;
	Vector<ResourceFoldState> resource_folds;
	Vector<SectionFoldState> section_folds;

	Object *_get_inspected_object() const {
		return ObjectDB::get_instance(inspected_object);
	}

	void _remember_and_unfold_sections(EditorProperty *p_property) {
		for (Node *node = p_property ? p_property->get_parent() : nullptr; node; node = node->get_parent()) {
			EditorInspectorSection *section = Object::cast_to<EditorInspectorSection>(node);
			if (!section || !section->get_vbox()) {
				continue;
			}
			const ObjectID id = section->get_instance_id();
			bool known = false;
			for (const SectionFoldState &state : section_folds) {
				if (state.section == id) {
					known = true;
					break;
				}
			}
			if (!known) {
				SectionFoldState state;
				state.section = id;
				state.unfolded = section->get_vbox()->is_visible();
				section_folds.push_back(state);
			}
			section->unfold();
		}
	}

	bool _resolve_property_chain(String &r_error) {
		EditorInspector *inspector = InspectorDock::get_inspector_singleton();
		Object *object = _get_inspected_object();
		if (!object) {
			r_error = "the object being inspected no longer exists";
			return false;
		}
		for (int i = 0; i < property_chain.size(); i++) {
			const StringName property = String(property_chain[i]);
			EditorProperty *editor_property = find_editor_property(inspector, object, property);
			if (!editor_property) {
				r_error = vformat("property '%s' was not rendered in the Inspector while resolving %s",
						String(property), _chain_to_string(i + 1));
				return false;
			}
			_remember_and_unfold_sections(editor_property);

			if (i == property_chain.size() - 1) {
				for (int context_index = 0; context_index < context_properties.size(); context_index++) {
					const StringName context_property = String(context_properties[context_index]);
					EditorProperty *context_editor_property = find_editor_property(inspector, object, context_property);
					if (!context_editor_property) {
						r_error = vformat("context property '%s' was not rendered beside target '%s'",
								String(context_property), String(property));
						return false;
					}
					_remember_and_unfold_sections(context_editor_property);
				}
				target_property = editor_property->get_instance_id();
				editor_property->select();
				return true;
			}

			EditorPropertyResource *resource_property = Object::cast_to<EditorPropertyResource>(editor_property);
			Ref<Resource> nested = editor_property->get_edited_property_value();
			if (!resource_property || nested.is_null()) {
				r_error = vformat("'%s' is not a non-null Resource property, so '%s' cannot be expanded beneath it",
						String(property), String(property_chain[i + 1]));
				return false;
			}

			ResourceFoldState fold;
			fold.object = object->get_instance_id();
			fold.property = property;
			fold.unfolded = object->editor_is_section_unfolded(property);
			resource_folds.push_back(fold);
			object->editor_set_section_unfold(property, true);
			resource_property->set_use_sub_inspector(true);
			resource_property->update_property();

			EditorInspector *nested_inspector = find_sub_inspector(resource_property, nested.ptr());
			if (!nested_inspector) {
				r_error = vformat("the Inspector could not expand Resource property '%s'", String(property));
				return false;
			}
			inspector = nested_inspector;
			object = nested.ptr();
		}
		return false;
	}

	String _chain_to_string(int p_count = -1) const {
		String chain;
		const int count = p_count < 0 ? property_chain.size() : MIN(p_count, property_chain.size());
		for (int i = 0; i < count; i++) {
			if (i > 0) {
				chain += " -> ";
			}
			chain += String(property_chain[i]);
		}
		return chain;
	}

	String _stage_name() const {
		switch (stage) {
			case WAITING_FOR_INSPECTOR:
				return "waiting for the Inspector to show the property";
			case WAITING_FOR_LAYOUT:
				return "waiting for the Inspector to finish laying out";
			case WAITING_FOR_RENDER:
				return "waiting for the editor to draw the scrolled Inspector";
			default:
				return "idle";
		}
	}

	// True when the budget has run out; fills in a message that says what it was waiting
	// for and whether the editor drew anything at all while it waited.
	bool _budget_exhausted() {
		if (stage == IDLE || started_msec == 0) {
			return false;
		}
		const uint64_t elapsed = OS::get_singleton()->get_ticks_msec() - started_msec;
		if (elapsed < CAPTURE_BUDGET_MSEC) {
			return false;
		}
		const uint64_t drawn = Engine::get_singleton()->get_frames_drawn() - frames_at_start;
		_fail(MCPToolError::FAILED,
				vformat("gave up after %d ms while %s; the editor drew %d frame(s) in that time. "
						"A software-rendered editor on a slow machine can be this slow legitimately; "
						"zero frames means it was not drawing at all.",
						(int)elapsed, _stage_name(), (int)drawn));
		return true;
	}

	void _clear_operation() {
		stage = IDLE;
		started_msec = 0;
		frames_at_start = 0;
		token = MCPDeferred::INVALID_TOKEN;
		resource.unref();
		original_resource.unref();
		inspected_object = ObjectID();
		property_chain.clear();
		context_properties.clear();
		resource_path = String();
		scene_path = String();
		node_path = String();
		output_path = MCPPaths::Resolved();
		original_object = ObjectID();
		original_scene_index = -1;
		original_tab_container = ObjectID();
		target_property = ObjectID();
		inspect_after_frame = 0;
		resource_folds.clear();
		section_folds.clear();
	}

	void _restore_editor_state() {
		for (int i = section_folds.size() - 1; i >= 0; i--) {
			EditorInspectorSection *section = Object::cast_to<EditorInspectorSection>(ObjectDB::get_instance(section_folds[i].section));
			if (section) {
				section_folds[i].unfolded ? section->unfold() : section->fold();
			}
		}
		for (int i = resource_folds.size() - 1; i >= 0; i--) {
			Object *object = ObjectDB::get_instance(resource_folds[i].object);
			if (object) {
				object->editor_set_section_unfold(resource_folds[i].property, resource_folds[i].unfolded);
			}
		}

		if (original_scene_index >= 0 && EditorSceneTabs::get_singleton() &&
				EditorNode::get_editor_data().get_edited_scene() != original_scene_index) {
			EditorSceneTabs::get_singleton()->set_current_tab(original_scene_index);
		}

		InspectorDock *dock = InspectorDock::get_singleton();
		EditorInspector *inspector = InspectorDock::get_inspector_singleton();
		if (dock && inspector) {
			Object *previous = ObjectDB::get_instance(original_object);
			// A Ref keeps a previously inspected Resource alive even if opening the target
			// happened to release its last editor-owned reference.
			if (!previous && original_resource.is_valid()) {
				previous = original_resource.ptr();
			}
			if (previous != _get_inspected_object()) {
				inspector->edit(nullptr);
				dock->update(previous);
				inspector->edit(previous);
			} else {
				inspector->update_tree();
			}
			if (!original_selected_path.is_empty()) {
				if (EditorProperty *selected = find_editor_property(inspector, previous, original_selected_path)) {
					selected->select();
				}
			}
			inspector->set_scroll_offset(original_scroll);
		}

		if (TabContainer *tabs = Object::cast_to<TabContainer>(ObjectDB::get_instance(original_tab_container))) {
			if (original_tab >= 0 && original_tab < tabs->get_tab_count()) {
				tabs->set_current_tab(original_tab);
			}
		}
		if (EditorDockManager::get_singleton()) {
			EditorDockManager::get_singleton()->set_docks_visible(original_docks_visible);
		}
	}

	void _cancel() {
		if (stage == IDLE) {
			return;
		}
		_restore_editor_state();
		_clear_operation();
	}

	void _fail(MCPToolError::Kind p_kind, const String &p_message) {
		const MCPDeferred::Token pending = token;
		_restore_editor_state();
		_clear_operation();
		MCPDeferred::fail(pending, p_kind, p_message);
	}

	Dictionary _capture(MCPToolError &r_error) {
		InspectorDock *dock = InspectorDock::get_singleton();
		EditorProperty *target = Object::cast_to<EditorProperty>(ObjectDB::get_instance(target_property));
		Viewport *viewport = dock ? dock->get_viewport() : nullptr;
		if (!dock || !target || !viewport || !dock->is_visible_in_tree() || !target->is_visible_in_tree()) {
			r_error.set(MCPToolError::INVALID_STATE, "the Inspector property stopped being visible before it could be captured");
			return Dictionary();
		}

		const Ref<Image> screen = viewport->get_texture().is_valid() ? viewport->get_texture()->get_image() : Ref<Image>();
		if (screen.is_null() || screen->is_empty()) {
			r_error.set(MCPToolError::FAILED, "the Inspector viewport produced no image; the renderer may not have drawn a frame yet");
			return Dictionary();
		}

		const Rect2 viewport_rect = viewport->get_visible_rect();
		const Rect2 dock_rect = dock->get_global_rect().intersection(viewport_rect);
		const Rect2 target_rect = target->get_global_rect();
		if (dock_rect.size.x <= 1 || dock_rect.size.y <= 1 || !dock_rect.intersects(target_rect)) {
			r_error.set(MCPToolError::FAILED, "the requested property could not be scrolled into the visible Inspector dock");
			return Dictionary();
		}

		Rect2 logical_crop(
				Point2(dock_rect.position.x, target_rect.position.y - context_above),
				Size2(dock_rect.size.x, target_rect.size.y + context_above + context_below));
		logical_crop = logical_crop.intersection(dock_rect);
		const Vector2 scale((double)screen->get_width() / viewport_rect.size.x,
				(double)screen->get_height() / viewport_rect.size.y);
		const Point2 relative_begin = logical_crop.position - viewport_rect.position;
		const Point2 relative_end = logical_crop.get_end() - viewport_rect.position;
		Rect2i pixels(
				Point2i(Math::floor(relative_begin.x * scale.x), Math::floor(relative_begin.y * scale.y)),
				Point2i(Math::ceil(relative_end.x * scale.x) - Math::floor(relative_begin.x * scale.x),
						Math::ceil(relative_end.y * scale.y) - Math::floor(relative_begin.y * scale.y)));
		pixels = pixels.intersection(Rect2i(0, 0, screen->get_width(), screen->get_height()));
		if (pixels.size.x <= 1 || pixels.size.y <= 1) {
			r_error.set(MCPToolError::FAILED, "the Inspector crop was empty after clipping it to the rendered window");
			return Dictionary();
		}

		const Ref<Image> image = screen->get_region(pixels);
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (dir.is_valid()) {
			dir->make_dir_recursive(output_path.absolute.get_base_dir());
		}
		if (image.is_null() || image->save_png(output_path.absolute) != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not write '%s'", output_path.res_path));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = output_path.res_path;
		result["width"] = image->get_width();
		result["height"] = image->get_height();
		result["inlined"] = false;
		result["resource"] = resource_path;
		result["scene"] = scene_path;
		result["node_path"] = node_path;
		// Array is reference-counted. The operation state is cleared immediately after
		// capture, so the result needs its own copy.
		result["property_chain"] = property_chain.duplicate();
		result["context_properties"] = context_properties.duplicate();
		result["target_property"] = String(target->get_edited_property());
		result["target_label"] = target->get_label();
		result["target_y"] = (int)Math::round((target_rect.position.y - logical_crop.position.y) * scale.y);
		result["target_height"] = MAX(1, (int)Math::round(target_rect.size.y * scale.y));
		result["pixel_scale"] = scale.y;
		const String inspected_subject = !resource_path.is_empty() ? resource_path : scene_path + ":" + node_path;
		MCPCaptureMetadata::stamp(result, MCPCaptureMetadata::SOURCE_EDITOR_VIEWPORT,
				vformat("Inspector property %s in %s", _chain_to_string(), inspected_subject));
		MCPCaptureMetadata::add_note(result,
				"This is a tightly cropped editor Inspector image centered on the named property; "
				"it is not the game viewport or the whole editor window.");
		if (inline_image) {
			add_inline_image(image, result);
		}
		return result;
	}

	Variant _poll() {
		if (_budget_exhausted()) {
			return Variant();
		}
		if (stage == WAITING_FOR_INSPECTOR) {
			if (Engine::get_singleton()->get_frames_drawn() < inspect_after_frame) {
				return Variant();
			}
			EditorInspector *inspector = InspectorDock::get_inspector_singleton();
			Object *subject = _get_inspected_object();
			if (!inspector || !subject) {
				return Variant();
			}
			if (inspector->get_edited_object() != subject) {
				InspectorDock::get_singleton()->update(subject);
				inspector->edit(subject);
				inspect_after_frame = Engine::get_singleton()->get_frames_drawn() + 1;
				return Variant();
			}
			String error;
			if (!_resolve_property_chain(error)) {
				_fail(MCPToolError::NOT_FOUND, error);
				return Variant();
			}
			EditorProperty *target = Object::cast_to<EditorProperty>(ObjectDB::get_instance(target_property));
			if (!target) {
				_fail(MCPToolError::FAILED, "the target Inspector property disappeared while it was being expanded");
				return Variant();
			}
			// Expanding sections and nested Resources changes the Inspector's minimum
			// size asynchronously. Let its containers sort before asking the scroll bar
			// for a range; otherwise a newly revealed property can be clamped against
			// the old content height and remain below the dock.
			capture_after_frame = Engine::get_singleton()->get_frames_drawn() + 2;
			stage = WAITING_FOR_LAYOUT;
			return Variant();
		}

		if (stage == WAITING_FOR_LAYOUT && Engine::get_singleton()->get_frames_drawn() >= capture_after_frame) {
			EditorInspector *inspector = InspectorDock::get_inspector_singleton();
			EditorProperty *target = Object::cast_to<EditorProperty>(ObjectDB::get_instance(target_property));
			if (!inspector || !target) {
				_fail(MCPToolError::FAILED, "the target Inspector property disappeared before it could be centered");
				return Variant();
			}
			EditorInspector *root_inspector = inspector->get_root_inspector();
			root_inspector->ensure_control_visible(target);
			const Rect2 target_rect = target->get_global_rect();
			const Rect2 inspector_rect = root_inspector->get_global_rect();
			const int desired_scroll = root_inspector->get_scroll_offset() +
					(int)Math::round(target_rect.get_center().y - inspector_rect.get_center().y);
			root_inspector->set_scroll_offset(MAX(0, desired_scroll));
			capture_after_frame = Engine::get_singleton()->get_frames_drawn() + 2;
			stage = WAITING_FOR_RENDER;
			return Variant();
		}

		if (stage == WAITING_FOR_RENDER && Engine::get_singleton()->get_frames_drawn() >= capture_after_frame) {
			MCPToolError error;
			Dictionary result = _capture(error);
			if (error.has_error()) {
				_fail(error.kind, error.message);
				return Variant();
			}
			_restore_editor_state();
			_clear_operation();
			return result;
		}
		return Variant();
	}

public:
	virtual String get_tool_name() const override { return "Godot_CaptureInspectorProperty"; }
	virtual String get_description() const override {
		return "Open either a project Resource or a node from a scene in the Inspector, recursively expand an exact chain of "
			   "Resource-valued properties, center the final property, and save a tightly cropped "
			   "Inspector-only PNG for documentation. Property-chain entries use raw Godot property "
			   "names, not translated labels. The previous inspected object, folds, scroll and dock "
			   "tab are restored after capture.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["resource"] = MCPSchema::string_property(
				"Resource file to inspect, as a res:// path. Use either this or scene, not both.", "");
		properties["scene"] = MCPSchema::string_property(
				"Scene file to open, as a res:// path. Requires node_path and cannot be combined with resource.", "");
		properties["node_path"] = MCPSchema::string_property("NodePath relative to the scene root; '.' selects the root node.", "");
		Dictionary chain_item = MCPSchema::string_property("One exact raw Godot property name.");
		chain_item["minLength"] = 1;
		properties["property_chain"] = MCPSchema::array_property(
				"Exact property names from the root Resource to the final variable. Every entry except "
				"the last must contain a non-null Resource, which is expanded as a sub-inspector.",
				chain_item);
		properties["context_properties"] = MCPSchema::array_property(
				"Optional raw property names on the final inspected object. Their Inspector groups are "
				"expanded to provide documentation context while the property_chain target remains selected.",
				chain_item);
		properties["path"] = MCPSchema::string_property(
				"Where to save the PNG, as a res:// path. Defaults to a timestamped file under res://ai_screenshots/.", "");
		Dictionary above = MCPSchema::integer_property("Editor UI points to include above the target property.", 120);
		above["minimum"] = 0;
		above["maximum"] = 2000;
		properties["context_above"] = above;
		Dictionary below = MCPSchema::integer_property("Editor UI points to include below the target property.", 160);
		below["minimum"] = 0;
		below["maximum"] = 2000;
		properties["context_below"] = below;
		properties["inline_image"] = MCPSchema::bool_property("Also return the image inline when it is small enough.", true);
		Vector<String> required;
		required.push_back("property_chain");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Where the PNG was saved.");
		properties["width"] = MCPSchema::integer_property("Cropped image width in pixels.");
		properties["height"] = MCPSchema::integer_property("Cropped image height in pixels.");
		properties["inlined"] = MCPSchema::bool_property("True when the image is also in the response.");
		properties["resource"] = MCPSchema::string_property("Resource that was inspected.");
		properties["scene"] = MCPSchema::string_property("Scene that was inspected, or empty for a Resource capture.");
		properties["node_path"] = MCPSchema::string_property("Selected node relative to the scene root, or empty for a Resource capture.");
		properties["property_chain"] = MCPSchema::array_property("Resolved property chain.", MCPSchema::string_property("Raw property name."));
		properties["context_properties"] = MCPSchema::array_property(
				"Additional properties whose groups were expanded for context.",
				MCPSchema::string_property("Raw property name."));
		properties["target_property"] = MCPSchema::string_property("Raw name of the centered property.");
		properties["target_label"] = MCPSchema::string_property("Label rendered by the Inspector.");
		properties["target_y"] = MCPSchema::integer_property("Target property's top edge within the crop, in pixels.");
		properties["target_height"] = MCPSchema::integer_property("Target property's rendered height, in pixels.");
		properties["pixel_scale"] = MCPSchema::number_property("Rendered pixels per editor UI point.");
		MCPCaptureMetadata::declare(properties);
		return MCPSchema::object_schema(properties);
	}

	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		const String path = p_arguments.has("path") ? String(p_arguments["path"]).strip_edges() : String();
		if (!path.is_empty()) {
			paths.push_back(path);
		}
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const Array requested_chain = p_arguments["property_chain"];
		if (requested_chain.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "property_chain must contain at least the final property name");
			return Dictionary();
		}
		for (int i = 0; i < requested_chain.size(); i++) {
			if (String(requested_chain[i]).strip_edges().is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("property_chain[%d] must not be empty", i));
				return Dictionary();
			}
		}
		const Array requested_context = p_arguments.get("context_properties", Array());
		for (int i = 0; i < requested_context.size(); i++) {
			if (String(requested_context[i]).strip_edges().is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("context_properties[%d] must not be empty", i));
				return Dictionary();
			}
		}
		const String requested_resource = String(p_arguments.get("resource", String())).strip_edges();
		const String requested_scene = String(p_arguments.get("scene", String())).strip_edges();
		const String requested_node = String(p_arguments.get("node_path", String())).strip_edges();
		if (requested_resource.is_empty() == requested_scene.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "provide exactly one of resource or scene");
			return Dictionary();
		}
		if (!requested_scene.is_empty() && requested_node.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "node_path is required when scene is provided; use '.' for the root node");
			return Dictionary();
		}
		if (!requested_resource.is_empty() && !requested_node.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "node_path is only valid with scene");
			return Dictionary();
		}
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton() || !InspectorDock::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor Inspector");
			return Dictionary();
		}
		if (DisplayServer::get_singleton() && DisplayServer::get_singleton()->get_name() == "headless") {
			r_error.set(MCPToolError::UNSUPPORTED,
					"this editor is running headless, so its Inspector cannot be photographed; relaunch it with a display");
			return Dictionary();
		}
		if (stage != IDLE) {
			r_error.set(MCPToolError::INVALID_STATE, "an Inspector property capture is already in progress");
			return Dictionary();
		}

		MCPPaths::Resolved resolved_subject;
		String path_error;
		const String requested_subject = !requested_resource.is_empty() ? requested_resource : requested_scene;
		if (!MCPPaths::resolve_existing(requested_subject, resolved_subject, path_error)) {
			r_error.set(MCPToolError::NOT_FOUND, path_error);
			return Dictionary();
		}
		if (resolved_subject.is_directory) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is a directory, not an inspectable file", resolved_subject.res_path));
			return Dictionary();
		}

		String requested_output = String(p_arguments.get("path", String())).strip_edges();
		if (requested_output.is_empty()) {
			requested_output = "res://ai_screenshots/inspector-" +
					Time::get_singleton()->get_datetime_string_from_system(false, false).replace(":", "").replace("-", "").replace(" ", "-") +
					"-" + itos(Time::get_singleton()->get_ticks_msec()) + ".png";
		}
		if (requested_output.get_extension().to_lower() != "png") {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "the capture path must end in .png");
			return Dictionary();
		}
		MCPPaths::Resolved resolved_output;
		if (!MCPPaths::resolve(requested_output, resolved_output, path_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, path_error);
			return Dictionary();
		}

		EditorInspector *inspector = InspectorDock::get_inspector_singleton();
		original_scene_index = EditorNode::get_editor_data().get_edited_scene();
		Object *previous = inspector->get_edited_object();
		original_object = previous ? previous->get_instance_id() : ObjectID();
		original_resource = Ref<Resource>(Object::cast_to<Resource>(previous));
		original_selected_path = inspector->get_selected_path();
		original_scroll = inspector->get_scroll_offset();
		original_docks_visible = !EditorDockManager::get_singleton() || EditorDockManager::get_singleton()->are_docks_visible();
		if (TabContainer *tabs = find_parent_tab_container(InspectorDock::get_singleton())) {
			original_tab_container = tabs->get_instance_id();
			original_tab = tabs->get_current_tab();
			const int inspector_tab = tabs->get_tab_idx_from_control(InspectorDock::get_singleton());
			if (inspector_tab >= 0) {
				tabs->set_current_tab(inspector_tab);
			}
		}
		if (EditorDockManager::get_singleton()) {
			EditorDockManager::get_singleton()->set_docks_visible(true);
		}

		Object *subject = nullptr;
		if (!requested_resource.is_empty()) {
			resource = ResourceLoader::load(resolved_subject.res_path);
			if (resource.is_null()) {
				r_error.set(MCPToolError::NOT_FOUND, vformat("'%s' could not be loaded as a Godot Resource", resolved_subject.res_path));
				_restore_editor_state();
				_clear_operation();
				return Dictionary();
			}
			resource_path = resolved_subject.res_path;
			subject = resource.ptr();
		} else {
			const String extension = resolved_subject.res_path.get_extension().to_lower();
			if (extension != "tscn" && extension != "scn" && extension != "res") {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is not a scene file", resolved_subject.res_path));
				_restore_editor_state();
				_clear_operation();
				return Dictionary();
			}
			EditorInterface::get_singleton()->open_scene_from_path(resolved_subject.res_path);
			Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
			Node *node = root && (requested_node == "." || requested_node.is_empty()) ? root :
					(root ? root->get_node_or_null(NodePath(requested_node)) : nullptr);
			if (!node) {
				r_error.set(MCPToolError::NOT_FOUND,
						vformat("node '%s' was not found in '%s'", requested_node, resolved_subject.res_path));
				_restore_editor_state();
				_clear_operation();
				return Dictionary();
			}
			scene_path = resolved_subject.res_path;
			node_path = requested_node;
			subject = node;
			SceneTreeDock::get_singleton()->set_selected(node, false);
		}
		inspected_object = subject->get_instance_id();
		property_chain = requested_chain.duplicate();
		context_properties = requested_context.duplicate();
		output_path = resolved_output;
		context_above = (int)p_arguments.get("context_above", 120);
		context_below = (int)p_arguments.get("context_below", 160);
		inline_image = (bool)p_arguments.get("inline_image", true);
		InspectorDock::get_singleton()->update(subject);
		inspector->edit(subject);
		stage = WAITING_FOR_INSPECTOR;
		inspect_after_frame = Engine::get_singleton()->get_frames_drawn() + 2;
		started_msec = OS::get_singleton()->get_ticks_msec();
		frames_at_start = Engine::get_singleton()->get_frames_drawn();
		token = MCPDeferred::begin_polled(DEFERRED_BACKSTOP_SECONDS,
				callable_mp(this, &CaptureInspectorPropertyTool::_poll),
				callable_mp(this, &CaptureInspectorPropertyTool::_cancel));
		return MCPDeferred::make_deferred_result(token);
	}
};

class CaptureSceneTreeNodeTool : public MCPTool {
	struct FoldState {
		NodePath path;
		bool collapsed = false;
	};

	bool active = false;
	bool prepared = false;
	uint64_t started_msec = 0;
	uint64_t frames_at_start = 0;
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	MCPPaths::Resolved output_path;
	String scene_path;
	String node_path;
	int context_above = 120;
	int context_below = 160;
	bool inline_image = true;
	int original_scene_index = -1;
	ObjectID original_tab_container;
	int original_tab = -1;
	bool original_docks_visible = true;
	ObjectID target_scene_original_node;
	Point2 target_scene_original_scroll;
	ObjectID target_node;
	ObjectID target_item;
	Vector<FoldState> folds;
	int expanded_ancestors = 0;
	uint64_t prepare_after_frame = 0;
	uint64_t capture_after_frame = 0;

	Tree *_get_tree() const {
		SceneTreeDock *dock = SceneTreeDock::get_singleton();
		return dock && dock->get_tree_editor() ? dock->get_tree_editor()->get_scene_tree() : nullptr;
	}

	void _clear_operation() {
		active = false;
		prepared = false;
		started_msec = 0;
		frames_at_start = 0;
		token = MCPDeferred::INVALID_TOKEN;
		output_path = MCPPaths::Resolved();
		scene_path = String();
		node_path = String();
		original_scene_index = -1;
		original_tab_container = ObjectID();
		original_tab = -1;
		target_scene_original_node = ObjectID();
		target_node = ObjectID();
		target_item = ObjectID();
		folds.clear();
		expanded_ancestors = 0;
		prepare_after_frame = 0;
	}

	void _restore_editor_state() {
		Tree *tree = _get_tree();
		if (prepared && tree) {
			for (int i = folds.size() - 1; i >= 0; i--) {
				TreeItem *item = tree->get_item_with_metadata(folds[i].path, 0);
				if (item) {
					item->set_collapsed(folds[i].collapsed);
				}
			}
		}
		if (prepared) {
			Node *previous_node = Object::cast_to<Node>(ObjectDB::get_instance(target_scene_original_node));
			if (SceneTreeDock::get_singleton()) {
				SceneTreeDock::get_singleton()->set_selected(previous_node, false);
			}
			if ((tree = _get_tree())) {
				tree->get_vscroll_bar()->set_value(target_scene_original_scroll.y);
			}
		}

		if (original_scene_index >= 0 && EditorSceneTabs::get_singleton() &&
				EditorNode::get_editor_data().get_edited_scene() != original_scene_index) {
			EditorSceneTabs::get_singleton()->set_current_tab(original_scene_index);
		}
		if (TabContainer *tabs = Object::cast_to<TabContainer>(ObjectDB::get_instance(original_tab_container))) {
			if (original_tab >= 0 && original_tab < tabs->get_tab_count()) {
				tabs->set_current_tab(original_tab);
			}
		}
		if (EditorDockManager::get_singleton()) {
			EditorDockManager::get_singleton()->set_docks_visible(original_docks_visible);
		}
	}

	void _cancel() {
		if (!active) {
			return;
		}
		_restore_editor_state();
		_clear_operation();
	}

	void _fail(MCPToolError::Kind p_kind, const String &p_message) {
		const MCPDeferred::Token pending = token;
		_restore_editor_state();
		_clear_operation();
		MCPDeferred::fail(pending, p_kind, p_message);
	}

	bool _prepare_capture(String &r_error) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(target_node));
		Tree *tree = _get_tree();
		SceneTreeDock *dock = SceneTreeDock::get_singleton();
		if (!node || !tree || !dock || !dock->get_tree_editor()) {
			r_error = "the scene or Scene tree disappeared while preparing the capture";
			return false;
		}
		dock->get_tree_editor()->update_tree();
		TreeItem *item = tree->get_item_with_metadata(node->get_path(), 0);
		if (!item) {
			r_error = "the scene opened but its target node row was not built in the Scene dock";
			return false;
		}
		if (Node *selected_node = dock->get_tree_editor()->get_selected()) {
			target_scene_original_node = selected_node->get_instance_id();
		}
		target_scene_original_scroll = tree->get_scroll();
		for (TreeItem *ancestor = item->get_parent(); ancestor && ancestor != tree->get_root(); ancestor = ancestor->get_parent()) {
			FoldState state;
			state.path = ancestor->get_metadata(0);
			state.collapsed = ancestor->is_collapsed();
			folds.push_back(state);
			if (state.collapsed) {
				expanded_ancestors++;
			}
			ancestor->set_collapsed(false);
		}
		item->select(0);
		item->set_as_cursor(0);
		tree->scroll_to_item(item, true);
		dock->set_selected(node, false);
		target_item = item->get_instance_id();
		capture_after_frame = Engine::get_singleton()->get_frames_drawn() + 2;
		prepared = true;
		return true;
	}

	Dictionary _capture(MCPToolError &r_error) {
		SceneTreeDock *dock = SceneTreeDock::get_singleton();
		Tree *tree = _get_tree();
		TreeItem *item = Object::cast_to<TreeItem>(ObjectDB::get_instance(target_item));
		Viewport *viewport = dock ? dock->get_viewport() : nullptr;
		if (!dock || !tree || !item || !viewport || !dock->is_visible_in_tree() || !tree->is_visible_in_tree()) {
			r_error.set(MCPToolError::INVALID_STATE, "the Scene tree node stopped being visible before it could be captured");
			return Dictionary();
		}

		const Ref<Image> screen = viewport->get_texture().is_valid() ? viewport->get_texture()->get_image() : Ref<Image>();
		if (screen.is_null() || screen->is_empty()) {
			r_error.set(MCPToolError::FAILED, "the Scene dock viewport produced no rendered image");
			return Dictionary();
		}
		const Rect2 viewport_rect = viewport->get_visible_rect();
		const Rect2 dock_rect = dock->get_global_rect().intersection(viewport_rect);
		Rect2 item_rect = tree->get_item_rect(item);
		item_rect.position += tree->get_global_rect().position;
		if (dock_rect.size.x <= 1 || dock_rect.size.y <= 1 || !dock_rect.intersects(item_rect)) {
			r_error.set(MCPToolError::FAILED, "the requested node could not be scrolled into the visible Scene dock");
			return Dictionary();
		}

		Rect2 logical_crop(Point2(dock_rect.position.x, item_rect.position.y - context_above),
				Size2(dock_rect.size.x, item_rect.size.y + context_above + context_below));
		logical_crop = logical_crop.intersection(dock_rect);
		const Vector2 scale((double)screen->get_width() / viewport_rect.size.x,
				(double)screen->get_height() / viewport_rect.size.y);
		const Point2 relative_begin = logical_crop.position - viewport_rect.position;
		const Point2 relative_end = logical_crop.get_end() - viewport_rect.position;
		const int x0 = Math::floor(relative_begin.x * scale.x);
		const int y0 = Math::floor(relative_begin.y * scale.y);
		const int x1 = Math::ceil(relative_end.x * scale.x);
		const int y1 = Math::ceil(relative_end.y * scale.y);
		Rect2i pixels(x0, y0, x1 - x0, y1 - y0);
		pixels = pixels.intersection(Rect2i(0, 0, screen->get_width(), screen->get_height()));
		if (pixels.size.x <= 1 || pixels.size.y <= 1) {
			r_error.set(MCPToolError::FAILED, "the Scene tree crop was empty after clipping it to the rendered window");
			return Dictionary();
		}

		const Ref<Image> image = screen->get_region(pixels);
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (dir.is_valid()) {
			dir->make_dir_recursive(output_path.absolute.get_base_dir());
		}
		if (image.is_null() || image->save_png(output_path.absolute) != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not write '%s'", output_path.res_path));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = output_path.res_path;
		result["width"] = image->get_width();
		result["height"] = image->get_height();
		result["inlined"] = false;
		result["scene"] = scene_path;
		result["node_path"] = node_path;
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(target_node));
		result["node_name"] = node ? String(node->get_name()) : String();
		result["target_y"] = (int)Math::round((item_rect.position.y - logical_crop.position.y) * scale.y);
		result["target_height"] = MAX(1, (int)Math::round(item_rect.size.y * scale.y));
		result["ancestor_depth"] = folds.size();
		result["expanded_ancestors"] = expanded_ancestors;
		result["pixel_scale"] = scale.y;
		MCPCaptureMetadata::stamp(result, MCPCaptureMetadata::SOURCE_EDITOR_VIEWPORT,
				vformat("Scene tree node %s in %s", node_path, scene_path));
		MCPCaptureMetadata::add_note(result,
				"This is a tightly cropped editor Scene dock image centered on the selected node; "
				"it is not the running scene or the whole editor window.");
		if (inline_image) {
			add_inline_image(image, result);
		}
		return result;
	}

	Variant _poll() {
		if (!active) {
			return Variant();
		}
		// Same budget and the same reasoning as the Inspector capture: this advances on
		// drawn frames, so on a slow software renderer a short deadline fails a test that
		// was only being patient.
		if (started_msec != 0) {
			const uint64_t elapsed = OS::get_singleton()->get_ticks_msec() - started_msec;
			if (elapsed >= CAPTURE_BUDGET_MSEC) {
				const uint64_t drawn = Engine::get_singleton()->get_frames_drawn() - frames_at_start;
				_fail(MCPToolError::FAILED,
						vformat("gave up after %d ms while waiting for the editor to draw the Scene "
								"dock; it drew %d frame(s) in that time.",
								(int)elapsed, (int)drawn));
				return Variant();
			}
		}
		if (!prepared) {
			if (Engine::get_singleton()->get_frames_drawn() < prepare_after_frame) {
				return Variant();
			}
			String error;
			if (!_prepare_capture(error)) {
				_fail(MCPToolError::FAILED, error);
			}
			return Variant();
		}
		if (Engine::get_singleton()->get_frames_drawn() < capture_after_frame) {
			return Variant();
		}
		MCPToolError error;
		Dictionary result = _capture(error);
		if (error.has_error()) {
			_fail(error.kind, error.message);
			return Variant();
		}
		_restore_editor_state();
		_clear_operation();
		return result;
	}

public:
	virtual String get_tool_name() const override { return "Godot_CaptureSceneTreeNode"; }
	virtual String get_description() const override {
		return "Open a scene, select and highlight an exact NodePath in the Scene tree, expand "
			   "all ancestors needed to reveal it, center it, and save a Scene-dock-only PNG with "
			   "the requested context above and below. The previous scene tab, selection, folds, "
			   "scroll and dock tab are restored after capture.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["scene"] = MCPSchema::string_property("Scene file to open, as a res:// path.");
		properties["node_path"] = MCPSchema::string_property("Exact NodePath relative to the scene root; '.' selects the root.");
		properties["path"] = MCPSchema::string_property(
				"Where to save the PNG, as a res:// path. Defaults to a timestamped file under res://ai_screenshots/.", "");
		Dictionary above = MCPSchema::integer_property("Editor UI points to include above the highlighted node.", 120);
		above["minimum"] = 0;
		above["maximum"] = 2000;
		properties["context_above"] = above;
		Dictionary below = MCPSchema::integer_property("Editor UI points to include below the highlighted node.", 160);
		below["minimum"] = 0;
		below["maximum"] = 2000;
		properties["context_below"] = below;
		properties["inline_image"] = MCPSchema::bool_property("Also return the image inline when it is small enough.", true);
		Vector<String> required;
		required.push_back("scene");
		required.push_back("node_path");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Where the PNG was saved.");
		properties["width"] = MCPSchema::integer_property("Cropped image width in pixels.");
		properties["height"] = MCPSchema::integer_property("Cropped image height in pixels.");
		properties["inlined"] = MCPSchema::bool_property("True when the image is also in the response.");
		properties["scene"] = MCPSchema::string_property("Scene that was opened.");
		properties["node_path"] = MCPSchema::string_property("Selected node relative to the scene root.");
		properties["node_name"] = MCPSchema::string_property("Selected node's displayed name.");
		properties["target_y"] = MCPSchema::integer_property("Highlighted row's top edge within the crop, in pixels.");
		properties["target_height"] = MCPSchema::integer_property("Highlighted row's rendered height, in pixels.");
		properties["ancestor_depth"] = MCPSchema::integer_property("Ancestor rows traversed to reveal the target.");
		properties["expanded_ancestors"] = MCPSchema::integer_property("Ancestor rows temporarily expanded to reveal the target.");
		properties["pixel_scale"] = MCPSchema::number_property("Rendered pixels per editor UI point.");
		MCPCaptureMetadata::declare(properties);
		return MCPSchema::object_schema(properties);
	}

	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		const String path = p_arguments.has("path") ? String(p_arguments["path"]).strip_edges() : String();
		if (!path.is_empty()) {
			paths.push_back(path);
		}
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton() || !SceneTreeDock::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor Scene dock");
			return Dictionary();
		}
		if (DisplayServer::get_singleton() && DisplayServer::get_singleton()->get_name() == "headless") {
			r_error.set(MCPToolError::UNSUPPORTED,
					"this editor is running headless, so its Scene tree cannot be photographed; relaunch it with a display");
			return Dictionary();
		}
		if (active) {
			r_error.set(MCPToolError::INVALID_STATE, "a Scene tree node capture is already in progress");
			return Dictionary();
		}

		MCPPaths::Resolved resolved_scene;
		String path_error;
		if (!MCPPaths::resolve_existing(p_arguments["scene"], resolved_scene, path_error)) {
			r_error.set(MCPToolError::NOT_FOUND, path_error);
			return Dictionary();
		}
		const String extension = resolved_scene.res_path.get_extension().to_lower();
		if (resolved_scene.is_directory || (extension != "tscn" && extension != "scn" && extension != "res")) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is not a scene file", resolved_scene.res_path));
			return Dictionary();
		}
		const String requested_node = String(p_arguments["node_path"]).strip_edges();
		if (requested_node.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "node_path must not be empty; use '.' for the root node");
			return Dictionary();
		}

		String requested_output = String(p_arguments.get("path", String())).strip_edges();
		if (requested_output.is_empty()) {
			requested_output = "res://ai_screenshots/scene-tree-" +
					Time::get_singleton()->get_datetime_string_from_system(false, false).replace(":", "").replace("-", "").replace(" ", "-") +
					"-" + itos(Time::get_singleton()->get_ticks_msec()) + ".png";
		}
		if (requested_output.get_extension().to_lower() != "png") {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "the capture path must end in .png");
			return Dictionary();
		}
		MCPPaths::Resolved resolved_output;
		if (!MCPPaths::resolve(requested_output, resolved_output, path_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, path_error);
			return Dictionary();
		}

		original_scene_index = EditorNode::get_editor_data().get_edited_scene();
		original_docks_visible = !EditorDockManager::get_singleton() || EditorDockManager::get_singleton()->are_docks_visible();
		if (TabContainer *tabs = find_parent_tab_container(SceneTreeDock::get_singleton())) {
			original_tab_container = tabs->get_instance_id();
			original_tab = tabs->get_current_tab();
			const int scene_tab = tabs->get_tab_idx_from_control(SceneTreeDock::get_singleton());
			if (scene_tab >= 0) {
				tabs->set_current_tab(scene_tab);
			}
		}
		if (EditorDockManager::get_singleton()) {
			EditorDockManager::get_singleton()->set_docks_visible(true);
		}

		EditorInterface::get_singleton()->open_scene_from_path(resolved_scene.res_path);
		Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
		Node *node = root && requested_node == "." ? root : (root ? root->get_node_or_null(NodePath(requested_node)) : nullptr);
		if (!node) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("node '%s' was not found in '%s'", requested_node, resolved_scene.res_path));
			_restore_editor_state();
			_clear_operation();
			return Dictionary();
		}

		output_path = resolved_output;
		scene_path = resolved_scene.res_path;
		node_path = requested_node;
		context_above = (int)p_arguments.get("context_above", 120);
		context_below = (int)p_arguments.get("context_below", 160);
		inline_image = (bool)p_arguments.get("inline_image", true);
		target_node = node->get_instance_id();
		prepare_after_frame = Engine::get_singleton()->get_frames_drawn() + 2;
		active = true;
		started_msec = OS::get_singleton()->get_ticks_msec();
		frames_at_start = Engine::get_singleton()->get_frames_drawn();
		token = MCPDeferred::begin_polled(DEFERRED_BACKSTOP_SECONDS,
				callable_mp(this, &CaptureSceneTreeNodeTool::_poll),
				callable_mp(this, &CaptureSceneTreeNodeTool::_cancel));
		return MCPDeferred::make_deferred_result(token);
	}
};

} // namespace

void mcp_register_capture_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(CaptureViewportTool)));
	registry->register_tool(Ref<MCPTool>(memnew(CaptureInspectorPropertyTool)));
	registry->register_tool(Ref<MCPTool>(memnew(CaptureSceneTreeNodeTool)));
}
