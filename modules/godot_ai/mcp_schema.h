/**************************************************************************/
/*  mcp_schema.h                                                          */
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

#ifndef MCP_SCHEMA_H
#define MCP_SCHEMA_H

#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// A deliberately small JSON Schema subset, sized for tool argument validation:
// type, properties, required, additionalProperties, enum, minimum/maximum,
// minLength/maxLength, items, and default filling.
//
// Tool arguments are attacker-influenced input, so validation lives here instead of
// being re-implemented (and eventually forgotten) inside each tool.
class MCPSchema {
	static bool _validate_value(const Variant &p_value, const Dictionary &p_schema, const String &p_path, String &r_error);
	static bool _type_matches(const Variant &p_value, const String &p_type);

public:
	// Validates p_value against p_schema. On success r_value receives the value with
	// documented defaults filled in for absent optional properties.
	static bool validate(const Variant &p_value, const Dictionary &p_schema, Variant &r_value, String &r_error);

	// Builders, so tools declare schemas without hand-writing dictionaries.
	static Dictionary object_schema(const Dictionary &p_properties, const Vector<String> &p_required = Vector<String>(), bool p_additional = false);
	static Dictionary string_property(const String &p_description, const Variant &p_default = Variant());
	static Dictionary bool_property(const String &p_description, const Variant &p_default = Variant());
	static Dictionary integer_property(const String &p_description, const Variant &p_default = Variant());
	// For quantities that are genuinely fractional - a frame budget is 16.7ms, not 16.
	static Dictionary number_property(const String &p_description, const Variant &p_default = Variant());
	static Dictionary enum_property(const String &p_description, const Vector<String> &p_values, const Variant &p_default = Variant());
	static Dictionary array_property(const String &p_description, const Dictionary &p_items);
	// A property with no type constraint, for arguments whose shape depends on what
	// they are addressing (a Godot property value can be any Variant).
	static Dictionary any_property(const String &p_description);

	// Reads a string argument that is spelled two ways.
	//
	// Godot_WriteTextFile has always taken `text` and Godot_WriteUserFile `content`.
	// Renaming either breaks callers, so both tools accept both spellings and this is
	// how they read one. Neither argument can be `required` in the schema when a caller
	// may legitimately send the other, so absence is diagnosed here instead - which is
	// why the message names both spellings rather than the canonical one only.
	//
	// Both present and disagreeing is refused, not silently resolved: picking one would
	// discard content the caller believed it was writing.
	static bool read_aliased_string(const Dictionary &p_arguments, const String &p_canonical,
			const String &p_alias, String &r_value, String &r_error);
};

#endif // MCP_SCHEMA_H
