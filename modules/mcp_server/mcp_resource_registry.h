/**************************************************************************/
/*  mcp_resource_registry.h                                               */
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

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"

class MCPResourceRegistry {
public:
	// ── Registration structs ──

	struct ResourceDef {
		String uri;
		String name;
		String description;
		String mime_type;
		Callable handler; // Returns Dictionary with "text" key containing content.
		bool requires_game = false; // If true, only listed/readable when game is running.
		bool subscribable = false; // If true, supports change notifications.
	};

	struct ResourceTemplateDef {
		String uri_template; // RFC 6570 Level 1 URI template, e.g. "godot://file/{path}"
		String name;
		String description;
		String mime_type;
		Callable handler; // Called with Dictionary containing extracted template params.
	};

private:
	// ── Resource storage ──
	HashMap<String, ResourceDef> resources;
	Vector<ResourceTemplateDef> templates;

	// ── Subscription tracking (stub -- delivery requires SSE) ──
	HashMap<String, HashSet<String>> subscriptions;
	Mutex subscription_mutex;

	// ── Pending notification queue ──
	Vector<String> pending_notifications;
	Mutex notification_mutex;

	// ── Internal helpers ──
	bool _try_match_template(const String &p_uri,
			ResourceTemplateDef &r_template,
			Dictionary &r_params) const;

public:
	// ── Registration ──
	void register_resource(const ResourceDef &p_def);
	void register_template(const ResourceTemplateDef &p_def);

	// ── MCP method handlers ──
	Dictionary handle_list(bool p_game_running);
	Dictionary handle_read(const String &p_uri, bool p_game_running);
	Dictionary handle_templates_list();

	// ── Subscription management (stubs) ──
	Dictionary handle_subscribe(const String &p_uri, const String &p_session_id);
	Dictionary handle_unsubscribe(const String &p_uri, const String &p_session_id);
	void unsubscribe_all(const String &p_session_id);

	// ── Notification dispatch (queue only, delivery via SSE later) ──
	void notify_changed(const String &p_uri);
	Vector<String> flush_notifications(const String &p_session_id);
};
