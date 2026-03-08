extends Control

@onready var label: Label = $Label

func _ready() -> void:
	var data := load("res://data.tres")
	var value: String = data.get("value") if data else "FAILED TO LOAD"
	label.text = "Resource Cache Test B\nLoaded data: %s" % value
	print("Resource Cache Test B - Loaded: %s" % value)
