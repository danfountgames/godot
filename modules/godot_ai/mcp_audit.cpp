/**************************************************************************/
/*  mcp_audit.cpp                                                         */
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

#include "mcp_audit.h"

#include "mcp_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/time.h"

static String s_log_path_override;

void MCPAudit::set_log_path_override(const String &p_path) {
	s_log_path_override = p_path;
}

void MCPAudit::clear_log_path_override() {
	s_log_path_override = String();
}

String MCPAudit::get_log_path() {
	if (!s_log_path_override.is_empty()) {
		return s_log_path_override;
	}
	String project_key = "no-project";
	if (ProjectSettings::get_singleton()) {
		const String resource_path = ProjectSettings::get_singleton()->get_resource_path();
		if (!resource_path.is_empty()) {
			// A stable per-project file name that is safe on every filesystem.
			project_key = resource_path.get_file() + "-" + String::num_uint64(resource_path.hash(), 16);
		}
	}
	return MCPService::get_state_dir().path_join("audit").path_join(project_key + ".log");
}

void MCPAudit::record(const String &p_client, const String &p_tool, const String &p_summary, bool p_allowed, const String &p_reason) {
	const String path = get_log_path();
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_null() || dir->make_dir_recursive(path.get_base_dir()) != OK) {
		return;
	}

	Dictionary entry;
	entry["time"] = Time::get_singleton()->get_datetime_string_from_system(true);
	entry["client"] = p_client;
	entry["tool"] = p_tool;
	// The summary is already redacted by MCPTool::describe_invocation.
	entry["invocation"] = p_summary;
	entry["allowed"] = p_allowed;
	entry["reason"] = p_reason;

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ_WRITE);
	if (file.is_null()) {
		file = FileAccess::open(path, FileAccess::WRITE);
	}
	if (file.is_null()) {
		return;
	}
	file->seek_end();
	file->store_line(JSON::stringify(entry));
}
