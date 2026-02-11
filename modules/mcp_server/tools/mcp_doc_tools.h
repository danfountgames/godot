/**************************************************************************/
/*  mcp_doc_tools.h                                                       */
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

#pragma once

#include "core/doc_data.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPDocTools {
public:
	static void register_tools(MCPToolRegistry *p_registry);

private:
	// Tool handlers.
	static Dictionary handle_search_classes(const Dictionary &p_args);
	static Dictionary handle_get_class(const Dictionary &p_args);
	static Dictionary handle_search_methods(const Dictionary &p_args);
	static Dictionary handle_get_method(const Dictionary &p_args);
	static Dictionary handle_get_property(const Dictionary &p_args);

	// Helpers.
	static String _bbcode_to_text(const String &p_bbcode);
	static String _format_method_sig(const DocData::MethodDoc &p_method);
	static String _format_property_sig(const DocData::PropertyDoc &p_prop);
	static int _score_class(const DocData::ClassDoc &p_class, const Vector<String> &p_terms);
	static int _score_method(const DocData::MethodDoc &p_method, const Vector<String> &p_terms);
	static bool _all_terms_match(const String &p_haystack, const Vector<String> &p_terms);
	static String _get_inheritance_chain(const String &p_class_name);
	static bool _class_matches_category(const DocData::ClassDoc &p_class, const String &p_category);
	static String _build_class_summary(const DocData::ClassDoc &p_class);
	static String _build_class_full(const DocData::ClassDoc &p_class, const String &p_sections, bool p_include_inherited);
};
