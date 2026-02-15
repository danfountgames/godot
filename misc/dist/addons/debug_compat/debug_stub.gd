extends Node
## GDScript compatibility layer for the DebugSemanticRegistry singleton.
##
## Registered as the "Debug" autoload on ALL engine builds. At runtime:
##   - Custom fork (native singleton exists): every call proxies to the
##     real C++ DebugSemanticRegistry via _native. Zero-cost for registration
##     calls; negligible overhead for hot-path cvar reads.
##   - Vanilla Godot 4.6 (no native singleton): every call returns a safe
##     default so game code runs without modification.
##
## This proxy-or-stub approach avoids the timing pitfalls of _enable_plugin
## (which only runs once) and ensures the same project.godot works on both
## engine builds without any manual steps.

# ===========================================================================
# Native singleton reference — set once in _enter_tree().
# Typed as Object to avoid parse errors on vanilla Godot where the
# DebugSemanticRegistry class does not exist.
# ===========================================================================
var _native: Object = null

# ===========================================================================
# CVar flag constants — match DebugSemanticRegistry::CVarFlags exactly.
# ===========================================================================
const CVAR_NONE: int = 0
const CVAR_ARCHIVE: int = 1   # 1 << 0
const CVAR_READONLY: int = 2  # 1 << 1
const CVAR_CHEAT: int = 4     # 1 << 2
const CVAR_HIDDEN: int = 8    # 1 << 3


func _enter_tree() -> void:
	if Engine.has_singleton("Debug"):
		_native = Engine.get_singleton("Debug")


# ===========================================================================
# Actions
# ===========================================================================

func register_action(name: String, callable: Callable, description: String = "", params: Dictionary = {}, category: String = "") -> void:
	if _native:
		_native.register_action(name, callable, description, params, category)

func unregister_action(name: String) -> void:
	if _native:
		_native.unregister_action(name)

func invoke_action(name: String, params: Dictionary = {}) -> Dictionary:
	if _native:
		return _native.invoke_action(name, params)
	return {}

func has_action(name: String) -> bool:
	if _native:
		return _native.has_action(name)
	return false

func get_action_list() -> PackedStringArray:
	if _native:
		return _native.get_action_list()
	return PackedStringArray()


# ===========================================================================
# Queries
# ===========================================================================

func register_query(name: String, callable: Callable, description: String = "", category: String = "") -> void:
	if _native:
		_native.register_query(name, callable, description, category)

func unregister_query(name: String) -> void:
	if _native:
		_native.unregister_query(name)

func evaluate_query(name: String) -> Variant:
	if _native:
		return _native.evaluate_query(name)
	return null

func has_query(name: String) -> bool:
	if _native:
		return _native.has_query(name)
	return false

func get_query_list() -> PackedStringArray:
	if _native:
		return _native.get_query_list()
	return PackedStringArray()


# ===========================================================================
# Events
# ===========================================================================

func register_event(name: String, signal_callable: Callable, description: String = "", category: String = "") -> void:
	if _native:
		_native.register_event(name, signal_callable, description, category)

func unregister_event(name: String) -> void:
	if _native:
		_native.unregister_event(name)

func has_event(name: String) -> bool:
	if _native:
		return _native.has_event(name)
	return false

func get_event_list() -> PackedStringArray:
	if _native:
		return _native.get_event_list()
	return PackedStringArray()

func get_recent_events(count: int = 20) -> Array:
	if _native:
		return _native.get_recent_events(count)
	return []


# ===========================================================================
# Interactables
# ===========================================================================

func register_interactable(name: String, node: Object, type: String = "logic", description: String = "", valid_actions: Array = [], category: String = "") -> void:
	if _native:
		_native.register_interactable(name, node, type, description, valid_actions, category)

func unregister_interactable(name: String) -> void:
	if _native:
		_native.unregister_interactable(name)

func has_interactable(name: String) -> bool:
	if _native:
		return _native.has_interactable(name)
	return false

func get_interactable_list() -> PackedStringArray:
	if _native:
		return _native.get_interactable_list()
	return PackedStringArray()


