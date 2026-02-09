/**************************************************************************/
/*  mcp_types.h                                                           */
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

#include "core/config/project_settings.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// ---------------------------------------------------------------------------
// Buffer and connection limits
// ---------------------------------------------------------------------------

// Maximum HTTP request buffer size per client (4 MB).
#define MCP_MAX_BUFFER_SIZE 4194304

// Maximum number of concurrent MCP clients.
#define MCP_MAX_CLIENTS 8

// ---------------------------------------------------------------------------
// Default server configuration
// ---------------------------------------------------------------------------

#define MCP_DEFAULT_PORT 6009
#define MCP_DEFAULT_HOST "127.0.0.1"
#define MCP_DEFAULT_SESSION_TIMEOUT_SEC 300
#define MCP_DEFAULT_CONNECTION_IDLE_TIMEOUT_SEC 60

// Maximum SSE / notification event queue size per session/stream (backpressure cap).
#define MCP_MAX_EVENT_QUEUE_SIZE 1000

// ---------------------------------------------------------------------------
// MCP Protocol version
// ---------------------------------------------------------------------------

#define MCP_PROTOCOL_VERSION "2025-06-18"

// ---------------------------------------------------------------------------
// JSON-RPC / MCP error codes
// ---------------------------------------------------------------------------

// Standard JSON-RPC error: Internal error (-32603).
#define MCP_ERROR_INTERNAL -32603

// Implementation-defined error: resource unavailable / not found (-32000 to -32099 range).
#define MCP_ERROR_RESOURCE_UNAVAILABLE -32002

// ---------------------------------------------------------------------------
// HTTP status codes used in responses
// ---------------------------------------------------------------------------

#define MCP_HTTP_200 "200 OK"
#define MCP_HTTP_202 "202 Accepted"
#define MCP_HTTP_204 "204 No Content"
#define MCP_HTTP_400 "400 Bad Request"
#define MCP_HTTP_401 "401 Unauthorized"
#define MCP_HTTP_403 "403 Forbidden"
#define MCP_HTTP_404 "404 Not Found"
#define MCP_HTTP_405 "405 Method Not Allowed"
#define MCP_HTTP_500 "500 Internal Server Error"

// ---------------------------------------------------------------------------
// Session ID header name
// ---------------------------------------------------------------------------

#define MCP_SESSION_HEADER "mcp-session-id"

// ---------------------------------------------------------------------------
// Path traversal protection
// ---------------------------------------------------------------------------

inline bool validate_path(const String &p_path) {
	// 1. Must start with res:// or user://
	if (!p_path.begins_with("res://") && !p_path.begins_with("user://")) {
		return false;
	}

	// 2. Reject path traversal sequences.
	if (p_path.find("..") != -1) {
		return false;
	}

	// 3. Reject null byte injection (path truncation attack).
	// Also reject U+FFFD (replacement character) — Godot's JSON parser
	// converts raw null bytes (\x00) to U+FFFD before the string reaches us.
	for (int i = 0; i < p_path.length(); i++) {
		if (p_path[i] == '\0' || p_path[i] == 0xFFFD) {
			return false;
		}
	}

	// 4. Reject URL-encoded traversal sequences (%2e = '.', %2f = '/').
	// These could bypass literal ".." checks if any layer URL-decodes.
	String lower = p_path.to_lower();
	if (lower.find("%2e") != -1 || lower.find("%2f") != -1 || lower.find("%00") != -1) {
		return false;
	}

	// 5. Reject access to Godot internal directories (.godot/, .import/).
	// These contain editor cache, imported resources, and should not be
	// written to or read from via MCP.
	String relative = p_path.begins_with("res://") ? p_path.substr(6) : p_path.substr(7);
	if (relative.begins_with(".godot/") || relative.begins_with(".godot") ||
			relative.begins_with(".import/") || relative.begins_with(".import")) {
		return false;
	}

	// 6. Canonicalize and verify it stays within project root.
	String canonical = ProjectSettings::get_singleton()
							   ->globalize_path(p_path)
							   .simplify_path();
	String project_root = ProjectSettings::get_singleton()
								  ->get_resource_path();

	return canonical.begins_with(project_root);
}

// ---------------------------------------------------------------------------
// MCP tool result helpers
// ---------------------------------------------------------------------------

inline Dictionary make_tool_error(const String &p_message) {
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = p_message;
	Array content;
	content.push_back(content_item);

	Dictionary result;
	result["content"] = content;
	result["isError"] = true;
	return result;
}

inline Dictionary make_tool_result(const String &p_text, const Dictionary &p_structured = Dictionary()) {
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = p_text;
	Array content;
	content.push_back(content_item);

	Dictionary result;
	result["content"] = content;
	if (!p_structured.is_empty()) {
		result["structuredContent"] = p_structured;
	}
	return result;
}

// ---------------------------------------------------------------------------
// MCP tool schema builder helpers (shared by all tool registration files)
// ---------------------------------------------------------------------------

inline Dictionary make_schema(const Dictionary &p_properties,
		const Array &p_required = Array()) {
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = p_properties;
	schema["required"] = p_required;
	return schema;
}

inline Dictionary make_prop(const String &p_type, const String &p_description) {
	Dictionary prop;
	prop["type"] = p_type;
	prop["description"] = p_description;
	return prop;
}

inline Dictionary make_annotations(bool p_read_only, bool p_destructive,
		bool p_idempotent, bool p_open_world = false) {
	Dictionary ann;
	ann["readOnlyHint"] = p_read_only;
	ann["destructiveHint"] = p_destructive;
	ann["idempotentHint"] = p_idempotent;
	ann["openWorldHint"] = p_open_world;
	return ann;
}

// ---------------------------------------------------------------------------
// Resource error response helper (shared by resource handlers)
// ---------------------------------------------------------------------------

inline Dictionary make_resource_error(int p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary response;
	response["error"] = error;
	return response;
}

// ---------------------------------------------------------------------------
// Directory skip-list helper (shared by recursive file walkers)
// ---------------------------------------------------------------------------

inline bool is_skip_directory(const String &p_name) {
	return p_name == ".godot" || p_name == ".import" || p_name == ".git" ||
			p_name == ".svn" || p_name == ".vs" || p_name == "__pycache__" ||
			p_name.begins_with(".godot");
}
