extends Control

func _ready() -> void:
	var button = $Panel/StartButton
	button.pressed.connect(_on_start_pressed)

func _on_start_pressed() -> void:
	print("BUTTON_CLICKED")