# ===========================================================================
# CVars
# ===========================================================================

func register_cvar(name: String, default_value: Variant, description: String = "", options: Dictionary = {}) -> void:
	if _native:
		_native.register_cvar(name, default_value, description, options)

func unregister_cvar(name: String) -> void:
	if _native:
		_native.unregister_cvar(name)

func set_cvar(name: String, value: Variant) -> void:
	if _native:
		_native.set_cvar(name, value)

func get_cvar(name: String) -> Variant:
	if _native:
		return _native.get_cvar(name)
	return null

func cvar_bool(name: String, default: bool = false) -> bool:
	if _native:
		return _native.cvar_bool(name, default)
	return default

func cvar_int(name: String, default: int = 0) -> int:
	if _native:
		return _native.cvar_int(name, default)
	return default

func cvar_float(name: String, default: float = 0.0) -> float:
	if _native:
		return _native.cvar_float(name, default)
	return default

func cvar_string(name: String, default: String = "") -> String:
	if _native:
		return _native.cvar_string(name, default)
	return default

func has_cvar(name: String) -> bool:
	if _native:
		return _native.has_cvar(name)
	return false

func get_cvar_list() -> PackedStringArray:
	if _native:
		return _native.get_cvar_list()
	return PackedStringArray()


# ===========================================================================
# Commands
# ===========================================================================

func register_command(name: String, callable: Callable, description: String = "", params: Dictionary = {}, category: String = "") -> void:
	if _native:
		_native.register_command(name, callable, description, params, category)

func unregister_command(name: String) -> void:
	if _native:
		_native.unregister_command(name)

func set_command_completion(name: String, completion_func: Callable) -> void:
	if _native:
		_native.set_command_completion(name, completion_func)

func execute_command(name: String, args: PackedStringArray) -> String:
	if _native:
		return _native.execute_command(name, args)
	return ""

func has_command(name: String) -> bool:
	if _native:
		return _native.has_command(name)
	return false

func get_command_list() -> PackedStringArray:
	if _native:
		return _native.get_command_list()
	return PackedStringArray()


# ===========================================================================
# UI Pages
# ===========================================================================

func register_ui_page(name: String, node: Object, description: String = "", options: Dictionary = {}) -> void:
	if _native:
		_native.register_ui_page(name, node, description, options)

func unregister_ui_page(name: String) -> void:
	if _native:
		_native.unregister_ui_page(name)

func has_ui_page(name: String) -> bool:
	if _native:
		return _native.has_ui_page(name)
	return false

func get_ui_page_list() -> PackedStringArray:
	if _native:
		return _native.get_ui_page_list()
	return PackedStringArray()

func get_active_ui_page() -> String:
	if _native:
		return _native.get_active_ui_page()
	return ""

func get_ui_page_info(name: String) -> Dictionary:
	if _native:
		return _native.get_ui_page_info(name)
	return {}

func get_ui_navigation_graph() -> Dictionary:
	if _native:
		return _native.get_ui_navigation_graph()
	return {}


# ===========================================================================
# Auto-expose
# ===========================================================================

func auto_expose(object: Object, tag: String = "") -> void:
	if _native:
		_native.auto_expose(object, tag)


# ===========================================================================
# Logging — proxy to native, or fall back to Godot's built-in print.
# ===========================================================================

func log(message: String) -> void:
	if _native:
		_native.log(message)
	else:
		print("[Debug] ", message)

func log_warning(message: String) -> void:
	if _native:
		_native.log_warning(message)
	else:
		push_warning("[Debug] " + message)

func log_error(message: String) -> void:
	if _native:
		_native.log_error(message)
	else:
		push_error("[Debug] " + message)


# ===========================================================================
# Manifest
# ===========================================================================

func get_manifest() -> Dictionary:
	if _native:
		return _native.get_manifest()
	return {
		"actions": {},
		"queries": {},
		"events": {},
		"interactables": {},
		"cvars": {},
		"commands": {},
		"ui_pages": {},
		"active_ui_page": "",
	}
