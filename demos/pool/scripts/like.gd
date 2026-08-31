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
## The pool is draining, slowly, all the time. Against `drift_damp` this settles at about
## forty pixels a second, so a Like left alone crosses the pool and is gone in roughly ten
## seconds.
##
## Without it Likes had zero gravity and damping, so they drifted a few pixels and then
## stopped **forever**: 90 to 130 of them sat motionless in the water on every board played
## without the current, the stated cost of push (your uncollected reward goes over the
## edge) never once happened, and a player who had not worked out that the left button is
## a hoover finished a board having collected nothing, with no way to find out why. A
## reward that waits indefinitely is not a reward, it is furniture.
@export var drain_pull: float = 48.0

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
	sleeping = false
	apply_central_force(Vector2(0.0, drain_pull) * mass)
	queue_redraw()


func _draw() -> void:
	var pulse := 0.85 + sin(_age * 7.0) * 0.15
	draw_circle(Vector2.ZERO, radius * pulse, Color(1.0, 0.42, 0.62))
	draw_circle(Vector2.ZERO, radius * 0.42 * pulse, Color(1.0, 0.85, 0.92))
