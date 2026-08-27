/**************************************************************************/
/*  json.cpp                                                              */
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

#include "json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace godot_ai {

JSONValueRef JSONValue::make_null() {
	return std::make_shared<JSONValue>();
}

JSONValueRef JSONValue::make_bool(bool p_value) {
	JSONValueRef value = std::make_shared<JSONValue>();
	value->type = BOOL;
	value->bool_value = p_value;
	return value;
}

JSONValueRef JSONValue::make_number(double p_value) {
	JSONValueRef value = std::make_shared<JSONValue>();
	value->type = NUMBER;
	value->number_value = p_value;
	char buffer[64];
	if (p_value == std::floor(p_value) && std::fabs(p_value) < 1e15) {
		snprintf(buffer, sizeof(buffer), "%lld", (long long)p_value);
	} else {
		snprintf(buffer, sizeof(buffer), "%.17g", p_value);
	}
	value->raw_number = buffer;
	return value;
}

JSONValueRef JSONValue::make_raw_number(const std::string &p_raw) {
	JSONValueRef value = std::make_shared<JSONValue>();
	value->type = NUMBER;
	value->raw_number = p_raw;
	value->number_value = strtod(p_raw.c_str(), nullptr);
	return value;
}

JSONValueRef JSONValue::make_string(const std::string &p_value) {
	JSONValueRef value = std::make_shared<JSONValue>();
	value->type = STRING;
	value->string_value = p_value;
	return value;
}

JSONValueRef JSONValue::make_array() {
	JSONValueRef value = std::make_shared<JSONValue>();
	value->type = ARRAY;
	return value;
}

JSONValueRef JSONValue::make_object() {
	JSONValueRef value = std::make_shared<JSONValue>();
	value->type = OBJECT;
	return value;
}

void JSONValue::push_back(const JSONValueRef &p_value) {
	array_value.push_back(p_value ? p_value : make_null());
}

void JSONValue::set(const std::string &p_key, const JSONValueRef &p_value) {
	JSONValueRef value = p_value ? p_value : make_null();
	for (auto &entry : object_value) {
		if (entry.first == p_key) {
			entry.second = value;
			return;
		}
	}
	object_value.emplace_back(p_key, value);
}

JSONValueRef JSONValue::get(const std::string &p_key) const {
	if (type != OBJECT) {
		return nullptr;
	}
	for (const auto &entry : object_value) {
		if (entry.first == p_key) {
			return entry.second;
		}
	}
	return nullptr;
}

std::string JSONValue::get_string_or(const std::string &p_key, const std::string &p_default) const {
	JSONValueRef value = get(p_key);
	if (value && value->is_string()) {
		return value->get_string();
	}
	return p_default;
}

std::string json_escape(const std::string &p_text) {
	std::string out = "\"";
	for (size_t i = 0; i < p_text.size(); i++) {
		const unsigned char character = (unsigned char)p_text[i];
		switch (character) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\b':
				out += "\\b";
				break;
			case '\f':
				out += "\\f";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default: {
				if (character < 0x20) {
					char buffer[8];
					snprintf(buffer, sizeof(buffer), "\\u%04x", character);
					out += buffer;
				} else {
					out += (char)character;
				}
			} break;
		}
	}
	out += "\"";
	return out;
}

std::string JSONValue::to_string() const {
	switch (type) {
		case NIL:
			return "null";
		case BOOL:
			return bool_value ? "true" : "false";
		case NUMBER:
			return raw_number.empty() ? "0" : raw_number;
		case STRING:
			return json_escape(string_value);
		case ARRAY: {
			std::string out = "[";
			for (size_t i = 0; i < array_value.size(); i++) {
				if (i > 0) {
					out += ",";
				}
				out += array_value[i]->to_string();
			}
			return out + "]";
		}
		case OBJECT: {
			std::string out = "{";
			for (size_t i = 0; i < object_value.size(); i++) {
				if (i > 0) {
					out += ",";
				}
				out += json_escape(object_value[i].first);
				out += ":";
				out += object_value[i].second->to_string();
			}
			return out + "}";
		}
	}
	return "null";
}

class JSONParser {
	const std::string &text;
	size_t pos = 0;
	std::string error;
	int depth = 0;

	static const int MAX_DEPTH = 128;

public:
	explicit JSONParser(const std::string &p_text) :
			text(p_text) {}

	const std::string &get_error() const { return error; }

