/**************************************************************************/
/*  debug_semantic_registry.cpp                                           */
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

#include "debug_semantic_registry.h"

#include "core/config/engine.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/variant_utility.h"
#include "scene/main/node.h"

DebugSemanticRegistry *DebugSemanticRegistry::singleton = nullptr;

DebugSemanticRegistry::DebugSemanticRegistry() {
	singleton = this;

#ifdef DEBUG_ENABLED
	event_log.resize(EVENT_LOG_CAPACITY);
	console_log.resize(CONSOLE_LOG_CAPACITY);
#endif
}

DebugSemanticRegistry::~DebugSemanticRegistry() {
	singleton = nullptr;
}

void DebugSemanticRegistry::_bind_methods() {
	// Actions.
	ClassDB::bind_method(D_METHOD("register_action", "name", "callable", "description", "params", "category"), &DebugSemanticRegistry::register_action, DEFVAL(""), DEFVAL(Dictionary()), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("unregister_action", "name"), &DebugSemanticRegistry::unregister_action);
	ClassDB::bind_method(D_METHOD("invoke_action", "name", "params"), &DebugSemanticRegistry::invoke_action, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("has_action", "name"), &DebugSemanticRegistry::has_action);
	ClassDB::bind_method(D_METHOD("get_action_list"), &DebugSemanticRegistry::get_action_list);

	// Queries.
	ClassDB::bind_method(D_METHOD("register_query", "name", "callable", "description", "category"), &DebugSemanticRegistry::register_query, DEFVAL(""), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("unregister_query", "name"), &DebugSemanticRegistry::unregister_query);
	ClassDB::bind_method(D_METHOD("evaluate_query", "name"), &DebugSemanticRegistry::evaluate_query);
	ClassDB::bind_method(D_METHOD("has_query", "name"), &DebugSemanticRegistry::has_query);
	ClassDB::bind_method(D_METHOD("get_query_list"), &DebugSemanticRegistry::get_query_list);

	// Events.
	ClassDB::bind_method(D_METHOD("register_event", "name", "signal_callable", "description", "category"), &DebugSemanticRegistry::register_event, DEFVAL(""), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("unregister_event", "name"), &DebugSemanticRegistry::unregister_event);
	ClassDB::bind_method(D_METHOD("has_event", "name"), &DebugSemanticRegistry::has_event);
	ClassDB::bind_method(D_METHOD("get_event_list"), &DebugSemanticRegistry::get_event_list);
	ClassDB::bind_method(D_METHOD("get_recent_events", "count"), &DebugSemanticRegistry::get_recent_events, DEFVAL(20));

	// Interactables.
	ClassDB::bind_method(D_METHOD("register_interactable", "name", "node", "type", "description", "valid_actions", "category"), &DebugSemanticRegistry::register_interactable, DEFVAL("logic"), DEFVAL(""), DEFVAL(Array()), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("unregister_interactable", "name"), &DebugSemanticRegistry::unregister_interactable);
	ClassDB::bind_method(D_METHOD("has_interactable", "name"), &DebugSemanticRegistry::has_interactable);
	ClassDB::bind_method(D_METHOD("get_interactable_list"), &DebugSemanticRegistry::get_interactable_list);

	// CVars.
	ClassDB::bind_method(D_METHOD("register_cvar", "name", "default_value", "description", "options"), &DebugSemanticRegistry::register_cvar, DEFVAL(""), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("unregister_cvar", "name"), &DebugSemanticRegistry::unregister_cvar);
	ClassDB::bind_method(D_METHOD("set_cvar", "name", "value"), &DebugSemanticRegistry::set_cvar);
	ClassDB::bind_method(D_METHOD("get_cvar", "name"), &DebugSemanticRegistry::get_cvar);
	ClassDB::bind_method(D_METHOD("cvar_bool", "name", "default"), &DebugSemanticRegistry::cvar_bool, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("cvar_int", "name", "default"), &DebugSemanticRegistry::cvar_int, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("cvar_float", "name", "default"), &DebugSemanticRegistry::cvar_float, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("cvar_string", "name", "default"), &DebugSemanticRegistry::cvar_string, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("has_cvar", "name"), &DebugSemanticRegistry::has_cvar);
	ClassDB::bind_method(D_METHOD("get_cvar_list"), &DebugSemanticRegistry::get_cvar_list);

	// Commands.
	ClassDB::bind_method(D_METHOD("register_command", "name", "callable", "description", "params", "category"), &DebugSemanticRegistry::register_command, DEFVAL(""), DEFVAL(Dictionary()), DEFVAL(""));
	ClassDB::bind_method(D_METHOD("unregister_command", "name"), &DebugSemanticRegistry::unregister_command);
	ClassDB::bind_method(D_METHOD("set_command_completion", "name", "completion_func"), &DebugSemanticRegistry::set_command_completion);
	ClassDB::bind_method(D_METHOD("execute_command", "name", "args"), &DebugSemanticRegistry::execute_command);
	ClassDB::bind_method(D_METHOD("has_command", "name"), &DebugSemanticRegistry::has_command);
	ClassDB::bind_method(D_METHOD("get_command_list"), &DebugSemanticRegistry::get_command_list);

	// UI Pages.
	ClassDB::bind_method(D_METHOD("register_ui_page", "name", "node", "description", "options"), &DebugSemanticRegistry::register_ui_page, DEFVAL(""), DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("unregister_ui_page", "name"), &DebugSemanticRegistry::unregister_ui_page);
	ClassDB::bind_method(D_METHOD("has_ui_page", "name"), &DebugSemanticRegistry::has_ui_page);
	ClassDB::bind_method(D_METHOD("get_ui_page_list"), &DebugSemanticRegistry::get_ui_page_list);
	ClassDB::bind_method(D_METHOD("get_active_ui_page"), &DebugSemanticRegistry::get_active_ui_page);
	ClassDB::bind_method(D_METHOD("get_ui_page_info", "name"), &DebugSemanticRegistry::get_ui_page_info);
	ClassDB::bind_method(D_METHOD("get_ui_navigation_graph"), &DebugSemanticRegistry::get_ui_navigation_graph);

	// Auto-expose.
	ClassDB::bind_method(D_METHOD("auto_expose", "object", "tag"), &DebugSemanticRegistry::auto_expose, DEFVAL(""));

	// Logging.
	ClassDB::bind_method(D_METHOD("log", "message"), &DebugSemanticRegistry::log);
	ClassDB::bind_method(D_METHOD("log_warning", "message"), &DebugSemanticRegistry::log_warning);
	ClassDB::bind_method(D_METHOD("log_error", "message"), &DebugSemanticRegistry::log_error);

	// Manifest.
	ClassDB::bind_method(D_METHOD("get_manifest"), &DebugSemanticRegistry::get_manifest);

	// Internal (used for signal connections, not meant for GDScript).
	ClassDB::bind_method(D_METHOD("_event_fired_0", "event_name"), &DebugSemanticRegistry::_event_fired_0);
	ClassDB::bind_method(D_METHOD("_event_fired_1", "a1", "event_name"), &DebugSemanticRegistry::_event_fired_1);
	ClassDB::bind_method(D_METHOD("_event_fired_2", "a1", "a2", "event_name"), &DebugSemanticRegistry::_event_fired_2);
	ClassDB::bind_method(D_METHOD("_event_fired_3", "a1", "a2", "a3", "event_name"), &DebugSemanticRegistry::_event_fired_3);

	// CVar flag constants (exposed as plain ints since VARIANT_ENUM_CAST
	// requires heavy includes we don't want in the header).
	BIND_CONSTANT(CVAR_NONE);
	BIND_CONSTANT(CVAR_ARCHIVE);
	BIND_CONSTANT(CVAR_READONLY);
	BIND_CONSTANT(CVAR_CHEAT);
	BIND_CONSTANT(CVAR_HIDDEN);
}

