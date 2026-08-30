class_name Like
extends RigidBody2D

## A Like: what a destroyed ring leaves behind, and the reason to bring danger towards
## yourself.
##
## Subject to the water, unlike the striker. It drifts, it is pulled, and if it reaches
## the bottom edge it is gone - so pulling is how you collect, and pulling is also what
## drags the loose rings down at you. That trade is the whole of the risk in this game.

@export var worth: int = 1
@export var radius: float = 7.0
## Below this the pull just moves it; above it, it is coming to you fast enough that it
## will reach the paddle without further help.
@export var drift_damp: float = 1.2

var _age: float = 0.0


func _ready() -> void:
	gravity_scale = 0.0
	linear_damp = drift_damp
	angular_damp = drift_damp
	var shape := CircleShape2D.new()
	shape.radius = radius
	var collider := CollisionShape2D.new()
	collider.shape = shape
	add_child(collider)
	# Likes pass through everything except the collector that eats them; they are reward,
	# not obstacle, and a pool of them jamming the board would be neither.
	collision_layer = 4
	collision_mask = 0


func apply_current(p_push: Vector2) -> void:
	sleeping = false
	apply_central_impulse(p_push * mass)


func _physics_process(delta: float) -> void:
	_age += delta
	queue_redraw()


func _draw() -> void:
	var pulse := 0.85 + sin(_age * 7.0) * 0.15
	draw_circle(Vector2.ZERO, radius * pulse, Color(1.0, 0.42, 0.62))
	draw_circle(Vector2.ZERO, radius * 0.42 * pulse, Color(1.0, 0.85, 0.92))
