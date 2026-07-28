/**************************************************************************/
/*  mcp_project_config_tools.cpp                                          */
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

// Project settings, on-demand checkpoints, and a screenshot of the whole editor.
//
// Three small things the production template needs and could not reach: the settings
// that decide a game's resolution and main scene, a way to mark a known-good point
// before a risky change rather than only before a tool's own write, and a capture that
// includes dialogs - which are separate OS windows and so invisible to a viewport
// capture.

#include "mcp_builtin_tools.h"

#include "../mcp_checkpoints.h"
#include "../mcp_paths.h"
#include "../mcp_tool_registry.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/os/os.h"
#include "core/variant/array.h"
#include "core/variant/variant_parser.h"
#include "servers/display_server.h"

#include "editor/editor_file_system.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"

namespace {

const int MAX_INLINE_SHOT_BYTES = 3 * 1024 * 1024;

class GetProjectSettingTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetProjectSetting"; }
	virtual String get_description() const override {
		return "Read a project setting, such as display/window/size/viewport_width or "
			   "application/run/main_scene. Pass no name to list the settings this project has "
			   "set explicitly, which is a short list - most of what a project runs on is the "
			   "engine's defaults.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property(
				"Setting to read. Empty lists the project's own settings.", "");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property("Setting that was read.");
		properties["value"] = MCPSchema::any_property("Its value, for types JSON can carry.");
		properties["text"] = MCPSchema::string_property("Its value in Godot's text form.");
		properties["settings"] = MCPSchema::array_property("Names, when listing.",
				MCPSchema::string_property("Setting name."));
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		ProjectSettings *settings = ProjectSettings::get_singleton();
		if (!settings) {
			r_error.set(MCPToolError::UNSUPPORTED, "no project settings are loaded");
			return Dictionary();
		}
		const String name = String(p_arguments["name"]).strip_edges();

		Dictionary result;
		if (name.is_empty()) {
			Array names;
			List<PropertyInfo> info;
			settings->get_property_list(&info);
			for (const PropertyInfo &property : info) {
				// Only what the project itself set: the full list is thousands of engine
				// defaults, which buries the handful that describe this game.
				if (settings->get_order(property.name) < ProjectSettings::NO_BUILTIN_ORDER_BASE) {
					names.push_back(property.name);
				}
			}
			result["settings"] = names;
			result["name"] = String();
			return result;
		}

		if (!settings->has_setting(name)) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("this project has no setting '%s'", name));
			return Dictionary();
		}
		const Variant value = settings->get_setting(name);
		result["name"] = name;
		switch (value.get_type()) {
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::STRING:
				result["value"] = value;
				break;
			default:
				result["value"] = String(value);
				break;
		}
		String text;
		VariantWriter::write_to_string(value, text);
		result["text"] = text;
		return result;
	}
};

class SetProjectSettingTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SetProjectSetting"; }
	virtual String get_description() const override {
		return "Change a project setting and save project.godot. This is a persistent change to "
			   "the project, not a runtime one - it is checkpointed, and it survives.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property("Setting to change.");
		properties["value"] = MCPSchema::any_property("New value.");
		Vector<String> required;
		required.push_back("name");
		required.push_back("value");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property("Setting that was changed.");
		properties["text"] = MCPSchema::string_property("The value it now holds, read back.");
		properties["created"] = MCPSchema::bool_property("True when the setting did not exist before.");
		return MCPSchema::object_schema(properties);
	}

	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		paths.push_back("res://project.godot");
		return paths;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		ProjectSettings *settings = ProjectSettings::get_singleton();
		if (!settings) {
			r_error.set(MCPToolError::UNSUPPORTED, "no project settings are loaded");
			return Dictionary();
		}
		const String name = String(p_arguments["name"]).strip_edges();
		if (name.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "a setting name is required");
			return Dictionary();
		}
		const bool created = !settings->has_setting(name);

		// Match the existing type where there is one. project.godot is typed, and a
		// viewport width written as a string is a setting that silently does nothing.
		Variant value = p_arguments["value"];
		if (!created) {
			const Variant current = settings->get_setting(name);
			if (current.get_type() != value.get_type() &&
					Variant::can_convert(value.get_type(), current.get_type())) {
				Callable::CallError call_error;
				const Variant *argument = &value;
				Variant converted;
				Variant::construct(current.get_type(), converted, &argument, 1, call_error);
				if (call_error.error == Callable::CallError::CALL_OK) {
					value = converted;
				}
			}
		}

		settings->set_setting(name, value);
		const Error error = settings->save();
		if (error != OK) {
			r_error.set(MCPToolError::FAILED, "could not save project.godot");
			return Dictionary();
		}

		Dictionary result;
		result["name"] = name;
		result["created"] = created;
		String text;
		VariantWriter::write_to_string(settings->get_setting(name), text);
		result["text"] = text;
		return result;
	}
};

class CreateCheckpointTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CreateCheckpoint"; }
	virtual String get_description() const override {
		return "Snapshot project files now, under a name you choose, so a known-good point exists "
			   "before a risky change. Checkpoints are otherwise only taken automatically before "
			   "a tool writes - this is for the moment before a sequence of them.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["label"] = MCPSchema::string_property("What this point is, for finding it later.");
		properties["paths"] = MCPSchema::array_property(
				"Project files to snapshot, as res:// paths.",
				MCPSchema::string_property("A res:// path."));
		Vector<String> required;
		required.push_back("paths");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["checkpoint"] = MCPSchema::string_property("Checkpoint id, for Godot_RestoreCheckpoint.");
		properties["paths"] = MCPSchema::array_property("Files snapshotted.",
				MCPSchema::string_property("A res:// path."));
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const Array requested = p_arguments["paths"];
		if (requested.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "name at least one file to snapshot");
			return Dictionary();
		}

		Vector<String> paths;
		Array confirmed;
		for (int i = 0; i < requested.size(); i++) {
			MCPPaths::Resolved resolved;
			String error;
			// Resolved here as well as in the checkpoint layer, so a path outside the
			// project is refused with a message about the argument rather than
			// disappearing into a snapshot that quietly holds nothing.
			if (!MCPPaths::resolve_existing(requested[i], resolved, error)) {
				r_error.set(MCPToolError::NOT_FOUND, error);
				return Dictionary();
			}
			if (resolved.is_directory) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS,
						vformat("'%s' is a directory; name the files to snapshot", resolved.res_path));
				return Dictionary();
			}
			paths.push_back(resolved.res_path);
			confirmed.push_back(resolved.res_path);
		}

		const String label = p_arguments.has("label") ? String(p_arguments["label"]) : String("manual");
		String error;
		const String id = MCPCheckpoints::create(get_tool_name(), label, paths, error);
		if (id.is_empty()) {
			r_error.set(MCPToolError::FAILED,
					error.is_empty() ? String("the checkpoint could not be created") : error);
			return Dictionary();
		}

		Dictionary result;
		result["checkpoint"] = id;
		result["paths"] = confirmed;
		return result;
	}
};

class CaptureEditorWindowTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CaptureEditorWindow"; }
	virtual String get_description() const override {
		return "Capture the whole screen the editor is on, including dialogs, popups and the "
			   "running game's window. Godot_CaptureViewport photographs only the editor's own "
			   "viewport, so anything in a separate OS window - which every dialog is - does not "
			   "appear in it, and a review of that image is a review of the wrong thing.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property(
				"Where to save the PNG, as a res:// path. Defaults to a timestamped file under "
				"res://ai_screenshots/.",
				"");
		properties["inline_image"] = MCPSchema::bool_property(
				"Also return the image in the response, when small enough.", true);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Where the PNG was saved.");
		properties["width"] = MCPSchema::integer_property("Image width.");
		properties["height"] = MCPSchema::integer_property("Image height.");
		properties["inlined"] = MCPSchema::bool_property("True when the image is in the response.");
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
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		DisplayServer *display = DisplayServer::get_singleton();
		if (!display || display->get_name() == "headless") {
			r_error.set(MCPToolError::UNSUPPORTED,
					"this editor is running headless, so there is no screen to capture; "
					"`python3 tools/virtual_display.py -- <godot binary> --path <project> --editor` "
					"starts one");
			return Dictionary();
		}
		if (!display->has_feature(DisplayServer::FEATURE_SCREEN_CAPTURE)) {
			r_error.set(MCPToolError::UNSUPPORTED,
					"this platform's display server cannot capture the screen; "
					"Godot_CaptureViewport still photographs the editor's own viewport");
			return Dictionary();
		}

		const Ref<Image> image = display->screen_get_image(display->window_get_current_screen());
		if (image.is_null() || image->is_empty()) {
			r_error.set(MCPToolError::FAILED, "the screen capture came back empty");
			return Dictionary();
		}

		String path = String(p_arguments["path"]).strip_edges();
		if (path.is_empty()) {
			path = vformat("res://ai_screenshots/editor_%d.png", OS::get_singleton()->get_ticks_msec());
		}
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve(path, resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}
		Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		const String directory = resolved.absolute.get_base_dir();
		if (access.is_valid() && !access->dir_exists(directory)) {
			access->make_dir_recursive(directory);
		}
		if (image->save_png(resolved.absolute) != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not save '%s'", resolved.res_path));
			return Dictionary();
		}
		EditorInterface::get_singleton()->get_resource_file_system()->scan();

		Dictionary result;
		result["path"] = resolved.res_path;
		result["width"] = image->get_width();
		result["height"] = image->get_height();
		result["inlined"] = false;

		if ((bool)p_arguments["inline_image"]) {
			const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(resolved.absolute);
			if (!bytes.is_empty() && bytes.size() <= MAX_INLINE_SHOT_BYTES) {
				Array content;
				Dictionary block;
				block["type"] = "image";
				block["data"] = CryptoCore::b64_encode_str(bytes.ptr(), bytes.size());
				block["mimeType"] = "image/png";
				content.push_back(block);
				result["_content"] = content;
				result["inlined"] = true;
			}
		}
		return result;
	}
};

} // namespace

void mcp_register_project_config_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(GetProjectSettingTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SetProjectSettingTool)));
	registry->register_tool(Ref<MCPTool>(memnew(CreateCheckpointTool)));
	registry->register_tool(Ref<MCPTool>(memnew(CaptureEditorWindowTool)));
}