// =============================================================================
// DEBUG_ENABLED implementation
// =============================================================================

#ifdef DEBUG_ENABLED

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::_push_console_log(const String &p_message, int p_type) {
	ConsoleLogEntry &entry = console_log.write[console_log_write_pos];
	entry.message = p_message;
	entry.type = p_type;
	entry.timestamp_msec = Time::get_singleton()->get_ticks_msec();

	console_log_write_pos = (console_log_write_pos + 1) % CONSOLE_LOG_CAPACITY;
	if (console_log_count < CONSOLE_LOG_CAPACITY) {
		console_log_count++;
	}
}

const DebugSemanticRegistry::ConsoleLogEntry &DebugSemanticRegistry::get_console_log_entry(int p_index) const {
	// p_index is 0 = oldest visible, (count-1) = newest.
	ERR_FAIL_INDEX_V(p_index, console_log_count, console_log.get(0));
	int start = (console_log_count < CONSOLE_LOG_CAPACITY)
			? 0
			: console_log_write_pos;
	int actual = (start + p_index) % CONSOLE_LOG_CAPACITY;
	return console_log.get(actual);
}

void DebugSemanticRegistry::_event_callback(const String &p_event_name, const Array &p_args) {
	// Write to ring buffer.
	EventLogEntry &entry = event_log.write[event_log_write_pos];
	entry.name = p_event_name;
	entry.args = p_args;
	entry.frame = Engine::get_singleton()->get_process_frames();
	entry.timestamp_msec = Time::get_singleton()->get_ticks_msec();

	event_log_write_pos = (event_log_write_pos + 1) % EVENT_LOG_CAPACITY;
	if (event_log_count < EVENT_LOG_CAPACITY) {
		event_log_count++;
	}

	// Also log to console.
	String args_str;
	if (p_args.size() > 0) {
		args_str = ": " + Variant(p_args).stringify();
	}
	_push_console_log("[EVENT] " + p_event_name + args_str, 0);
}

void DebugSemanticRegistry::_event_fired_0(const String &p_event_name) {
	_event_callback(p_event_name, Array());
}

void DebugSemanticRegistry::_event_fired_1(const Variant &p_a1, const String &p_event_name) {
	Array args;
	args.push_back(p_a1);
	_event_callback(p_event_name, args);
}

void DebugSemanticRegistry::_event_fired_2(const Variant &p_a1, const Variant &p_a2, const String &p_event_name) {
	Array args;
	args.push_back(p_a1);
	args.push_back(p_a2);
	_event_callback(p_event_name, args);
}

