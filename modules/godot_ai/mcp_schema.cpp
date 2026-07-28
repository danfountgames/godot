/**************************************************************************/
/*  mcp_schema.cpp                                                        */
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

#include "mcp_schema.h"

#include "core/io/json.h"
#include "core/variant/array.h"

bool MCPSchema::_type_matches(const Variant &p_value, const String &p_type) {
	if (p_type == "object") {
		return p_value.get_type() == Variant::DICTIONARY;
	}
	if (p_type == "array") {
		return p_value.get_type() == Variant::ARRAY || p_value.get_type() == Variant::PACKED_STRING_ARRAY;
	}
	if (p_type == "string") {
		return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME;
	}
	if (p_type == "boolean") {
		return p_value.get_type() == Variant::BOOL;
	}
	if (p_type == "integer") {
		if (p_value.get_type() == Variant::INT) {
			return true;
		}
		// JSON has no integer type; a whole float off the wire is still an integer.
		if (p_value.get_type() == Variant::FLOAT) {
			const double number = p_value;
			return number == (double)(int64_t)number;
		}
		return false;
	}
	if (p_type == "number") {
		return p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT;
	}
	if (p_type == "null") {
		return p_value.get_type() == Variant::NIL;
	}
	// An unknown type constraint must not silently accept everything.
	return false;
}

bool MCPSchema::_validate_value(const Variant &p_value, const Dictionary &p_schema, const String &p_path, String &r_error) {
	const String where = p_path.is_empty() ? String("argument") : ("'" + p_path + "'");

	if (p_schema.has("type")) {
		const Variant type_value = p_schema["type"];
		bool matched = false;
		if (type_value.get_type() == Variant::ARRAY) {
			const Array types = type_value;
			for (int i = 0; i < types.size(); i++) {
				if (_type_matches(p_value, types[i])) {
					matched = true;
					break;
				}
			}
		} else {
			matched = _type_matches(p_value, type_value);
		}
		if (!matched) {
			r_error = vformat("%s must be of type %s", where, String(JSON::stringify(type_value)));
			return false;
		}
	}

	if (p_schema.has("enum")) {
		const Array allowed = p_schema["enum"];
		bool found = false;
		for (int i = 0; i < allowed.size(); i++) {
			if (allowed[i] == p_value) {
				found = true;
				break;
			}
		}
		if (!found) {
			r_error = vformat("%s must be one of %s", where, String(JSON::stringify(allowed)));
			return false;
		}
	}

	if (p_value.get_type() == Variant::STRING) {
		const String text = p_value;
		if (p_schema.has("minLength") && text.length() < (int)p_schema["minLength"]) {
			r_error = vformat("%s must be at least %d characters", where, (int)p_schema["minLength"]);
			return false;
		}
		if (p_schema.has("maxLength") && text.length() > (int)p_schema["maxLength"]) {
			r_error = vformat("%s must be at most %d characters", where, (int)p_schema["maxLength"]);
			return false;
		}
	}

	if (p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT) {
		const double number = p_value;
		if (p_schema.has("minimum") && number < (double)p_schema["minimum"]) {
			r_error = vformat("%s must be >= %s", where, String(p_schema["minimum"]));
			return false;
		}
		if (p_schema.has("maximum") && number > (double)p_schema["maximum"]) {
			r_error = vformat("%s must be <= %s", where, String(p_schema["maximum"]));
			return false;
		}
	}

	if (p_value.get_type() == Variant::ARRAY && p_schema.has("items")) {
		const Array array = p_value;
		const Dictionary items = p_schema["items"];
		for (int i = 0; i < array.size(); i++) {
			if (!_validate_value(array[i], items, p_path + "[" + itos(i) + "]", r_error)) {
				return false;
			}
		}
	}

	if (p_value.get_type() == Variant::DICTIONARY && p_schema.has("properties")) {
		const Dictionary value = p_value;
		const Dictionary properties = p_schema["properties"];

		if (p_schema.has("required")) {
			const Array required = p_schema["required"];
			for (int i = 0; i < required.size(); i++) {
				const String key = required[i];
				if (!value.has(key)) {
					r_error = vformat("missing required argument '%s'", p_path.is_empty() ? key : (p_path + "." + key));
					return false;
				}
			}
		}

		// Unknown arguments are rejected rather than ignored: a caller that misspells
		// an argument must be told, not silently given default behaviour.
		const bool additional_allowed = p_schema.has("additionalProperties") ? (bool)p_schema["additionalProperties"] : true;
		if (!additional_allowed) {
			const Array keys = value.keys();
			for (int i = 0; i < keys.size(); i++) {
				const String key = keys[i];
				if (!properties.has(key)) {
					String known;
					const Array property_names = properties.keys();
					for (int j = 0; j < property_names.size(); j++) {
						known += (j > 0 ? ", " : "") + String(property_names[j]);
					}
					r_error = vformat("unknown argument '%s' (known arguments: %s)", key, known.is_empty() ? "none" : known);
					return false;
				}
			}
		}

		const Array property_names = properties.keys();
		for (int i = 0; i < property_names.size(); i++) {
			const String key = property_names[i];
			if (!value.has(key)) {
				continue;
			}
			const Dictionary property_schema = properties[key];
			if (!_validate_value(value[key], property_schema, p_path.is_empty() ? key : (p_path + "." + key), r_error)) {
				return false;
			}
		}
	}

	return true;
}

