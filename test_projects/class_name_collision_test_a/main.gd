extends Control

@onready var label: Label = $Label

func _ready() -> void:
	var enemy := Enemy.new()
	var behavior := enemy.get_behavior()
	label.text = "Class Name Collision Test A\nEnemy behavior: %s" % behavior
	print("Project A - Enemy behavior: %s" % behavior)