void DebugSemanticRegistry::_event_fired_3(const Variant &p_a1, const Variant &p_a2, const Variant &p_a3, const String &p_event_name) {
	Array args;
	args.push_back(p_a1);
	args.push_back(p_a2);
	args.push_back(p_a3);
	_event_callback(p_event_name, args);
}

String DebugSemanticRegistry::_get_object_tag(Object *p_obj, const String &p_tag) const {
	if (!p_tag.is_empty()) {
		return p_tag;
	}

	// Try class_name from script first.
	ScriptInstance *si = p_obj->get_script_instance();
	if (si) {
		Ref<Script> script = si->get_script();
		if (script.is_valid()) {
			StringName global_name = script->get_global_name();
			if (global_name != StringName()) {
				return String(global_name);
			}
		}
	}

	// Fall back to native class name.
	return p_obj->get_class();
}

void DebugSemanticRegistry::_auto_expose_cleanup(ObjectID p_object_id) {
	for (int i = auto_expose_records.size() - 1; i >= 0; i--) {
		if (auto_expose_records[i].object_id == p_object_id) {
			const AutoExposeRecord &record = auto_expose_records[i];

			for (const String &cvar_name : record.cvar_names) {
				cvars.erase(cvar_name);
			}
			for (const String &cmd_name : record.command_names) {
				commands.erase(cmd_name);
			}

			auto_expose_records.remove_at(i);
			_completions_dirty = true;
		}
	}
}

// -----------------------------------------------------------------------------
// Actions
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_action(const String &p_name, const Callable &p_callable,
		const String &p_description, const Dictionary &p_params,
		const String &p_category) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "Action name cannot be empty.");
	ERR_FAIL_COND_MSG(!p_callable.is_valid(), "Action callable must be valid.");

	ActionEntry entry;
	entry.name = p_name;
	entry.callable = p_callable;
	entry.description = p_description;
	entry.params_schema = p_params;
	entry.category = p_category;

	actions.insert(p_name, entry);
	_completions_dirty = true;
}

void DebugSemanticRegistry::unregister_action(const String &p_name) {
	if (actions.erase(p_name)) {
		_completions_dirty = true;
	}
}

Dictionary DebugSemanticRegistry::invoke_action(const String &p_name, const Dictionary &p_params) {
	const ActionEntry *entry = actions.getptr(p_name);
	ERR_FAIL_NULL_V_MSG(entry, Dictionary(), vformat("Action '%s' not found.", p_name));

	Variant result;
	Callable::CallError ce;
	const Variant params_var = p_params;
	const Variant *args[1] = { &params_var };
	entry->callable.callp(args, 1, result, ce);

	if (ce.error != Callable::CallError::CALL_OK) {
		// Try calling with no args (common pattern for simple actions).
		entry->callable.callp(nullptr, 0, result, ce);
	}

	if (result.get_type() == Variant::DICTIONARY) {
		return result;
	}

	Dictionary ret;
	ret["result"] = result;
	return ret;
}

bool DebugSemanticRegistry::has_action(const String &p_name) const {
	return actions.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_action_list() const {
	PackedStringArray list;
	for (const KeyValue<String, ActionEntry> &kv : actions) {
		list.push_back(kv.key);
	}
	return list;
}

// -----------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_query(const String &p_name, const Callable &p_callable,
		const String &p_description, const String &p_category) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "Query name cannot be empty.");
	ERR_FAIL_COND_MSG(!p_callable.is_valid(), "Query callable must be valid.");

	QueryEntry entry;
	entry.name = p_name;
	entry.callable = p_callable;
	entry.description = p_description;
	entry.category = p_category;

	queries.insert(p_name, entry);
	_completions_dirty = true;
}

void DebugSemanticRegistry::unregister_query(const String &p_name) {
	if (queries.erase(p_name)) {
		_completions_dirty = true;
	}
}

Variant DebugSemanticRegistry::evaluate_query(const String &p_name) {
	const QueryEntry *entry = queries.getptr(p_name);
	ERR_FAIL_NULL_V_MSG(entry, Variant(), vformat("Query '%s' not found.", p_name));

	Variant result;
	Callable::CallError ce;
	entry->callable.callp(nullptr, 0, result, ce);

	if (ce.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("Failed to evaluate query '%s': call error %d.", p_name, (int)ce.error));
		return Variant();
	}
	return result;
}

