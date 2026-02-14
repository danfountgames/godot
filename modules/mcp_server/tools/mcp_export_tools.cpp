/**************************************************************************/
/*  mcp_export_tools.cpp                                                  */
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

#include "mcp_export_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/version.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_platform.h"
#include "editor/export/editor_export_preset.h"
#include "editor/file_system/editor_paths.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPExportTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- editor/export/list_presets ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"editor/export/list_presets",
				"List Export Presets",
				"List all configured export presets and their target platforms. Each preset "
				"specifies a platform, export path, and configuration. Presets must be configured "
				"in Project > Export before exporting. Use editor/export/run to execute an export. "
				"Use editor/export/check_templates to verify export templates are installed.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPExportTools::handle_list_presets));
	}

	// ---- editor/export/run ----
	{
		Dictionary props;
		props["preset"] = make_prop("string",
				"Name of the export preset to use (from editor/export/list_presets)");
		Dictionary debug_prop = make_prop("boolean",
				"If true, export a debug build. Default is false (release).");
		debug_prop["default"] = false;
		props["debug"] = debug_prop;
		props["output_path"] = make_prop("string",
				"Override the export output path. If omitted, uses the path "
				"configured in the preset.");

		Array required;
		required.push_back("preset");
		p_registry->register_tool(
				"editor/export/run",
				"Run Export",
				"Export the project using a configured preset. Specify preset by name "
				"(use editor/export/list_presets to discover them). Exports can be debug or "
				"release mode. Export templates must be installed first (Editor > Manage Export "
				"Templates). For Web exports, the output is an HTML5 build. For iOS, ensure "
				"signing credentials are configured. This operation may take a while for "
				"large projects.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPExportTools::handle_run_export));
	}

	// ---- editor/export/check_templates ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"editor/export/check_templates",
				"Check Export Templates",
				"Check which export templates are installed. Export templates are required for "
				"building the project for each platform. If a template is missing, use "
				"Editor > Manage Export Templates to download and install it. Returns "
				"availability per platform and the expected template version.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPExportTools::handle_check_templates));
	}
}

// ============================================================================
// Tool 1: editor/export/list_presets
// ============================================================================

