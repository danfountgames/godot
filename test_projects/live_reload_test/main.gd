extends Control

@onready var label: Label = $Label

# This value will be modified by the sync agent to test live reload.
const RELOAD_VERSION := 1

func _ready() -> void:
	print("Live Reload Test loaded - version: %d" % RELOAD_VERSION)
	label.text = "Live Reload Test\nVersion: %d\nProject: %s" % [
		RELOAD_VERSION,
		ProjectSettings.get_setting("application/config/name")
	]