bool DebugSemanticRegistry::has_query(const String &p_name) const {
	return queries.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_query_list() const {
	PackedStringArray list;
	for (const KeyValue<String, QueryEntry> &kv : queries) {
		list.push_back(kv.key);
	}
	return list;
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_event(const String &p_name, const Callable &p_signal_callable,
		const String &p_description, const String &p_category) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "Event name cannot be empty.");

	EventEntry entry;
	entry.name = p_name;
	entry.callable = p_signal_callable;
	entry.description = p_description;
	entry.category = p_category;
	entry.connected = false;

	events.insert(p_name, entry);
	_completions_dirty = true;

	// Connect to the signal. The p_signal_callable wraps a signal reference
	// (e.g., player.died from GDScript). We extract the source object and
	// signal name, then connect our event logger.
	//
	// GDScript usage:
	//   Debug.register_event("player_died", player.died, "Player died")
	if (p_signal_callable.is_valid()) {
		Object *source = p_signal_callable.get_object();
		StringName signal_name = p_signal_callable.get_method();
		if (source && signal_name != StringName() && source->has_signal(signal_name)) {
			// Determine how many args the signal emits so we can unbind them.
			int signal_arg_count = 0;
			List<MethodInfo> signal_list;
			source->get_signal_list(&signal_list);
			for (const MethodInfo &mi : signal_list) {
				if (mi.name == signal_name) {
					signal_arg_count = mi.arguments.size();
					break;
				}
			}

			// Build callable that captures signal args (up to 3) instead of
			// dropping them. .bind(name) appends the event name as the last arg.
			Callable cb;
			switch (signal_arg_count) {
				case 0:
					cb = Callable(this, SNAME("_event_fired_0")).bind(p_name);
					break;
				case 1:
					cb = Callable(this, SNAME("_event_fired_1")).bind(p_name);
					break;
				case 2:
					cb = Callable(this, SNAME("_event_fired_2")).bind(p_name);
					break;
				case 3:
					cb = Callable(this, SNAME("_event_fired_3")).bind(p_name);
					break;
				default:
					// >3 args: capture first 3, drop the rest.
					cb = Callable(this, SNAME("_event_fired_3")).bind(p_name);
					if (signal_arg_count > 3) {
						cb = cb.unbind(signal_arg_count - 3);
					}
					break;
			}
			source->connect(signal_name, cb, Object::CONNECT_REFERENCE_COUNTED);
			events.getptr(p_name)->connected = true;
		}
	}
}

void DebugSemanticRegistry::unregister_event(const String &p_name) {
	EventEntry *entry = events.getptr(p_name);
	if (entry && entry->connected && entry->callable.is_valid()) {
		// Disconnect from the source signal.
		Object *source = entry->callable.get_object();
		StringName signal_name = entry->callable.get_method();
		if (source && signal_name != StringName()) {
			// We can't easily reconstruct the exact Callable we connected,
			// so we rely on CONNECT_REFERENCE_COUNTED to handle cleanup
			// when the registry is destroyed. For explicit unregister, we
			// just mark it disconnected and erase the entry.
		}
	}
	if (events.erase(p_name)) {
		_completions_dirty = true;
	}
}

bool DebugSemanticRegistry::has_event(const String &p_name) const {
	return events.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_event_list() const {
	PackedStringArray list;
	for (const KeyValue<String, EventEntry> &kv : events) {
		list.push_back(kv.key);
	}
	return list;
}

Array DebugSemanticRegistry::get_recent_events(int p_count) const {
	Array result;
	int count = MIN(p_count, event_log_count);
	// Return newest first.
	for (int i = 0; i < count; i++) {
		int idx = (event_log_write_pos - 1 - i + EVENT_LOG_CAPACITY) % EVENT_LOG_CAPACITY;
		const EventLogEntry &entry = event_log.get(idx);

		Dictionary d;
		d["name"] = entry.name;
		d["args"] = entry.args;
		d["frame"] = entry.frame;
		d["timestamp_msec"] = entry.timestamp_msec;
		result.push_back(d);
	}
	return result;
}

// -----------------------------------------------------------------------------
// Interactables
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_interactable(const String &p_name, Object *p_node,
		const String &p_type, const String &p_description,
		const Array &p_valid_actions,
		const String &p_category) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "Interactable name cannot be empty.");
	ERR_FAIL_NULL_MSG(p_node, "Interactable node cannot be null.");

	InteractableEntry entry;
	entry.name = p_name;
	entry.node_id = p_node->get_instance_id();
	entry.type = p_type;
	entry.description = p_description;
	for (int i = 0; i < p_valid_actions.size(); i++) {
		entry.valid_actions.push_back(p_valid_actions[i]);
	}
	entry.category = p_category;

	interactables.insert(p_name, entry);
	_completions_dirty = true;
}

void DebugSemanticRegistry::unregister_interactable(const String &p_name) {
	if (interactables.erase(p_name)) {
		_completions_dirty = true;
	}
}

bool DebugSemanticRegistry::has_interactable(const String &p_name) const {
	return interactables.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_interactable_list() const {
	PackedStringArray list;
	for (const KeyValue<String, InteractableEntry> &kv : interactables) {
		list.push_back(kv.key);
	}
	return list;
}

// -----------------------------------------------------------------------------
// CVars
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_cvar(const String &p_name, const Variant &p_default_value,
		const String &p_description, const Dictionary &p_options) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "CVar name cannot be empty.");

	CVarEntry entry;
	entry.name = p_name;
	entry.value = p_default_value;
	entry.default_value = p_default_value;
	entry.description = p_description;
	entry.type = p_default_value.get_type();

	if (p_options.has("category")) {
		entry.category = p_options["category"];
	}
	if (p_options.has("min")) {
		entry.min_value = p_options["min"];
	}
	if (p_options.has("max")) {
		entry.max_value = p_options["max"];
	}
	if (p_options.has("flags")) {
		entry.flags = (int)p_options["flags"];
	}

	cvars.insert(p_name, entry);
	_completions_dirty = true;
}

void DebugSemanticRegistry::unregister_cvar(const String &p_name) {
	if (cvars.erase(p_name)) {
		_completions_dirty = true;
	}
}

