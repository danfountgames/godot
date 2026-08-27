/**************************************************************************/
/*  mcp_runtime_paths.cpp                                                 */
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

#include "mcp_runtime_paths.h"

#include "core/variant/variant_parser.h"

namespace {

// The path split into its components, with empty ones dropped. `/root/Main/Player`
// becomes ["root", "Main", "Player"].
Vector<String> components_of(const String &p_path) {
	Vector<String> components;
	for (const String &part : p_path.strip_edges().split("/", false)) {
		const String trimmed = part.strip_edges();
		if (!trimmed.is_empty()) {
			components.push_back(trimmed);
		}
	}
	return components;
}

} // namespace

String MCPRuntimePaths::scene_name_of(const String &p_runtime_path) {
	const Vector<String> components = components_of(p_runtime_path);
	if (components.size() < 2 || components[0] != "root") {
		return String();
	}
	return components[1];
}

bool MCPRuntimePaths::to_scene_path(const String &p_runtime_path, const String &p_scene_root_name,
		String &r_scene_path, String &r_error) {
	r_scene_path = String();
	r_error = String();

	const String trimmed = p_runtime_path.strip_edges();
	if (trimmed.is_empty()) {
		r_error = "a runtime node path is needed";
		return false;
	}
	if (p_scene_root_name.strip_edges().is_empty()) {
		r_error = "there is no scene open in the editor to promote into";
		return false;
	}

	const Vector<String> components = components_of(trimmed);
	if (components.is_empty() || components[0] != "root") {
		r_error = vformat("'%s' is not a running-game path; those start at /root, as in "
						  "/root/%s/Player",
				p_runtime_path, p_scene_root_name);
		return false;
	}
	if (components.size() < 2) {
		// `/root` is the game's window viewport, not a node in any scene.
		r_error = "'/root' is the running game's viewport, not a node in the scene";
		return false;
	}
	if (components[1] != p_scene_root_name) {
		// The game is playing something other than what the editor has open. Writing into
		// the open scene anyway would put a value from one scene into another, and the
		// result would look plausible.
		r_error = vformat("the running game is playing '%s' but the editor has '%s' open; "
						  "open '%s' before promoting a value out of it",
				components[1], p_scene_root_name, components[1]);
		return false;
	}

	if (components.size() == 2) {
		// The scene root itself, which the editor addresses as ".".
		r_scene_path = ".";
		return true;
	}

	String path;
	for (int i = 2; i < components.size(); i++) {
		if (!path.is_empty()) {
			path += "/";
		}
		path += components[i];
	}
	r_scene_path = path;
	return true;
}

bool MCPRuntimePaths::to_runtime_path(const String &p_scene_path, const String &p_scene_root_name,
		String &r_runtime_path, String &r_error) {
	r_runtime_path = String();
	r_error = String();

	if (p_scene_root_name.strip_edges().is_empty()) {
		r_error = "there is no scene open in the editor";
		return false;
	}

	const String trimmed = p_scene_path.strip_edges();
	if (trimmed.begins_with("/root")) {
		r_error = vformat("'%s' is already a running-game path", p_scene_path);
		return false;
	}

	if (trimmed.is_empty() || trimmed == ".") {
		r_runtime_path = "/root/" + p_scene_root_name;
		return true;
	}

	const Vector<String> components = components_of(trimmed);
	String path = "/root/" + p_scene_root_name;
	for (int i = 0; i < components.size(); i++) {
		path += "/" + components[i];
	}
	r_runtime_path = path;
	return true;
}

Variant mcp_variant_from_text(const String &p_text) {
	const String trimmed = p_text.strip_edges();
	if (trimmed.is_empty()) {
		return Variant();
	}

	VariantParser::StreamString stream;
	stream.s = trimmed;

	Variant parsed;
	String error;
	int line = 0;
	if (VariantParser::parse(&stream, parsed, error, line) != OK) {
		// Not an error to report: the caller has the JSON value to fall back on, and a
		// refusal about spelling would be worse than a value that arrived.
		return Variant();
	}
	return parsed;
}