bool MCPSchema::validate(const Variant &p_value, const Dictionary &p_schema, Variant &r_value, String &r_error) {
	Variant value = p_value;
	// MCP clients may omit `arguments` entirely for a tool that needs none.
	if (value.get_type() == Variant::NIL && (!p_schema.has("type") || String(p_schema["type"]) == "object")) {
		value = Dictionary();
	}

	if (!_validate_value(value, p_schema, String(), r_error)) {
		return false;
	}

	// Fill declared defaults so tools can read arguments without repeating them.
	if (value.get_type() == Variant::DICTIONARY && p_schema.has("properties")) {
		Dictionary filled = ((Dictionary)value).duplicate();
		const Dictionary properties = p_schema["properties"];
		const Array property_names = properties.keys();
		for (int i = 0; i < property_names.size(); i++) {
			const String key = property_names[i];
			const Dictionary property_schema = properties[key];
			if (!filled.has(key) && property_schema.has("default")) {
				filled[key] = property_schema["default"];
			}
		}
		r_value = filled;
		return true;
	}

	r_value = value;
	return true;
}

Dictionary MCPSchema::object_schema(const Dictionary &p_properties, const Vector<String> &p_required, bool p_additional) {
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = p_properties;
	if (!p_required.is_empty()) {
		Array required;
		for (const String &name : p_required) {
			required.push_back(name);
		}
		schema["required"] = required;
	}
	schema["additionalProperties"] = p_additional;
	return schema;
}

static Dictionary make_property(const String &p_type, const String &p_description, const Variant &p_default) {
	Dictionary property;
	property["type"] = p_type;
	property["description"] = p_description;
	if (p_default.get_type() != Variant::NIL) {
		property["default"] = p_default;
	}
	return property;
}

Dictionary MCPSchema::string_property(const String &p_description, const Variant &p_default) {
	return make_property("string", p_description, p_default);
}

Dictionary MCPSchema::bool_property(const String &p_description, const Variant &p_default) {
	return make_property("boolean", p_description, p_default);
}

Dictionary MCPSchema::integer_property(const String &p_description, const Variant &p_default) {
	return make_property("integer", p_description, p_default);
}

Dictionary MCPSchema::enum_property(const String &p_description, const Vector<String> &p_values, const Variant &p_default) {
	Dictionary property = make_property("string", p_description, p_default);
	Array values;
	for (const String &value : p_values) {
		values.push_back(value);
	}
	property["enum"] = values;
	return property;
}

Dictionary MCPSchema::array_property(const String &p_description, const Dictionary &p_items) {
	Dictionary property = make_property("array", p_description, Variant());
	property["items"] = p_items;
	return property;
}
