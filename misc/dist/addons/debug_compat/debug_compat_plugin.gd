@tool
extends EditorPlugin
## Editor plugin that registers the Debug compatibility autoload.
##
## The autoload is ALWAYS registered as "Debug". At runtime, the autoload
## script detects whether the native C++ singleton exists:
##   - Custom fork:  proxies every call to the native singleton (transparent).
##   - Vanilla 4.6:  returns safe no-op defaults (stub mode).
##
## This means the same project.godot works on both engines without
## manual intervention — no _enable_plugin timing issues.


func _enable_plugin() -> void:
	add_autoload_singleton("Debug", "res://addons/debug_compat/debug_stub.gd")


func _disable_plugin() -> void:
	remove_autoload_singleton("Debug")