	void skip_whitespace() {
		while (pos < text.size()) {
			const char character = text[pos];
			if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
				pos++;
			} else {
				break;
			}
		}
	}

	bool at_end() {
		skip_whitespace();
		return pos >= text.size();
	}

	void fail(const std::string &p_message) {
		if (error.empty()) {
			error = p_message + " at offset " + std::to_string(pos);
		}
	}

	static void append_utf8(std::string &r_out, unsigned int p_code) {
		if (p_code < 0x80) {
			r_out += (char)p_code;
		} else if (p_code < 0x800) {
			r_out += (char)(0xC0 | (p_code >> 6));
			r_out += (char)(0x80 | (p_code & 0x3F));
		} else if (p_code < 0x10000) {
			r_out += (char)(0xE0 | (p_code >> 12));
			r_out += (char)(0x80 | ((p_code >> 6) & 0x3F));
			r_out += (char)(0x80 | (p_code & 0x3F));
		} else {
			r_out += (char)(0xF0 | (p_code >> 18));
			r_out += (char)(0x80 | ((p_code >> 12) & 0x3F));
			r_out += (char)(0x80 | ((p_code >> 6) & 0x3F));
			r_out += (char)(0x80 | (p_code & 0x3F));
		}
	}

	bool parse_hex4(unsigned int &r_value) {
		if (pos + 4 > text.size()) {
			fail("truncated unicode escape");
			return false;
		}
		r_value = 0;
		for (int i = 0; i < 4; i++) {
			const char character = text[pos++];
			r_value <<= 4;
			if (character >= '0' && character <= '9') {
				r_value |= (unsigned int)(character - '0');
			} else if (character >= 'a' && character <= 'f') {
				r_value |= (unsigned int)(character - 'a' + 10);
			} else if (character >= 'A' && character <= 'F') {
				r_value |= (unsigned int)(character - 'A' + 10);
			} else {
				fail("invalid unicode escape");
				return false;
			}
		}
		return true;
	}

	bool parse_string(std::string &r_out) {
		if (pos >= text.size() || text[pos] != '"') {
			fail("expected string");
			return false;
		}
		pos++;
		r_out.clear();
		while (pos < text.size()) {
			const char character = text[pos++];
			if (character == '"') {
				return true;
			}
			if (character != '\\') {
				if ((unsigned char)character < 0x20) {
					fail("unescaped control character in string");
					return false;
				}
				r_out += character;
				continue;
			}
			if (pos >= text.size()) {
				fail("truncated escape");
				return false;
			}
			const char escape = text[pos++];
			switch (escape) {
				case '"':
					r_out += '"';
					break;
				case '\\':
					r_out += '\\';
					break;
				case '/':
					r_out += '/';
					break;
				case 'b':
					r_out += '\b';
					break;
				case 'f':
					r_out += '\f';
					break;
				case 'n':
					r_out += '\n';
					break;
				case 'r':
					r_out += '\r';
					break;
				case 't':
					r_out += '\t';
					break;
				case 'u': {
					unsigned int code = 0;
					if (!parse_hex4(code)) {
						return false;
					}
					if (code >= 0xD800 && code <= 0xDBFF && pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
						pos += 2;
						unsigned int low = 0;
						if (!parse_hex4(low)) {
							return false;
						}
						if (low >= 0xDC00 && low <= 0xDFFF) {
							code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
						} else {
							append_utf8(r_out, code);
							code = low;
						}
					}
					append_utf8(r_out, code);
				} break;
				default:
					fail("invalid escape sequence");
					return false;
			}
		}
		fail("unterminated string");
		return false;
	}

	JSONValueRef parse_value() {
		if (depth > MAX_DEPTH) {
			fail("maximum nesting depth exceeded");
			return nullptr;
		}
		skip_whitespace();
		if (pos >= text.size()) {
			fail("unexpected end of input");
			return nullptr;
		}
		switch (text[pos]) {
			case '{': {
				pos++;
				depth++;
				JSONValueRef object = JSONValue::make_object();
				skip_whitespace();
				if (pos < text.size() && text[pos] == '}') {
					pos++;
					depth--;
					return object;
				}
				while (true) {
					skip_whitespace();
					std::string key;
					if (!parse_string(key)) {
						return nullptr;
					}
					skip_whitespace();
					if (pos >= text.size() || text[pos] != ':') {
						fail("expected ':'");
						return nullptr;
					}
					pos++;
					JSONValueRef value = parse_value();
					if (!value) {
						return nullptr;
					}
					object->set(key, value);
					skip_whitespace();
					if (pos < text.size() && text[pos] == ',') {
						pos++;
						continue;
					}
					if (pos < text.size() && text[pos] == '}') {
						pos++;
						depth--;
						return object;
					}
					fail("expected ',' or '}'");
					return nullptr;
				}
			}
			case '[': {
				pos++;
				depth++;
				JSONValueRef array = JSONValue::make_array();
				skip_whitespace();
				if (pos < text.size() && text[pos] == ']') {
					pos++;
					depth--;
					return array;
				}
				while (true) {
					JSONValueRef value = parse_value();
					if (!value) {
						return nullptr;
					}
					array->push_back(value);
					skip_whitespace();
					if (pos < text.size() && text[pos] == ',') {
						pos++;
						continue;
					}
					if (pos < text.size() && text[pos] == ']') {
						pos++;
						depth--;
						return array;
					}
					fail("expected ',' or ']'");
					return nullptr;
				}
			}
			case '"': {
				std::string value;
				if (!parse_string(value)) {
					return nullptr;
				}
				return JSONValue::make_string(value);
			}
			case 't': {
				if (text.compare(pos, 4, "true") == 0) {
					pos += 4;
					return JSONValue::make_bool(true);
				}
				fail("invalid literal");
				return nullptr;
			}
			case 'f': {
				if (text.compare(pos, 5, "false") == 0) {
					pos += 5;
					return JSONValue::make_bool(false);
				}
				fail("invalid literal");
				return nullptr;
			}
			case 'n': {
				if (text.compare(pos, 4, "null") == 0) {
					pos += 4;
					return JSONValue::make_null();
				}
				fail("invalid literal");
				return nullptr;
			}
			default: {
				const size_t start = pos;
				if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
					pos++;
				}
				bool has_digits = false;
				while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E' || text[pos] == '-' || text[pos] == '+')) {
					if (text[pos] >= '0' && text[pos] <= '9') {
						has_digits = true;
					}
					pos++;
				}
				if (!has_digits) {
					fail("unexpected token");
					return nullptr;
				}
				return JSONValue::make_raw_number(text.substr(start, pos - start));
			}
		}
	}
};

JSONValueRef json_parse(const std::string &p_text, std::string *r_error) {
	JSONParser parser(p_text);
	JSONValueRef value = parser.parse_value();
	if (!value) {
		if (r_error) {
			*r_error = parser.get_error();
		}
		return nullptr;
	}
	if (!parser.at_end()) {
		if (r_error) {
			*r_error = "trailing content after JSON document";
		}
		return nullptr;
	}
	return value;
}

} // namespace godot_ai
