/**************************************************************************/
/*  mcp_compare_tools.cpp                                                 */
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

#include "../mcp_image_diff.h"
#include "../mcp_paths.h"
#include "../mcp_schema.h"
#include "../mcp_tool_registry.h"

#include "core/io/image.h"

namespace {

// Comparing two captures.
//
// Screenshots existed and were treated as attachments: taken, saved, described in
// prose. The question a change actually raises - "did this alter what the player sees,
// and where?" - could only be answered by a person opening two files. This answers it,
// and can write the picture that shows it, because evidence about a visual medium
// should be something you look at.
class CompareCapturesTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CompareCaptures"; }
	virtual String get_description() const override {
		return "Compare two images and report what changed between them: how many pixels, what "
			   "fraction of the frame, and the box containing all of it. Optionally writes a "
			   "difference image with the changed area marked. Use it around a change - capture, "
			   "edit, capture again - to show that a fix worked or that an edit altered nothing "
			   "it should not have. Both captures must be the same size; a resized window is a "
			   "different question and is refused rather than answered wrongly.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["before"] = MCPSchema::string_property("Project path of the earlier image.");
		properties["after"] = MCPSchema::string_property("Project path of the later image.");
		properties["output"] = MCPSchema::string_property(
				"Where to write the difference image. Omit to compare without writing one.", "");
		properties["tolerance"] = MCPSchema::integer_property(
				"Per-channel difference, 0-255, treated as equal. Defaults to 8: two captures of "
				"an unchanged scene are not bit-identical, and 0 would report a change every time.",
				MCPImageDiff::DEFAULT_TOLERANCE);
		Vector<String> required;
		required.push_back("before");
		required.push_back("after");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["verdict"] = MCPSchema::string_property(
				"identical, minor, substantial, or incomparable.");
		properties["summary"] = MCPSchema::string_property("One line a person can read.");
		properties["changed_pixels"] = MCPSchema::integer_property("How many pixels differ.");
		properties["changed_fraction"] = MCPSchema::number_property("Those pixels as a fraction of the frame.");
		properties["max_channel_delta"] = MCPSchema::integer_property(
				"Largest single-channel difference found, 0-255, counted even below the tolerance.");
		properties["changed_bounds"] = MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
		properties["width"] = MCPSchema::integer_property("Width of both images.");
		properties["height"] = MCPSchema::integer_property("Height of both images.");
		properties["output"] = MCPSchema::string_property("Where the difference image was written.");
		return MCPSchema::object_schema(properties);
	}
	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		Vector<String> paths;
		const String output = String(p_arguments.get("output", String())).strip_edges();
		if (!output.is_empty()) {
			paths.push_back(output);
		}
		return paths;
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPPaths::Resolved before_path;
		MCPPaths::Resolved after_path;
		String error;
		if (!MCPPaths::resolve_existing(p_arguments["before"], before_path, error) ||
				!MCPPaths::resolve_existing(p_arguments["after"], after_path, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}

		const Ref<Image> before = Image::load_from_file(before_path.absolute);
		const Ref<Image> after = Image::load_from_file(after_path.absolute);
		if (before.is_null() || after.is_null()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("could not read %s as an image",
							before.is_null() ? before_path.res_path : after_path.res_path));
			return Dictionary();
		}

		const int tolerance = (int)p_arguments.get("tolerance", MCPImageDiff::DEFAULT_TOLERANCE);
		const MCPImageDiff::Result result = MCPImageDiff::compare(before, after, tolerance);
		if (!result.comparable) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("%s is %dx%d and %s is %dx%d. %s",
							before_path.res_path, before->get_width(), before->get_height(),
							after_path.res_path, after->get_width(), after->get_height(),
							MCPImageDiff::describe(result)));
			return Dictionary();
		}

		Dictionary out = result.to_dictionary();
		out["summary"] = MCPImageDiff::describe(result);

		const String output = String(p_arguments.get("output", String())).strip_edges();
		if (!output.is_empty()) {
			MCPPaths::Resolved output_path;
			if (!MCPPaths::resolve(output, output_path, error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
			}
			const Ref<Image> rendered = MCPImageDiff::render(before, after, tolerance);
			if (rendered.is_null() || rendered->save_png(output_path.absolute) != OK) {
				r_error.set(MCPToolError::FAILED,
						vformat("the difference image could not be written to %s", output_path.res_path));
				return Dictionary();
			}
			out["output"] = output_path.res_path;
		}
		return out;
	}
};

} // namespace

void mcp_register_compare_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(CompareCapturesTool)));
}
