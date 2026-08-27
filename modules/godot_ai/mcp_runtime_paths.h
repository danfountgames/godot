/**************************************************************************/
/*  mcp_runtime_paths.h                                                   */
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

#ifndef MCP_RUNTIME_PATHS_H
#define MCP_RUNTIME_PATHS_H

#include "core/string/ustring.h"
#include "core/variant/variant.h"

// Translating between the running game's node paths and the edited scene's.
//
// These are two different trees that happen to look alike, and the difference is exactly
// one level plus a name. The running game addresses `/root/Main/Player`: `/root` is the
// game's own window viewport and `Main` is the instantiated scene. The editor addresses
// the same node as `Player`, relative to the edited scene's root - which *is* `Main`.
//
// Getting this wrong is silent in the worst way. `/root/Main/Player` used as an editor
// path finds nothing, and a promotion that found nothing and said so is fine; a
// promotion that found the *wrong* node and wrote to it is not. So the translation
// insists the scene names match rather than stripping two path components and hoping.
class MCPRuntimePaths {
public:
	// The edited-scene path for a running game's node path, or false with a reason.
	//
	// `p_scene_root_name` is the name of the edited scene's root node. When the running
	// path names a different scene, this refuses: the game is playing something other
	// than what the editor has open, and writing into the open scene would put a value
	// from one scene into another.
	static bool to_scene_path(const String &p_runtime_path, const String &p_scene_root_name,
			String &r_scene_path, String &r_error);

	// The running game's path for an edited-scene path, which is the same rule backwards.
	// Useful for asking "what is this node doing right now" from an editor selection.
	static bool to_runtime_path(const String &p_scene_path, const String &p_scene_root_name,
			String &r_runtime_path, String &r_error);

	// The scene name a runtime path is rooted in - `Main` for `/root/Main/Player` - or an
	// empty string when the path is not of that shape.
	static String scene_name_of(const String &p_runtime_path);
};

// A Variant from Godot's own text form - the inverse of `String(Variant)`.
//
// The runtime agent sends every property value twice: as JSON, which has no Vector2 and
// no Color, and as this text, which round-trips every type. Anything that wants to carry
// a position back out of a running game has to read the second one. Returns a nil
// Variant when the text does not parse, which the caller treats as "fall back to JSON"
// rather than as an error - a value that arrived is better than a refusal about how it
// was spelled.
Variant mcp_variant_from_text(const String &p_text);

#endif // MCP_RUNTIME_PATHS_H
