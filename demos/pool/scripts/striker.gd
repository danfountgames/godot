class_name Striker
extends RigidBody2D

## The ball. A small rubber ring, and the one object in this pool that is **not** subject
## to the water.
##
## This is the mechanical decision the whole game turns on, and it reverses what POOL used
## to be. Everything else here glides, slows and settles, because a pool full of drifting
## clutter is the pleasure of the setting. A breakout ball that does that is a breakout
## ball that limply stops halfway up the board, and the game dies with it.
##
## So the striker holds its speed. The current does not slow it or speed it up - it
## *steers* it, and then the speed is restored. The player is bending a trajectory, never
## fighting one, and there is no state in which the ball is drifting uselessly.
##
## Everything about it should still look wet: the wake, the wobble, the spray. Underneath
## it is arcade motion with a constant magnitude.


## Small, because the whole point is that it fits through things. A striker much
## bigger than this turns every ring into a solid brick and the game becomes
## ordinary breakout with a pool skin.
##
## Shared as a constant because the ring has to draw the same answer the ring *decides*:
## Target paints the hole bright when the striker fits, and when that test was written
## against a hardcoded 8.0 the picture would have started lying the moment this changed.
const DEFAULT_RADIUS := 6.5
@export var radius: float = DEFAULT_RADIUS
## Held, not approached. The current changes where this points and never how big it is.
@export var speed: float = 430.0
## How hard the current can turn it, in radians per second at full strength. Enough to
## curve a shot back to the paddle across half a pool; not enough to reverse it.
##
## 3.4 was too much by about threefold, and it ate the best thing in the game. Measured
## with the pool emptied and the lounger parked: the catch window is 74px with no current
## - half the lounger plus the ball, which is the honest geometry - and **300px** with
## pull held, so a 132px lounger defended 71% of an 840px pool. At that authority you
## cannot miss, and the rebound aiming that the whole game is built on stops mattering
## because any rebound comes back. 1.2 puts the window near 110px: pull is a real rescue,
## and it is still a rescue you can fail.
@export var turn_rate: float = 1.2
## The shallowest line the ball is allowed to travel, as a fraction of its speed.
##
## Without this a striker can settle into a near-horizontal orbit between the side walls
## and never come back down. It is not a rare corner: one measured board ran **200 seconds
## and destroyed eighteen of forty-four rings** because the ball spent nearly all of it
## skimming the top of the pool, and the same pin is how an earlier run farmed 494 repeat
## threads. The current cannot rescue it either, because the pull is weakest at exactly
## the far end where the orbit sits. At 0.18 the ball crosses the pool vertically at least
## every seven seconds, whatever else is done to it, and the orbit stops existing.
@export var min_vertical: float = 0.18

var launched: bool = false
var threads_this_life: int = 0

var _trail: PackedVector2Array = PackedVector2Array()


func _ready() -> void:
	gravity_scale = 0.0
	linear_damp = 0.0
	angular_damp = 0.0
	var surface := PhysicsMaterial.new()
	# Perfectly elastic. Anything less is drag by another name: fifty rebounds at 0.95
	# is a ball that has quietly stopped.
	surface.bounce = 1.0
	surface.friction = 0.0
	physics_material_override = surface
	var shape := CircleShape2D.new()
	shape.radius = radius
	var collider := CollisionShape2D.new()
	collider.shape = shape
	add_child(collider)
	# Layer 2, colliding only with the walls (layer 1). It passes straight through every
	# target body: whether a pass is a thread or a clip is decided by Target, on the
	# striker's line, because a solver cannot tell those apart and a 2D ring encloses its
	# own hole.
	#
	# This only became true when the targets moved off layer 1. Godot pairs two bodies if
	# *either* one's mask contains the other's layer, so while targets were also on layer 1
	# the solver bounced the striker off the rim regardless of what the sensor decided: a
	# "thread" scored, raised the multiplier, and then the ball clipped anyway. The rule
	# the whole game rests on was decorative for as long as those two bits were the same.
	collision_layer = 2
	collision_mask = 1
	contact_monitor = true
	max_contacts_reported = 8


func launch(p_direction: Vector2) -> void:
	launched = true
	linear_velocity = p_direction.normalized() * speed


## Steering, not pushing. `p_turn` is a direction to bend towards and `p_strength` is how
## much of this frame's heading to give it; the magnitude is put back afterwards, so the
## current can aim the ball anywhere and never change how fast it is going.
func steer(p_turn: Vector2, p_strength: float, p_delta: float) -> void:
	if not launched or p_turn.is_zero_approx():
		return
	var heading := linear_velocity.normalized()
	if heading.is_zero_approx():
		heading = Vector2.UP
	var wanted := p_turn.normalized()
	# Rotate towards the wanted heading by at most this many radians, so strength reads
	# as "how sharply", never as "how fast". A negative strength bends away instead,
	# which is the whole of what push does to the ball.
	var most := turn_rate * p_delta * absf(clampf(p_strength, -1.0, 1.0))
	var step := clampf(heading.angle_to(wanted), -most, most)
	if p_strength < 0.0:
		step = -step
	linear_velocity = heading.rotated(step) * speed


func _physics_process(_delta: float) -> void:
	if not launched:
		return
	# The speed is a constant of the game, restored every frame. A collision that bled
	# energy, a solver that ate some, a graze along a wall - none of them are allowed to
	# turn the ball into something that has to be rescued.
	var heading := linear_velocity.normalized()
	if heading.is_zero_approx():
		heading = Vector2.UP
	# Applied here rather than at each place a heading is set, because a wall bounce, a
	# rim deflection and the current can each produce a shallow line and only the frame
	# loop sees all three.
	if absf(heading.y) < min_vertical:
		heading.y = min_vertical * (-1.0 if heading.y < 0.0 else 1.0)
		heading = heading.normalized()
	linear_velocity = heading * speed

	_trail.push_back(global_position)
	if _trail.size() > 14:
		_trail.remove_at(0)
	queue_redraw()


func _draw() -> void:
	# The wake, so it looks like water even though it does not behave like it.
	for i in _trail.size():
		var at := to_local(_trail[i])
		var fade := float(i) / float(maxi(1, _trail.size()))
		draw_circle(at, radius * (0.3 + fade * 0.5), Color(1, 1, 1, 0.06 + fade * 0.10))
	draw_circle(Vector2.ZERO, radius, Color(1.0, 0.98, 0.85))
	draw_circle(Vector2.ZERO, radius * 0.45, Color(0.10, 0.35, 0.5))


## Reflects off a rim it clipped. Called by the game rather than by the solver, because
## the solver was never allowed to see this collision - see Target.
func deflect_from(p_centre: Vector2) -> void:
	var normal := (global_position - p_centre).normalized()
	if normal.is_zero_approx():
		normal = Vector2.UP
	var heading := linear_velocity.normalized()
	if heading.dot(normal) > 0.0:
		# Already leaving. Reflecting again would send it back in, and a ball that
		# ping-pongs inside one ring is the bug this guard exists for.
		return
	linear_velocity = heading.bounce(normal) * speed
	# Nudged clear so the sensor cannot re-trigger on the same pass.
	global_position += normal * (radius + 2.0)
