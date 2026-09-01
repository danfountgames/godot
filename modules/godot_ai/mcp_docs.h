/**************************************************************************/
/*  mcp_docs.h                                                            */
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

#ifndef MCP_DOCS_H
#define MCP_DOCS_H

#include "core/doc_data.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// Reading the editor's own class reference.
//
// The engine ships 800-odd documented classes and the agent could not see one of
// them, so it worked from whatever API it had memorised. That is wrong twice over
// here: this is a 4.8-dev fork, and the reference also contains documentation
// generated from the *project's own scripts*, which no model has ever seen. Being
// able to read it turns "look up Godot" into "look up this project".
//
// Everything is shaped around not answering with too much. Node3D has hundreds of
// members; returning them all is the same mistake as returning every memory note on
// every recall. The default answer is a summary plus member names, and full text is
// something the caller asks for by naming a member.
class MCPDocs {
public:
	static constexpr int DEFAULT_LIMIT = 40;

	struct Query {
		String class_name;
		String search;
		String member;
		bool include_inherited = false;
		int limit = DEFAULT_LIMIT;
	};

	enum Result {
		RESULT_OK,
		RESULT_NOT_FOUND, // No such class or member.
		RESULT_INVALID, // The query itself does not make sense.
		RESULT_UNAVAILABLE, // No documentation in this build or this run.
	};

	static Result lookup(const Query &p_query, Dictionary &r_out, String &r_error);

	// --- Pure helpers, free of the editor so they can be tested directly. ---

	// Case-insensitive substring match. An empty needle matches everything, which is
	// what makes "list the members" and "search the members" one code path.
	static bool matches(const String &p_haystack, const String &p_needle);

	// "move_and_slide() -> bool", "get_node(path: NodePath) -> Node". Reads like the
	// call the caller is about to write, which is the point of showing it at all.
	static String signature(const DocData::MethodDoc &p_method);

	// First sentence of a description, for a listing. Godot's docs are BBCode; the
	// markup is stripped rather than passed on, since nothing downstream renders it.
	static String brief_of(const String &p_description);

	// Strips BBCode tags ([b], [code], [param x], [Node]) leaving the text. Link
	// tags keep their target, because "[Node]" carries the name the reader needs.
	static String strip_bbcode(const String &p_text);

	// "core", "editor", "extension" and the rest, said in a way that tells the caller
	// whether they are looking at the engine or at their own code.
	static String describe_api_type(const String &p_api_type);
};

#endif // MCP_DOCS_H
