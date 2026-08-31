class_name Paddle
extends StaticBody2D

## The inflatable lounger. It slides along the near edge of the pool and it is where the
## current comes from.
##
## The rings-only rule is relaxed here on purpose. A horizontal bar is the most readable
## thing in a breakout game - it says "this is you, and this is the line you defend" at a
## glance - and three rubber rings lashed together is still a lounger. Rendered as three
## rings, collided with as one capsule, so the shape is characterful and the bounce is
## predictable.

@export var width: float = 132.0
@export var thickness: float = 22.0
## How far off straight-up a hit at the very end of the lounger sends the striker. The
## contact position across the paddle is the player's aim, and it has to matter more than
## the incoming angle does or there is no skill in the rebound.
@export var max_bounce_degrees: float = 40.0
@export var move_speed: float = 620.0

## Where the player wants it. Set this and the paddle goes there; a human's key presses
## end up here too, so what an agent asserts about is what a person does.
var wanted_x: float = 0.0

var _limit_low: float = 0.0
var _limit_high: float = 0.0


var _collider: CollisionShape2D


func _ready() -> void:
	rebuild()
	wanted_x = position.x
	# Layer 8, masking only the rings (16). The rebound against the striker is resolved in
	# code so that where the ball lands across the lounger decides where it goes; letting
	# the solver also have an opinion would mean the incoming angle quietly won half the
	# time, and the player would never learn to aim. The striker is on layer 2 and this
	# masks 16, so the solver never sees that pair.
	#
	# Rings are a different matter, and this used to mask nothing at all. A ring pulled
	# down the pool went *through* the lounger and out of the open edge, which deleted it
	# for free: pull was a clear button, and the danger half of the pull/push trade did not
	# exist in the build. Now the lounger is something a float can pile up against.
	collision_layer = 8
	collision_mask = 16


## Rebuilt rather than scaled, because a cocktail that widens the lounger has to widen
## the thing the ball actually meets as well as the thing on screen. Called on the way in
## and whenever the width changes.
func rebuild() -> void:
	if _collider == null:
		_collider = CollisionShape2D.new()
		_collider.rotation = PI * 0.5
		add_child(_collider)
	var shape := CapsuleShape2D.new()
	shape.radius = thickness * 0.5
	shape.height = width
	_collider.shape = shape
	queue_redraw()


func set_limits(p_low: float, p_high: float) -> void:
	_limit_low = p_low + width * 0.5
	_limit_high = p_high - width * 0.5


func _physics_process(delta: float) -> void:
	if _limit_high > _limit_low:
		wanted_x = clampf(wanted_x, _limit_low, _limit_high)
	# Moved at a speed rather than teleported, so the paddle cannot outrun the ball and
	# a rescue is a thing you have to have started early enough.
	position.x = move_toward(position.x, wanted_x, move_speed * delta)
	queue_redraw()


## Where a striker hitting at `p_at` should go. Straight up from the middle, fanning out
## towards the ends; the incoming angle is discarded entirely, which is what makes the
## rebound something the player aims rather than something that happens to them.
func rebound_direction(p_at: Vector2) -> Vector2:
	var offset := clampf((p_at.x - global_position.x) / (width * 0.5), -1.0, 1.0)
	return Vector2.UP.rotated(deg_to_rad(max_bounce_degrees) * offset)


func _draw() -> void:
	# Three linked rings: the lounger, without giving up the identity.
	var body := Color(1.0, 0.62, 0.30)
	var spacing := width / 3.0
	for i in 3:
		var at := Vector2(-width * 0.5 + spacing * (float(i) + 0.5), 0.0)
		draw_arc(at, thickness * 0.62, 0.0, TAU, 24, body, thickness * 0.52, true)
	draw_line(Vector2(-width * 0.5, 0), Vector2(width * 0.5, 0), body.darkened(0.2), 5.0)