void DebugSemanticRegistry::set_cvar(const String &p_name, const Variant &p_value) {
	CVarEntry *entry = cvars.getptr(p_name);
	ERR_FAIL_NULL_MSG(entry, vformat("CVar '%s' not found.", p_name));
	ERR_FAIL_COND_MSG(entry->flags & CVAR_READONLY, vformat("CVar '%s' is read-only.", p_name));

	// Type coercion.
	Variant coerced = p_value;
	if (p_value.get_type() != entry->type && entry->type != Variant::NIL) {
		Variant::Type target = entry->type;
		if (Variant::can_convert(p_value.get_type(), target)) {
			Callable::CallError ce;
			const Variant *args[1] = { &p_value };
			Variant::construct(target, coerced, args, 1, ce);
			if (ce.error != Callable::CallError::CALL_OK) {
				coerced = p_value; // Fall back to original.
			}
		}
	}

	// Range clamping.
	if (entry->min_value.get_type() != Variant::NIL && entry->max_value.get_type() != Variant::NIL) {
		if (entry->type == Variant::INT) {
			int val = coerced;
			val = CLAMP(val, (int)entry->min_value, (int)entry->max_value);
			coerced = val;
		} else if (entry->type == Variant::FLOAT) {
			double val = coerced;
			val = CLAMP(val, (double)entry->min_value, (double)entry->max_value);
			coerced = val;
		}
	}

	entry->value = coerced;

	// If this is a live-bound property (from auto_expose), write through.
	if (entry->bound_object_id.is_valid()) {
		Object *obj = ObjectDB::get_instance(entry->bound_object_id);
		if (obj) {
			obj->set(entry->bound_property, coerced);
		}
	}
}

Variant DebugSemanticRegistry::get_cvar(const String &p_name) const {
	const CVarEntry *entry = cvars.getptr(p_name);
	if (!entry) {
		return Variant();
	}

	// If live-bound, read from the actual object property.
	if (entry->bound_object_id.is_valid()) {
		Object *obj = ObjectDB::get_instance(entry->bound_object_id);
		if (obj) {
			return obj->get(entry->bound_property);
		}
	}

	return entry->value;
}

bool DebugSemanticRegistry::cvar_bool(const String &p_name, bool p_default) const {
	const CVarEntry *entry = cvars.getptr(p_name);
	if (!entry) {
		return p_default;
	}
	if (entry->bound_object_id.is_valid()) {
		Object *obj = ObjectDB::get_instance(entry->bound_object_id);
		if (obj) {
			return obj->get(entry->bound_property);
		}
	}
	return entry->value;
}

int DebugSemanticRegistry::cvar_int(const String &p_name, int p_default) const {
	const CVarEntry *entry = cvars.getptr(p_name);
	if (!entry) {
		return p_default;
	}
	if (entry->bound_object_id.is_valid()) {
		Object *obj = ObjectDB::get_instance(entry->bound_object_id);
		if (obj) {
			return obj->get(entry->bound_property);
		}
	}
	return entry->value;
}

double DebugSemanticRegistry::cvar_float(const String &p_name, double p_default) const {
	const CVarEntry *entry = cvars.getptr(p_name);
	if (!entry) {
		return p_default;
	}
	if (entry->bound_object_id.is_valid()) {
		Object *obj = ObjectDB::get_instance(entry->bound_object_id);
		if (obj) {
			return obj->get(entry->bound_property);
		}
	}
	return entry->value;
}

String DebugSemanticRegistry::cvar_string(const String &p_name, const String &p_default) const {
	const CVarEntry *entry = cvars.getptr(p_name);
	if (!entry) {
		return p_default;
	}
	if (entry->bound_object_id.is_valid()) {
		Object *obj = ObjectDB::get_instance(entry->bound_object_id);
		if (obj) {
			return obj->get(entry->bound_property);
		}
	}
	return entry->value;
}

bool DebugSemanticRegistry::has_cvar(const String &p_name) const {
	return cvars.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_cvar_list() const {
	PackedStringArray list;
	for (const KeyValue<String, CVarEntry> &kv : cvars) {
		list.push_back(kv.key);
	}
	return list;
}

// -----------------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_command(const String &p_name, const Callable &p_callable,
		const String &p_description, const Dictionary &p_params,
		const String &p_category) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "Command name cannot be empty.");
	ERR_FAIL_COND_MSG(!p_callable.is_valid(), "Command callable must be valid.");

	CommandEntry entry;
	entry.name = p_name;
	entry.callable = p_callable;
	entry.description = p_description;
	entry.params_schema = p_params;
	entry.category = p_category;

	commands.insert(p_name, entry);
	_completions_dirty = true;
}

void DebugSemanticRegistry::unregister_command(const String &p_name) {
	if (commands.erase(p_name)) {
		_completions_dirty = true;
	}
}

void DebugSemanticRegistry::set_command_completion(const String &p_name, const Callable &p_completion_func) {
	CommandEntry *entry = commands.getptr(p_name);
	ERR_FAIL_NULL_MSG(entry, vformat("Command '%s' not found.", p_name));
	entry->completion_func = p_completion_func;
}

