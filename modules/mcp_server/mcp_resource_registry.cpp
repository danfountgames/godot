/**************************************************************************/
/*  mcp_resource_registry.cpp                                             */
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

#include "mcp_resource_registry.h"

#include "core/variant/variant.h"

// ============================================================================
// Registration
// ============================================================================

void MCPResourceRegistry::register_resource(const ResourceDef &p_def) {
	resources[p_def.uri] = p_def;
}

void MCPResourceRegistry::register_template(const ResourceTemplateDef &p_def) {
	templates.push_back(p_def);
}

// ============================================================================
// resources/list
// ============================================================================

Dictionary MCPResourceRegistry::handle_list(bool p_game_running) {
	Array resource_list;

	for (const KeyValue<String, ResourceDef> &E : resources) {
		const ResourceDef &def = E.value;

		// Skip dynamic resources when game is not running.
		if (def.requires_game && !p_game_running) {
			continue;
		}

		Dictionary entry;
		entry["uri"] = def.uri;
		entry["name"] = def.name;
		entry["description"] = def.description;
		entry["mimeType"] = def.mime_type;
		resource_list.push_back(entry);
	}

	Dictionary result;
	result["resources"] = resource_list;
	return result;
}

// ============================================================================
// resources/read
// ============================================================================

Dictionary MCPResourceRegistry::handle_read(const String &p_uri, bool p_game_running) {
	// Step 1: Check for exact match in registered resources.
	if (resources.has(p_uri)) {
		const ResourceDef &def = resources[p_uri];

		// Gate dynamic resources on game running state.
		if (def.requires_game && !p_game_running) {
			Dictionary error;
			error["code"] = -32002;
			error["message"] = "Resource unavailable: " + p_uri +
					" requires a running game. Use debug/run_project first.";
			Dictionary response;
			response["error"] = error;
			return response;
		}

		// Call the handler to produce content.
		Variant handler_result = def.handler.call();
		if (handler_result.get_type() != Variant::DICTIONARY) {
			Dictionary error;
			error["code"] = -32603;
			error["message"] = "Internal error: handler for " + p_uri +
					" returned non-Dictionary";
			Dictionary response;
			response["error"] = error;
			return response;
		}

		Dictionary content_item = handler_result;
		// Ensure URI and mimeType are set.
		content_item["uri"] = p_uri;
		content_item["mimeType"] = def.mime_type;

		Array contents;
		contents.push_back(content_item);

		Dictionary result;
		result["contents"] = contents;
		return result;
	}

	// Step 2: Check for template match.
	ResourceTemplateDef matched_template;
	Dictionary matched_params;
	if (_try_match_template(p_uri, matched_template, matched_params)) {
		// Call the template handler with extracted params.
		Variant handler_result = matched_template.handler.call(matched_params);
		if (handler_result.get_type() != Variant::DICTIONARY) {
			Dictionary error;
			error["code"] = -32603;
			error["message"] = "Internal error: template handler returned non-Dictionary";
			Dictionary response;
			response["error"] = error;
			return response;
		}

		Dictionary content_item = handler_result;

		// Check if the handler returned an error (e.g., file not found).
		if (content_item.has("error")) {
			return content_item; // Pass through error response.
		}

		content_item["uri"] = p_uri;
		content_item["mimeType"] = matched_template.mime_type;

		Array contents;
		contents.push_back(content_item);

		Dictionary result;
		result["contents"] = contents;
		return result;
	}

	// Step 3: Unknown URI.
	Dictionary error;
	error["code"] = -32002;
	error["message"] = "Resource not found: " + p_uri;
	Dictionary response;
	response["error"] = error;
	return response;
}

// ============================================================================
// resources/templates/list
// ============================================================================

Dictionary MCPResourceRegistry::handle_templates_list() {
	Array template_list;

	for (const ResourceTemplateDef &tmpl : templates) {
		Dictionary entry;
		entry["uriTemplate"] = tmpl.uri_template;
		entry["name"] = tmpl.name;
		entry["description"] = tmpl.description;
		entry["mimeType"] = tmpl.mime_type;
		template_list.push_back(entry);
	}

	Dictionary result;
	result["resourceTemplates"] = template_list;
	return result;
}

// ============================================================================
// Template Matching (RFC 6570 Level 1)
// ============================================================================

