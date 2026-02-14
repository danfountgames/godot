/**************************************************************************/
/*  debug_semantic_registry.h                                             */
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

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class DebugSemanticRegistry : public Object {
	GDCLASS(DebugSemanticRegistry, Object);

	static DebugSemanticRegistry *singleton;

protected:
	static void _bind_methods();

public:
	static DebugSemanticRegistry *get_singleton() { return singleton; }

	// =========================================================================
	// CVar flags
	// =========================================================================
	enum CVarFlags {
		CVAR_NONE = 0,
		CVAR_ARCHIVE = 1 << 0, // Save to config file.
		CVAR_READONLY = 1 << 1, // Cannot be set from console.
		CVAR_CHEAT = 1 << 2, // Only active when cheats enabled.
		CVAR_HIDDEN = 1 << 3, // Hidden from autocomplete.
	};

#ifdef DEBUG_ENABLED

	// =========================================================================
	// Data structures
	// =========================================================================

	struct ActionEntry {
		String name;
		Callable callable; // fn(Dictionary params) -> Dictionary result
		String description;
		Dictionary params_schema;
		String category;
	};

	struct QueryEntry {
		String name;
		Callable callable; // fn() -> Variant
		String description;
		String category;
	};

	struct EventEntry {
		String name;
		Callable callable; // The signal's callable for connecting.
		String description;
		String category;
		bool connected = false;
	};

	struct EventLogEntry {
		String name;
		Array args;
		int64_t frame = 0;
		uint64_t timestamp_msec = 0;
	};

	struct InteractableEntry {
		String name;
		ObjectID node_id;
		String type; // "ui", "world_2d", "world_3d", "logic"
		String description;
		PackedStringArray valid_actions;
		String category;
	};

	struct CVarEntry {
		String name;
		Variant value;
		Variant default_value;
		String description;
		String category;
		Variant::Type type = Variant::NIL;
		Variant min_value;
		Variant max_value;
		int flags = CVAR_NONE;
		// Live property binding (from auto_expose).
		ObjectID bound_object_id;
		StringName bound_property;
	};

	struct CommandEntry {
		String name;
		Callable callable; // fn(PackedStringArray args) -> String result
		String description;
		Dictionary params_schema;
		String category;
		Callable completion_func; // Optional: fn(String partial) -> PackedStringArray
	};

	// UI page / screen semantics — describes the game's page navigation graph.
	struct UIPageEntry {
		String name; // Unique identifier (e.g., "main_menu", "settings.audio").
		ObjectID node_id; // The Control node representing this page.
		String description; // Human-readable purpose (e.g., "Audio settings page").
		String parent_page; // Parent page name, empty for root pages.
		PackedStringArray child_pages; // Names of sub-pages reachable from here.
		String back_action; // How to go back (node path or action name, "" = none).
		PackedStringArray enter_actions; // Actions/buttons that navigate TO this page.
		Dictionary metadata; // Game-specific extra data (tags, state, etc.).
	};

	// Track what an auto_expose() call registered so we can clean up.
	struct AutoExposeRecord {
		ObjectID object_id;
		String tag;
		Vector<String> cvar_names;
		Vector<String> command_names;
	};

private:
	HashMap<String, ActionEntry> actions;
	HashMap<String, QueryEntry> queries;
	HashMap<String, EventEntry> events;
	HashMap<String, InteractableEntry> interactables;
	HashMap<String, CVarEntry> cvars;
	HashMap<String, CommandEntry> commands;
	HashMap<String, UIPageEntry> ui_pages;

	// Event log ring buffer.
	static const int EVENT_LOG_CAPACITY = 200;
	Vector<EventLogEntry> event_log;
	int event_log_write_pos = 0;
	int event_log_count = 0;

	// Console output log ring buffer.
	struct ConsoleLogEntry {
		String message;
		int type = 0; // 0=info, 1=warning, 2=error
		uint64_t timestamp_msec = 0;
	};
	static const int CONSOLE_LOG_CAPACITY = 1000;
	Vector<ConsoleLogEntry> console_log;
	int console_log_write_pos = 0;
	int console_log_count = 0;

	// Auto-expose tracking.
	Vector<AutoExposeRecord> auto_expose_records;

	// Dirty flag for autocomplete rebuild.
	bool _completions_dirty = true;

	// Internal helpers.
	void _event_callback(const String &p_event_name, const Array &p_args);
	void _event_fired_0(const String &p_event_name); // No-arg event callback for signal connection.
	void _auto_expose_cleanup(ObjectID p_object_id);
	String _get_object_tag(Object *p_obj, const String &p_tag) const;
	void _push_console_log(const String &p_message, int p_type);

public:
	// =========================================================================
	// Actions
	// =========================================================================
	void register_action(const String &p_name, const Callable &p_callable,
			const String &p_description = "", const Dictionary &p_params = Dictionary(),
			const String &p_category = "");
	void unregister_action(const String &p_name);
	Dictionary invoke_action(const String &p_name, const Dictionary &p_params = Dictionary());
	bool has_action(const String &p_name) const;
	PackedStringArray get_action_list() const;

	// =========================================================================
	// Queries
	// =========================================================================
	void register_query(const String &p_name, const Callable &p_callable,
			const String &p_description = "", const String &p_category = "");
	void unregister_query(const String &p_name);
	Variant evaluate_query(const String &p_name);
	bool has_query(const String &p_name) const;
	PackedStringArray get_query_list() const;

	// =========================================================================
	// Events
	// =========================================================================
	void register_event(const String &p_name, const Callable &p_signal_callable,
			const String &p_description = "", const String &p_category = "");
	void unregister_event(const String &p_name);
	bool has_event(const String &p_name) const;
	PackedStringArray get_event_list() const;
	Array get_recent_events(int p_count = 20) const;

	// =========================================================================
	// Interactables
	// =========================================================================
	void register_interactable(const String &p_name, Object *p_node,
			const String &p_type = "logic", const String &p_description = "",
			const Array &p_valid_actions = Array(),
			const String &p_category = "");
	void unregister_interactable(const String &p_name);
	bool has_interactable(const String &p_name) const;
	PackedStringArray get_interactable_list() const;

	// =========================================================================
	// CVars
	// =========================================================================
	void register_cvar(const String &p_name, const Variant &p_default_value,
			const String &p_description = "", const Dictionary &p_options = Dictionary());
	void unregister_cvar(const String &p_name);
	void set_cvar(const String &p_name, const Variant &p_value);
	Variant get_cvar(const String &p_name) const;
	bool cvar_bool(const String &p_name, bool p_default = false) const;
	int cvar_int(const String &p_name, int p_default = 0) const;
	double cvar_float(const String &p_name, double p_default = 0.0) const;
	String cvar_string(const String &p_name, const String &p_default = "") const;
	bool has_cvar(const String &p_name) const;
	PackedStringArray get_cvar_list() const;

	// =========================================================================
	// Commands
	// =========================================================================
	void register_command(const String &p_name, const Callable &p_callable,
			const String &p_description = "", const Dictionary &p_params = Dictionary(),
			const String &p_category = "");
	void unregister_command(const String &p_name);
	void set_command_completion(const String &p_name, const Callable &p_completion_func);
	String execute_command(const String &p_name, const PackedStringArray &p_args);
	bool has_command(const String &p_name) const;
	PackedStringArray get_command_list() const;

	// =========================================================================
	// UI Pages — navigation graph
	// =========================================================================
	void register_ui_page(const String &p_name, Object *p_node,
			const String &p_description = "", const Dictionary &p_options = Dictionary());
	void unregister_ui_page(const String &p_name);
	bool has_ui_page(const String &p_name) const;
	PackedStringArray get_ui_page_list() const;
	String get_active_ui_page() const; // Returns name of currently visible root-level page.
	Dictionary get_ui_page_info(const String &p_name) const;
	Dictionary get_ui_navigation_graph() const; // Full graph for MCP/console.

	// =========================================================================
	// Auto-expose
	// =========================================================================
	void auto_expose(Object *p_obj, const String &p_tag = "");

	// =========================================================================
	// Logging
	// =========================================================================
	void log(const String &p_message);
	void log_warning(const String &p_message);
	void log_error(const String &p_message);

	// =========================================================================
	// Manifest / introspection (for console and MCP)
	// =========================================================================
	Dictionary get_manifest() const;
	bool is_completions_dirty() const { return _completions_dirty; }
	void mark_completions_clean() { _completions_dirty = false; }

	// Accessors for console/MCP to iterate registries.
	const HashMap<String, ActionEntry> &get_actions() const { return actions; }
	const HashMap<String, QueryEntry> &get_queries() const { return queries; }
	const HashMap<String, EventEntry> &get_events() const { return events; }
	const HashMap<String, InteractableEntry> &get_interactables() const { return interactables; }
	const HashMap<String, CVarEntry> &get_cvars() const { return cvars; }
	const HashMap<String, CommandEntry> &get_commands() const { return commands; }
	const HashMap<String, UIPageEntry> &get_ui_pages() const { return ui_pages; }

	// Console log access.
	int get_console_log_count() const { return console_log_count; }
	const ConsoleLogEntry &get_console_log_entry(int p_index) const;

#else // !DEBUG_ENABLED — Release stubs

public:
	void register_action(const String &, const Callable &,
			const String & = "", const Dictionary & = Dictionary(),
			const String & = "") {}
	void unregister_action(const String &) {}
	Dictionary invoke_action(const String &, const Dictionary & = Dictionary()) { return Dictionary(); }
	bool has_action(const String &) const { return false; }
	PackedStringArray get_action_list() const { return PackedStringArray(); }

	void register_query(const String &, const Callable &,
			const String & = "", const String & = "") {}
	void unregister_query(const String &) {}
	Variant evaluate_query(const String &) { return Variant(); }
	bool has_query(const String &) const { return false; }
	PackedStringArray get_query_list() const { return PackedStringArray(); }

	void register_event(const String &, const Callable &,
			const String & = "", const String & = "") {}
	void unregister_event(const String &) {}
	bool has_event(const String &) const { return false; }
	PackedStringArray get_event_list() const { return PackedStringArray(); }
	Array get_recent_events(int = 20) const { return Array(); }

	void register_interactable(const String &, Object *,
			const String & = "logic", const String & = "",
			const Array & = Array(),
			const String & = "") {}
	void unregister_interactable(const String &) {}
	bool has_interactable(const String &) const { return false; }
	PackedStringArray get_interactable_list() const { return PackedStringArray(); }

	void register_cvar(const String &, const Variant &,
			const String & = "", const Dictionary & = Dictionary()) {}
	void unregister_cvar(const String &) {}
	void set_cvar(const String &, const Variant &) {}
	Variant get_cvar(const String &) const { return Variant(); }
	bool cvar_bool(const String &, bool p_default = false) const { return p_default; }
	int cvar_int(const String &, int p_default = 0) const { return p_default; }
	double cvar_float(const String &, double p_default = 0.0) const { return p_default; }
	String cvar_string(const String &, const String &p_default = "") const { return p_default; }
	bool has_cvar(const String &) const { return false; }
	PackedStringArray get_cvar_list() const { return PackedStringArray(); }

	void register_command(const String &, const Callable &,
			const String & = "", const Dictionary & = Dictionary(),
			const String & = "") {}
	void unregister_command(const String &) {}
	void set_command_completion(const String &, const Callable &) {}
	String execute_command(const String &, const PackedStringArray &) { return String(); }
	bool has_command(const String &) const { return false; }
	PackedStringArray get_command_list() const { return PackedStringArray(); }

	void register_ui_page(const String &, Object *,
			const String & = "", const Dictionary & = Dictionary()) {}
	void unregister_ui_page(const String &) {}
	bool has_ui_page(const String &) const { return false; }
	PackedStringArray get_ui_page_list() const { return PackedStringArray(); }
	String get_active_ui_page() const { return String(); }
	Dictionary get_ui_page_info(const String &) const { return Dictionary(); }
	Dictionary get_ui_navigation_graph() const { return Dictionary(); }

	void auto_expose(Object *, const String & = "") {}

	void log(const String &) {}
	void log_warning(const String &) {}
	void log_error(const String &) {}

	Dictionary get_manifest() const { return Dictionary(); }

#endif // DEBUG_ENABLED

	DebugSemanticRegistry();
	~DebugSemanticRegistry();
};
