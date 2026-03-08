extends Control

@onready var label: Label = $Label

func _ready() -> void:
	print("Minimal 2D project loaded")
	label.text = "Minimal 2D - Project Mounted Successfully\nProject: %s" % ProjectSettings.get_setting("application/config/name")