String DebugSemanticRegistry::execute_command(const String &p_name, const PackedStringArray &p_args) {
	const CommandEntry *entry = commands.getptr(p_name);
	ERR_FAIL_NULL_V_MSG(entry, "", vformat("Command '%s' not found.", p_name));

	Variant result;
	Callable::CallError ce;
	const Variant args_var = p_args;
	const Variant *args[1] = { &args_var };
	entry->callable.callp(args, 1, result, ce);

	if (ce.error != Callable::CallError::CALL_OK) {
		// Try calling with no args.
		entry->callable.callp(nullptr, 0, result, ce);
	}

	if (ce.error != Callable::CallError::CALL_OK) {
		return vformat("Error calling command '%s': call error %d", p_name, (int)ce.error);
	}

	return result.get_type() == Variant::NIL ? String() : result.stringify();
}

bool DebugSemanticRegistry::has_command(const String &p_name) const {
	return commands.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_command_list() const {
	PackedStringArray list;
	for (const KeyValue<String, CommandEntry> &kv : commands) {
		list.push_back(kv.key);
	}
	return list;
}

// -----------------------------------------------------------------------------
// UI Pages — navigation graph
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::register_ui_page(const String &p_name, Object *p_node,
		const String &p_description, const Dictionary &p_options) {
	ERR_FAIL_COND_MSG(p_name.is_empty(), "UI page name cannot be empty.");
	ERR_FAIL_NULL_MSG(p_node, "UI page node cannot be null.");

	UIPageEntry entry;
	entry.name = p_name;
	entry.node_id = p_node->get_instance_id();
	entry.description = p_description;

	if (p_options.has("parent")) {
		entry.parent_page = p_options["parent"];
	}
	if (p_options.has("children")) {
		Variant children_var = p_options["children"];
		if (children_var.get_type() == Variant::PACKED_STRING_ARRAY) {
			entry.child_pages = children_var;
		} else if (children_var.get_type() == Variant::ARRAY) {
			Array arr = children_var;
			for (int i = 0; i < arr.size(); i++) {
				entry.child_pages.push_back(arr[i]);
			}
		}
	}
	if (p_options.has("back")) {
		entry.back_action = p_options["back"];
	}
	if (p_options.has("enter_actions")) {
		Variant actions_var = p_options["enter_actions"];
		if (actions_var.get_type() == Variant::PACKED_STRING_ARRAY) {
			entry.enter_actions = actions_var;
		} else if (actions_var.get_type() == Variant::ARRAY) {
			Array arr = actions_var;
			for (int i = 0; i < arr.size(); i++) {
				entry.enter_actions.push_back(arr[i]);
			}
		}
	}
	if (p_options.has("metadata")) {
		entry.metadata = p_options["metadata"];
	}

	// If parent is specified and exists, add this as a child of the parent.
	if (!entry.parent_page.is_empty()) {
		UIPageEntry *parent = ui_pages.getptr(entry.parent_page);
		if (parent) {
			// Add to parent's children if not already there.
			bool found = false;
			for (const String &child : parent->child_pages) {
				if (child == p_name) {
					found = true;
					break;
				}
			}
			if (!found) {
				parent->child_pages.push_back(p_name);
			}
		}
	}

	ui_pages.insert(p_name, entry);
	_completions_dirty = true;

	// Auto-cleanup when the node exits the tree.
	Node *node = Object::cast_to<Node>(p_node);
	if (node) {
		String page_name = p_name;
		node->connect(
				SNAME("tree_exiting"),
				callable_mp(this, &DebugSemanticRegistry::unregister_ui_page).bind(page_name),
				Object::CONNECT_ONE_SHOT);
	}
}

void DebugSemanticRegistry::unregister_ui_page(const String &p_name) {
	UIPageEntry *entry = ui_pages.getptr(p_name);
	if (!entry) {
		return;
	}

	// Remove from parent's children list.
	if (!entry->parent_page.is_empty()) {
		UIPageEntry *parent = ui_pages.getptr(entry->parent_page);
		if (parent) {
			for (int i = 0; i < parent->child_pages.size(); i++) {
				if (parent->child_pages[i] == p_name) {
					parent->child_pages.remove_at(i);
					break;
				}
			}
		}
	}

	if (ui_pages.erase(p_name)) {
		_completions_dirty = true;
	}
}

bool DebugSemanticRegistry::has_ui_page(const String &p_name) const {
	return ui_pages.has(p_name);
}

PackedStringArray DebugSemanticRegistry::get_ui_page_list() const {
	PackedStringArray list;
	for (const KeyValue<String, UIPageEntry> &kv : ui_pages) {
		list.push_back(kv.key);
	}
	return list;
}

String DebugSemanticRegistry::get_active_ui_page() const {
	// Walk all pages and find the one whose node is visible. For pages at the
	// same depth level, prefer the one that is also visible_in_tree().
	// If multiple root-level pages are visible, return the first found.
	String active;
	for (const KeyValue<String, UIPageEntry> &kv : ui_pages) {
		Object *obj = ObjectDB::get_instance(kv.value.node_id);
		if (!obj) {
			continue;
		}
		Node *node = Object::cast_to<Node>(obj);
		if (!node) {
			continue;
		}
		// Check CanvasItem visibility (most UI pages are Control descendants).
		bool visible = false;
		if (node->has_method("is_visible_in_tree")) {
			visible = node->call("is_visible_in_tree");
		}
		if (visible) {
			// Prefer deeper pages (more specific) over root pages.
			if (active.is_empty() || !kv.value.parent_page.is_empty()) {
				active = kv.key;
			}
		}
	}
	return active;
}

Dictionary DebugSemanticRegistry::get_ui_page_info(const String &p_name) const {
	const UIPageEntry *entry = ui_pages.getptr(p_name);
	if (!entry) {
		return Dictionary();
	}

	Dictionary info;
	info["name"] = entry->name;
	info["description"] = entry->description;
	info["parent"] = entry->parent_page;
	info["children"] = entry->child_pages;
	info["back"] = entry->back_action;
	info["enter_actions"] = entry->enter_actions;

	// Include live visibility status.
	Object *obj = ObjectDB::get_instance(entry->node_id);
	if (obj) {
		Node *node = Object::cast_to<Node>(obj);
		if (node) {
			info["node_path"] = String(node->get_path());
			if (node->has_method("is_visible_in_tree")) {
				info["visible"] = (bool)node->call("is_visible_in_tree");
			}
		}
	} else {
		info["node_path"] = "";
		info["visible"] = false;
	}

	if (!entry->metadata.is_empty()) {
		info["metadata"] = entry->metadata;
	}

	return info;
}

Dictionary DebugSemanticRegistry::get_ui_navigation_graph() const {
	Dictionary graph;

	for (const KeyValue<String, UIPageEntry> &kv : ui_pages) {
		Dictionary page_info;
		page_info["description"] = kv.value.description;
		page_info["parent"] = kv.value.parent_page;
		page_info["children"] = kv.value.child_pages;
		page_info["back"] = kv.value.back_action;
		page_info["enter_actions"] = kv.value.enter_actions;

		Object *obj = ObjectDB::get_instance(kv.value.node_id);
		if (obj) {
			Node *node = Object::cast_to<Node>(obj);
			if (node) {
				page_info["node_path"] = String(node->get_path());
				if (node->has_method("is_visible_in_tree")) {
					page_info["visible"] = (bool)node->call("is_visible_in_tree");
				}
			}
		}

		graph[kv.key] = page_info;
	}

	return graph;
}

// -----------------------------------------------------------------------------
// Auto-expose
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::auto_expose(Object *p_obj, const String &p_tag) {
	ERR_FAIL_NULL_MSG(p_obj, "Cannot auto_expose null object.");

	String tag = _get_object_tag(p_obj, p_tag);

	// Check for tag collision — auto_expose is designed for singletons and
	// one-of-a-kind nodes. If another object already owns this tag, warn
	// and clean up the stale record (the old object may have been freed
	// without tree_exiting, or this is a genuine collision).
	for (int i = auto_expose_records.size() - 1; i >= 0; i--) {
		if (auto_expose_records[i].tag == tag) {
			ObjectID old_id = auto_expose_records[i].object_id;
			Object *old_obj = ObjectDB::get_instance(old_id);
			if (old_obj && old_obj != p_obj) {
				WARN_PRINT(vformat(
						"auto_expose: tag '%s' already in use by %s (id:%d). "
						"Replacing. Use an explicit tag for multiple instances: "
						"Debug.auto_expose(self, \"enemy_\" + str(get_index()))",
						tag, old_obj->get_class(), (int64_t)old_id));
			}
			// Clean up the old record regardless.
			_auto_expose_cleanup(old_id);
			break;
		}
	}

	AutoExposeRecord record;
	record.object_id = p_obj->get_instance_id();
	record.tag = tag;

	// --- Scan @export properties ---
	List<PropertyInfo> prop_list;
	p_obj->get_property_list(&prop_list);

	for (const PropertyInfo &pi : prop_list) {
		if (!(pi.usage & PROPERTY_USAGE_EDITOR)) {
			continue; // Only @export properties.
		}
		if (pi.usage & PROPERTY_USAGE_CATEGORY || pi.usage & PROPERTY_USAGE_GROUP || pi.usage & PROPERTY_USAGE_SUBGROUP) {
			continue; // Skip category/group headers.
		}

		// Filter to types we can reasonably expose.
		switch (pi.type) {
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::STRING:
			case Variant::VECTOR2:
			case Variant::VECTOR3:
			case Variant::COLOR:
				break; // Supported.
			default:
				continue; // Skip complex types.
		}

		String cvar_name = tag + "." + pi.name;

		CVarEntry cvar;
		cvar.name = cvar_name;
		cvar.value = p_obj->get(pi.name);
		cvar.default_value = cvar.value;
		cvar.description = vformat("@export %s on %s", pi.name, tag);
		cvar.category = tag;
		cvar.type = pi.type;
		cvar.bound_object_id = p_obj->get_instance_id();
		cvar.bound_property = pi.name;

		// Extract range hints.
		if (!pi.hint_string.is_empty() && (pi.hint == PROPERTY_HINT_RANGE)) {
			PackedStringArray parts = pi.hint_string.split(",");
			if (parts.size() >= 2) {
				cvar.min_value = parts[0].to_float();
				cvar.max_value = parts[1].to_float();
			}
		}

		cvars.insert(cvar_name, cvar);
		record.cvar_names.push_back(cvar_name);
	}

	// --- Scan debug_* methods ---
	List<MethodInfo> method_list;
	p_obj->get_method_list(&method_list);

	for (const MethodInfo &mi : method_list) {
		if (!mi.name.begins_with("debug_")) {
			continue;
		}

		String cmd_name = tag + "." + mi.name.substr(6); // Strip "debug_" prefix.

		CommandEntry cmd;
		cmd.name = cmd_name;
		cmd.callable = Callable(p_obj, mi.name);
		cmd.category = tag;

		// Build description from method info.
		String param_desc;
		Dictionary params_schema;
		for (int i = 0; i < mi.arguments.size(); i++) {
			const PropertyInfo &arg = mi.arguments[i];
			if (!param_desc.is_empty()) {
				param_desc += ", ";
			}
			param_desc += arg.name + ":" + Variant::get_type_name(arg.type);
			params_schema[arg.name] = Variant::get_type_name(arg.type);
		}
		cmd.description = vformat("debug_%s(%s) on %s", mi.name.substr(6), param_desc, tag);
		cmd.params_schema = params_schema;

		commands.insert(cmd_name, cmd);
		record.command_names.push_back(cmd_name);
	}

	auto_expose_records.push_back(record);
	_completions_dirty = true;

	// Connect to object destruction for auto-cleanup.
	// Use tree_exiting for Node-derived objects (most common case).
	ObjectID obj_id = p_obj->get_instance_id();
	Node *node = Object::cast_to<Node>(p_obj);
	if (node) {
		node->connect(
				SNAME("tree_exiting"),
				callable_mp(this, &DebugSemanticRegistry::_auto_expose_cleanup).bind(obj_id),
				Object::CONNECT_ONE_SHOT);
	}
	// For non-Node objects, the caller must manually unregister or let
	// cleanup happen when the registry is destroyed.
}

// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------

void DebugSemanticRegistry::log(const String &p_message) {
	_push_console_log(p_message, 0);
	print_line("[Debug] " + p_message);
}

void DebugSemanticRegistry::log_warning(const String &p_message) {
	_push_console_log(p_message, 1);
	WARN_PRINT("[Debug] " + p_message);
}

void DebugSemanticRegistry::log_error(const String &p_message) {
	_push_console_log(p_message, 2);
	ERR_PRINT("[Debug] " + p_message);
}

// -----------------------------------------------------------------------------
// Manifest / introspection
// -----------------------------------------------------------------------------

Dictionary DebugSemanticRegistry::get_manifest() const {
	Dictionary manifest;

	// Actions.
	Dictionary actions_dict;
	for (const KeyValue<String, ActionEntry> &kv : actions) {
		Dictionary entry;
		entry["description"] = kv.value.description;
		entry["params"] = kv.value.params_schema;
		entry["category"] = kv.value.category;
		actions_dict[kv.key] = entry;
	}
	manifest["actions"] = actions_dict;

	// Queries.
	Dictionary queries_dict;
	for (const KeyValue<String, QueryEntry> &kv : queries) {
		Dictionary entry;
		entry["description"] = kv.value.description;
		entry["category"] = kv.value.category;
		queries_dict[kv.key] = entry;
	}
	manifest["queries"] = queries_dict;

	// Events.
	Dictionary events_dict;
	for (const KeyValue<String, EventEntry> &kv : events) {
		Dictionary entry;
		entry["description"] = kv.value.description;
		entry["category"] = kv.value.category;
		events_dict[kv.key] = entry;
	}
	manifest["events"] = events_dict;

	// Interactables.
	Dictionary interactables_dict;
	for (const KeyValue<String, InteractableEntry> &kv : interactables) {
		Dictionary entry;
		entry["type"] = kv.value.type;
		entry["description"] = kv.value.description;
		entry["valid_actions"] = kv.value.valid_actions;
		entry["category"] = kv.value.category;
		interactables_dict[kv.key] = entry;
	}
	manifest["interactables"] = interactables_dict;

	// CVars.
	Dictionary cvars_dict;
	for (const KeyValue<String, CVarEntry> &kv : cvars) {
		Dictionary entry;
		entry["value"] = get_cvar(kv.key); // Use getter for live-bound values.
		entry["default"] = kv.value.default_value;
		entry["description"] = kv.value.description;
		entry["type"] = Variant::get_type_name(kv.value.type);
		entry["category"] = kv.value.category;
		if (kv.value.min_value.get_type() != Variant::NIL) {
			entry["min"] = kv.value.min_value;
		}
		if (kv.value.max_value.get_type() != Variant::NIL) {
			entry["max"] = kv.value.max_value;
		}
		cvars_dict[kv.key] = entry;
	}
	manifest["cvars"] = cvars_dict;

	// Commands.
	Dictionary commands_dict;
	for (const KeyValue<String, CommandEntry> &kv : commands) {
		Dictionary entry;
		entry["description"] = kv.value.description;
		entry["params"] = kv.value.params_schema;
		entry["category"] = kv.value.category;
		commands_dict[kv.key] = entry;
	}
	manifest["commands"] = commands_dict;

	// UI Pages.
	if (!ui_pages.is_empty()) {
		manifest["ui_pages"] = get_ui_navigation_graph();
		manifest["active_ui_page"] = get_active_ui_page();
	}

	return manifest;
}

#endif // DEBUG_ENABLED
