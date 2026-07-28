/**************************************************************************/
/*  json.h                                                                */
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

// Minimal JSON reader/writer for godot-ai-relay.
//
// The relay is deliberately independent of the engine, so it cannot use Godot's
// JSON class. It only needs enough JSON to inspect message envelopes (id, method,
// jsonrpc) and to synthesise well-formed error responses; heavier JSON handling
// belongs on the editor side.

#ifndef GODOT_AI_RELAY_JSON_H
#define GODOT_AI_RELAY_JSON_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace godot_ai {

class JSONValue;
using JSONValueRef = std::shared_ptr<JSONValue>;

class JSONValue {
public:
	enum Type {
		NIL,
		BOOL,
		NUMBER,
		STRING,
		ARRAY,
		OBJECT,
	};

private:
	Type type = NIL;
	bool bool_value = false;
	double number_value = 0.0;
	// Numbers are kept verbatim so integer ids round-trip without turning into
	// floats (a JSON-RPC id of 1 must not come back as 1.0).
	std::string raw_number;
	std::string string_value;
	std::vector<JSONValueRef> array_value;
	std::vector<std::pair<std::string, JSONValueRef>> object_value;

public:
	static JSONValueRef make_null();
	static JSONValueRef make_bool(bool p_value);
	static JSONValueRef make_number(double p_value);
	static JSONValueRef make_raw_number(const std::string &p_raw);
	static JSONValueRef make_string(const std::string &p_value);
	static JSONValueRef make_array();
	static JSONValueRef make_object();

	Type get_type() const { return type; }
	bool is_null() const { return type == NIL; }
	bool is_object() const { return type == OBJECT; }
	bool is_array() const { return type == ARRAY; }
	bool is_string() const { return type == STRING; }
	bool is_number() const { return type == NUMBER; }
	bool is_bool() const { return type == BOOL; }

	bool get_bool() const { return bool_value; }
	double get_number() const { return number_value; }
	const std::string &get_string() const { return string_value; }
	const std::vector<JSONValueRef> &get_array() const { return array_value; }

	void push_back(const JSONValueRef &p_value);
	void set(const std::string &p_key, const JSONValueRef &p_value);
	// Returns nullptr when the key is absent or this is not an object.
	JSONValueRef get(const std::string &p_key) const;
	std::string get_string_or(const std::string &p_key, const std::string &p_default) const;

	std::string to_string() const;

	friend class JSONParser;
};

// Escapes a string as a JSON string literal, including surrounding quotes.
std::string json_escape(const std::string &p_text);

// Parses one JSON document. Returns nullptr on failure and fills r_error.
JSONValueRef json_parse(const std::string &p_text, std::string *r_error = nullptr);

} // namespace godot_ai

#endif // GODOT_AI_RELAY_JSON_H
