extends Node

var health: int = 100

func broken_syntax( -> void:  # Line 5: syntax error (missing closing paren)
	pass

func _ready() -> void:
	pass

func bad_type() -> void:
	var x: int = "not an int"  # Line 12: type error
