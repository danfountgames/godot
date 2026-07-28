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

#include "../mcp_paths.h"
#include "../mcp_tool_registry.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/os/time.h"
#include "core/variant/array.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "scene/main/viewport.h"
#include "servers/display_server.h"

namespace {

// Returning the image inline is what makes this useful to a model, but a screenshot
// is large; anything bigger than this is saved to disk and referenced by path only.
static const int MAX_INLINE_IMAGE_BYTES = 3 * 1024 * 1024;

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

		String requested = String(p_arguments["path"]).strip_edges();
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

		if ((bool)p_arguments["inline_image"]) {
			const Vector<uint8_t> png = image->save_png_to_buffer();
			if (!png.is_empty() && png.size() <= MAX_INLINE_IMAGE_BYTES) {
				Dictionary image_content;
				image_content["type"] = "image";
				image_content["data"] = CryptoCore::b64_encode_str(png.ptr(), png.size());
				image_content["mimeType"] = "image/png";

				Array content;
				content.push_back(image_content);
				result["_content"] = content;
				result["inlined"] = true;
			}
		}
		return result;
	}
};

} // namespace

void mcp_register_capture_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(CaptureViewportTool)));
}
