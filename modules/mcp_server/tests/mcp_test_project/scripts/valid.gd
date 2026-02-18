extends Node

var health: int = 100
var speed: float = 300.0

func take_damage(amount: int) -> void:
	health -= amount
	if health <= 0:
		queue_free()