Dictionary MCPExportTools::handle_list_presets(const Dictionary &p_args) {
	EditorExport *exporter = EditorExport::get_singleton();
	ERR_FAIL_NULL_V(exporter, make_tool_error("Export system not available. This tool requires the Godot editor."));

	Array presets;
	for (int i = 0; i < exporter->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> preset = exporter->get_export_preset(i);
		if (preset.is_null()) {
			continue;
		}

		Dictionary p;
		p["index"] = i;
		p["name"] = preset->get_name();

		Ref<EditorExportPlatform> platform = preset->get_platform();
		p["platform"] = platform.is_valid() ? platform->get_name() : "<unknown>";
		p["runnable"] = preset->is_runnable();
		p["export_path"] = preset->get_export_path();
		p["custom_features"] = preset->get_custom_features();
		p["dedicated_server"] = preset->is_dedicated_server();
		presets.push_back(p);
	}

	String text;
	if (presets.is_empty()) {
		text = "No export presets configured.\n\n"
			   "Create presets in Project > Export... before using this tool.\n"
			   "Common presets: Web, Linux/X11, Windows Desktop, macOS, iOS, Android.";
	} else {
		text = vformat("Export presets (%d):\n", presets.size());
		for (int i = 0; i < presets.size(); i++) {
			Dictionary p = presets[i];
			text += vformat("  [%d] %s (%s)%s\n",
					(int)p["index"],
					(String)p["name"],
					(String)p["platform"],
					(bool)p["runnable"] ? " [runnable]" : "");
			String export_path = p["export_path"];
			if (!export_path.is_empty()) {
				text += vformat("      Path: %s\n", export_path);
			}
		}
	}

	Dictionary structured;
	structured["presets"] = presets;
	structured["count"] = presets.size();

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 2: editor/export/run
// ============================================================================

Dictionary MCPExportTools::handle_run_export(const Dictionary &p_args) {
	EditorExport *exporter = EditorExport::get_singleton();
	ERR_FAIL_NULL_V(exporter, make_tool_error("Export system not available. This tool requires the Godot editor."));

	// Get the preset name.
	if (!p_args.has("preset")) {
		return make_tool_error("Missing required parameter: preset");
	}

	String preset_name = p_args["preset"];
	if (preset_name.is_empty()) {
		return make_tool_error("Parameter 'preset' must not be empty.");
	}

	// Resolve preset by name (case-sensitive match).
	Ref<EditorExportPreset> preset;
	for (int i = 0; i < exporter->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> candidate = exporter->get_export_preset(i);
		if (candidate.is_valid() && candidate->get_name() == preset_name) {
			preset = candidate;
			break;
		}
	}

	if (preset.is_null()) {
		// Provide helpful suggestion with available preset names.
		String available;
		for (int i = 0; i < exporter->get_export_preset_count(); i++) {
			Ref<EditorExportPreset> p = exporter->get_export_preset(i);
			if (p.is_valid()) {
				if (!available.is_empty()) {
					available += ", ";
				}
				available += "\"" + p->get_name() + "\"";
			}
		}
		String msg = vformat("Export preset not found: \"%s\"\n\n", preset_name);
		if (available.is_empty()) {
			msg += "No export presets are configured. Create presets in Project > Export...";
		} else {
			msg += "Available presets: " + available + "\n"
				   "Use editor/export/list_presets to see full details.";
		}
		return make_tool_error(msg);
	}

	// Get the platform.
	Ref<EditorExportPlatform> platform = preset->get_platform();
	if (platform.is_null()) {
		return make_tool_error(vformat(
				"Export platform not available for preset '%s'. "
				"The platform module may not be loaded.",
				preset_name));
	}

	// Resolve debug flag.
	bool debug = (bool)p_args.get("debug", false);

	// Resolve export path.
	String export_path = (String)p_args.get("output_path", "");
	if (export_path.is_empty()) {
		export_path = preset->get_export_path();
	}
	if (export_path.is_empty()) {
		return make_tool_error(vformat(
				"No export path configured for preset '%s'.\n"
				"Set an output_path parameter or configure the path in Project > Export.",
				preset_name));
	}

	// Check if the platform can export (validates templates + project config).
	{
		String can_export_error;
		bool missing_templates = false;
		bool can = platform->can_export(preset, can_export_error, missing_templates, debug);
		if (!can) {
			String msg = vformat("Cannot export preset '%s':\n\n", preset_name);
			if (missing_templates) {
				msg += "Export templates are missing. Install them via "
					   "Editor > Manage Export Templates.\n\n";
			}
			if (!can_export_error.is_empty()) {
				msg += can_export_error;
			}
			return make_tool_error(msg);
		}
	}

	// Ensure output directory exists.
	String dir = export_path.get_base_dir();
	if (!dir.is_empty()) {
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid() && !da->dir_exists(dir)) {
			Error mkdir_err = da->make_dir_recursive(dir);
			if (mkdir_err != OK) {
				return make_tool_error(vformat(
						"Failed to create output directory: %s\n"
						"Error code: %d",
						dir, (int)mkdir_err));
			}
		}
	}

	// Execute export.
	// NOTE: export_project() uses EditorProgress internally which touches
	// the UI. It is safest to call from the main thread. Since MCP tool
	// handlers may run on the poll thread (which is the main thread in the
	// current single-threaded model), this works directly. If MCP moves to
	// threaded dispatch in a future phase, this will need to be wrapped
	// with callable_mp / call_deferred.
	Error err = platform->export_project(preset, debug, export_path, 0);

	if (err != OK) {
		// Collect export messages for a more detailed error report.
		String messages;
		for (int i = 0; i < platform->get_message_count(); i++) {
			EditorExportPlatform::ExportMessage msg = platform->get_message(i);
			if (msg.msg_type >= EditorExportPlatform::EXPORT_MESSAGE_WARNING) {
				String level = (msg.msg_type == EditorExportPlatform::EXPORT_MESSAGE_ERROR)
						? "ERROR"
						: "WARNING";
				messages += vformat("  [%s] %s: %s\n", level, msg.category, msg.text);
			}
		}

		String error_text = vformat(
				"Export failed for preset '%s' (error code %d).\n\n",
				preset_name, (int)err);

		if (!messages.is_empty()) {
			error_text += "Export messages:\n" + messages + "\n";
		}

		error_text += "Common causes:\n"
					  "- Export templates not installed (Editor > Manage Export Templates)\n"
					  "- Invalid export path or insufficient permissions\n"
					  "- Missing signing credentials (iOS/Android)\n"
					  "- Missing platform SDK";

		return make_tool_error(error_text);
	}

	// Build success response.
	String text = vformat(
			"Export completed successfully.\n"
			"Preset: %s\n"
			"Platform: %s\n"
			"Mode: %s\n"
			"Output: %s",
			preset->get_name(),
			platform->get_name(),
			debug ? "debug" : "release",
			export_path);

	// Include any warnings from the export process.
	String warnings;
	for (int i = 0; i < platform->get_message_count(); i++) {
		EditorExportPlatform::ExportMessage msg = platform->get_message(i);
		if (msg.msg_type == EditorExportPlatform::EXPORT_MESSAGE_WARNING) {
			warnings += vformat("  [WARNING] %s: %s\n", msg.category, msg.text);
		}
	}
	if (!warnings.is_empty()) {
		text += "\n\nWarnings:\n" + warnings;
	}

	Dictionary structured;
	structured["preset"] = preset->get_name();
	structured["platform"] = platform->get_name();
	structured["debug"] = debug;
	structured["output_path"] = export_path;
	structured["success"] = true;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 3: editor/export/check_templates
// ============================================================================

Dictionary MCPExportTools::handle_check_templates(const Dictionary &p_args) {
	EditorExport *exporter = EditorExport::get_singleton();
	ERR_FAIL_NULL_V(exporter, make_tool_error("Export system not available. This tool requires the Godot editor."));

	EditorPaths *editor_paths = EditorPaths::get_singleton();
	ERR_FAIL_NULL_V(editor_paths, make_tool_error("EditorPaths singleton not available."));

	// The expected template version matches the engine build configuration.
	String template_version = GODOT_VERSION_FULL_CONFIG;
	String templates_dir = editor_paths->get_export_templates_dir();
	String version_dir = templates_dir.path_join(template_version);

	// Check if the version-specific template directory exists.
	bool version_dir_exists = DirAccess::exists(version_dir);

	// Iterate all registered export platforms and check template availability.
	Dictionary platform_status;
	Array platforms_detail;
	int installed_count = 0;
	int total_platforms = exporter->get_export_platform_count();

	for (int i = 0; i < total_platforms; i++) {
		Ref<EditorExportPlatform> platform = exporter->get_export_platform(i);
		if (platform.is_null()) {
			continue;
		}

		String platform_name = platform->get_name();

		// To check template availability for a platform, we need a preset.
		// Create a temporary preset to check against, or try finding an
		// existing one. We use can_export's template check via
		// has_valid_export_configuration.
		bool has_templates = false;

		// First, try to find an existing preset for this platform.
		Ref<EditorExportPreset> test_preset;
		for (int j = 0; j < exporter->get_export_preset_count(); j++) {
			Ref<EditorExportPreset> p = exporter->get_export_preset(j);
			if (p.is_valid() && p->get_platform() == platform) {
				test_preset = p;
				break;
			}
		}

		// If no preset exists, create a temporary one.
		if (test_preset.is_null()) {
			test_preset = platform->create_preset();
		}

		if (test_preset.is_valid()) {
			String error_str;
			bool missing_templates = false;
			platform->has_valid_export_configuration(test_preset, error_str, missing_templates, false);
			has_templates = !missing_templates;
		}

		platform_status[platform_name] = has_templates;

		Dictionary detail;
		detail["name"] = platform_name;
		detail["templates_installed"] = has_templates;
		platforms_detail.push_back(detail);

		if (has_templates) {
			installed_count++;
		}
	}

	// Build text output.
	String text = vformat("Export Template Status (version %s):\n", template_version);
	text += vformat("Template directory: %s\n", version_dir);
	text += vformat("Directory exists: %s\n\n", version_dir_exists ? "yes" : "no");

	for (int i = 0; i < platforms_detail.size(); i++) {
		Dictionary detail = platforms_detail[i];
		String name = detail["name"];
		bool installed = detail["templates_installed"];
		text += vformat("  %s: %s\n", name, installed ? "installed" : "MISSING");
	}

	text += vformat("\n%d / %d platforms have templates installed.",
			installed_count, total_platforms);

	if (installed_count < total_platforms) {
		text += "\n\nTo install missing templates: Editor > Manage Export Templates.";
	}

	// Build structured output.
	Dictionary structured;
	structured["template_version"] = template_version;
	structured["templates_dir"] = version_dir;
	structured["directory_exists"] = version_dir_exists;
	structured["templates_installed"] = platform_status;
	structured["platforms"] = platforms_detail;
	structured["installed_count"] = installed_count;
	structured["total_platforms"] = total_platforms;

	return make_tool_result(text, structured);
}