bool MCPResourceRegistry::_try_match_template(const String &p_uri,
		ResourceTemplateDef &r_template,
		Dictionary &r_params) const {
	// RFC 6570 Level 1 matching:
	//   "godot://file/{path}"       matches "godot://file/scripts/player.gd"
	//   "godot://game/node/{node_id}/properties" matches "godot://game/node/12345/properties"
	//
	// Algorithm: For each template, find the {param} brace pair. Split the
	// template into prefix and suffix. If the URI starts with prefix and ends
	// with suffix, the middle portion is the parameter value.

	for (const ResourceTemplateDef &tmpl : templates) {
		String pattern = tmpl.uri_template;
		int brace_start = pattern.find("{");
		if (brace_start < 0) {
			continue;
		}

		int brace_end = pattern.find("}", brace_start);
		if (brace_end < 0) {
			continue;
		}

		String prefix = pattern.substr(0, brace_start);
		String param_name = pattern.substr(brace_start + 1, brace_end - brace_start - 1);
		String suffix = pattern.substr(brace_end + 1);

		// The URI must start with the prefix and end with the suffix.
		if (!p_uri.begins_with(prefix)) {
			continue;
		}
		if (!suffix.is_empty() && !p_uri.ends_with(suffix)) {
			continue;
		}

		// Extract the parameter value (the part between prefix and suffix).
		int value_length = p_uri.length() - prefix.length() - suffix.length();
		if (value_length <= 0) {
			continue; // Empty parameter value -- reject.
		}

		String value = p_uri.substr(prefix.length(), value_length);
		if (value.is_empty()) {
			continue;
		}

		r_template = tmpl;
		r_params[param_name] = value;
		return true;
	}

	return false;
}

// ============================================================================
// Subscription Management (stubs -- delivery requires SSE from AGENT_06)
// ============================================================================

Dictionary MCPResourceRegistry::handle_subscribe(const String &p_uri,
		const String &p_session_id) {
	MutexLock lock(subscription_mutex);
	subscriptions[p_uri].insert(p_session_id);

	// Return empty result on success per MCP spec.
	Dictionary result;
	return result;
}

Dictionary MCPResourceRegistry::handle_unsubscribe(const String &p_uri,
		const String &p_session_id) {
	MutexLock lock(subscription_mutex);
	if (subscriptions.has(p_uri)) {
		subscriptions[p_uri].erase(p_session_id);
		if (subscriptions[p_uri].is_empty()) {
			subscriptions.erase(p_uri);
		}
	}

	Dictionary result;
	return result;
}

void MCPResourceRegistry::unsubscribe_all(const String &p_session_id) {
	MutexLock lock(subscription_mutex);
	Vector<String> empty_uris;
	for (KeyValue<String, HashSet<String>> &E : subscriptions) {
		E.value.erase(p_session_id);
		if (E.value.is_empty()) {
			empty_uris.push_back(E.key);
		}
	}
	for (const String &uri : empty_uris) {
		subscriptions.erase(uri);
	}
}

// ============================================================================
// Notification Dispatch (queue only -- delivery via AGENT_06 SSE)
// ============================================================================

void MCPResourceRegistry::notify_changed(const String &p_uri) {
	// Only queue if someone is subscribed.
	{
		MutexLock slock(subscription_mutex);
		if (!subscriptions.has(p_uri)) {
			return;
		}
	}

	MutexLock nlock(notification_mutex);
	// Deduplicate: if this URI is already pending, skip.
	if (pending_notifications.find(p_uri) == -1) {
		pending_notifications.push_back(p_uri);
	}
}

Vector<String> MCPResourceRegistry::flush_notifications(const String &p_session_id) {
	MutexLock nlock(notification_mutex);
	MutexLock slock(subscription_mutex);

	Vector<String> result;
	for (const String &uri : pending_notifications) {
		if (subscriptions.has(uri) && subscriptions[uri].has(p_session_id)) {
			result.push_back(uri);
		}
	}
	// Clear all pending notifications after flush.
	// NOTE: This means if multiple sessions are subscribed, only the first
	// to flush gets notifications. AGENT_06 must change this to per-session
	// queues when SSE delivery is implemented.
	pending_notifications.clear();
	return result;
}
