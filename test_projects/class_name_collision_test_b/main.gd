extends Control

@onready var label: Label = $Label

func _ready() -> void:
	var enemy := Enemy.new()
	var behavior := enemy.get_behavior()
	label.text = "Class Name Collision Test B\nEnemy behavior: %s" % behavior
	print("Project B - Enemy behavior: %s" % behavior)
