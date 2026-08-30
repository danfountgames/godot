class_name Ring
extends RigidBody2D

## One flat ring. Everything in the pool is one of these, and the only differences
## between them are area, whether they are lit, and which one is active right now.
##
## The hole is the ring's identity, its size language and its voice, but it is never a
## passage: the collision shape is the outer circle and nothing else. That is the brief
## being explicit, and it is also what makes merging cheap - two rings merge by adding
## areas, and there is no geometry to rebuild.

signal came_to_rest(ring: Ring)
signal bounced(ring: Ring, other: Node, speed: float)
signal absorbed(eater: Ring, eaten: Ring, speed: float)

## Uniform at every size, so a ring reads as a ring whether it is the striker or the
## monster at the end of a chain.
const HOLE_RATIO := 0.42

@export var area: float = 2400.0
## Lit rings are the set's targets. Unlit ones are terrain and ammunition.
@export var lit: bool = false
@export var density: float = 0.0022
@export var bounce: float = 0.55
@export var surface_friction: float = 0.15

## --- Water, pushed down from the pool ---------------------------------------------
## Not chosen here. How the water behaves is a property of the pool, and a value living
## on a ring spawned at runtime is one that can be tuned in the running game and never
## promoted into the scene.
@export var glide_speed: float = 80.0
@export var glide_damp: float = 0.35
@export var grab_damp: float = 4.5
@export var water_scale: float = 1.0
@export var rest_speed: float = 14.0
@export var rest_time: float = 0.2

## The rule that keeps merging from making aim irrelevant: a graze is a bounce, and only
## a hit with real momentum behind it merges. It is also what gives the jets a job -
## nudging the active ring back above the threshold is worth doing.
@export var merge_speed: float = 90.0

var active: bool = false
## Why the last contact went the way it did. A merge that does not happen looks exactly
## like a bounce from outside, and "it bounced at 275 px/s against a threshold of 90" is
## not a thing any amount of staring at the trace would have told me.
var contact_report: String = ""
var contacts: int = 0
var at_rest: bool = false

var _slow_for: float = 0.0
## The velocity as of the last physics frame, which is the one the merge rule means.
## Reading linear_velocity inside a contact callback does not give the approach
## speed: body_entered fires around the solver, not reliably before it, so the two
## bodies in one contact disagree. Measured on this game - the target reported the
## same hit at 203.9 px/s and the striker at 66.1, against a threshold of 90, so a
## shot that should have merged bounced and the whole central rule looked broken.
var _prev_velocity: Vector2 = Vector2.ZERO
var _circle: CircleShape2D


func outer_radius() -> float:
	return sqrt(maxf(1.0, area) / PI)


func inner_radius() -> float:
	return outer_radius() * HOLE_RATIO


func _ready() -> void:
	_circle = CircleShape2D.new()
	var collider := CollisionShape2D.new()
	collider.shape = _circle
	add_child(collider)

	var surface := PhysicsMaterial.new()
	surface.bounce = bounce
	surface.friction = surface_friction
	physics_material_override = surface

	contact_monitor = true
	max_contacts_reported = 8
	body_entered.connect(_on_body_entered)
	apply_area()


## Area is the source of truth; radius and mass are derived from it. Mass scaling with
## area is what makes a chain slow itself down: momentum is conserved across a merge, so
## every absorption costs speed. That is the shot's pacing, and no timer is involved.
func apply_area() -> void:
	if _circle != null:
		_circle.radius = outer_radius()
	mass = maxf(0.05, area * density)
	queue_redraw()


func _physics_process(delta: float) -> void:
	var speed := linear_velocity.length()
	# Two regimes. Above the threshold the water barely resists and the ring travels;
	# below it the water grabs and the ring settles like a curling stone rather than
	# dribbling across the pool for ten seconds.
	linear_damp = (glide_damp if speed > glide_speed else grab_damp) * water_scale
	angular_damp = linear_damp * 2.0

	if speed < rest_speed:
		_slow_for += delta
		if _slow_for >= rest_time and not at_rest:
			at_rest = true
			linear_velocity = Vector2.ZERO
			angular_velocity = 0.0
			came_to_rest.emit(self)
	else:
		_slow_for = 0.0
		at_rest = false
	_prev_velocity = linear_velocity


## Nothing is frozen when it stops. A frozen body is a static one, so the next ring to
## hit it would be hitting a wall - which is exactly the dead stop this game is not
## supposed to have. Drag brings rings to rest and Godot sleeps them; that is enough.
func _on_body_entered(body: Node) -> void:
	var other := body as Ring
	if other == null:
		bounced.emit(self, body, _prev_velocity.length())
		return

	var closing := (_prev_velocity - other._prev_velocity).length()
	contact_report = "active=%s other_active=%s closing=%0.1f threshold=%0.1f" % [
		active, other.active, closing, merge_speed]
	contacts += 1
	if active and not other.active:
		if closing >= merge_speed:
			# Deferred: the physics server is mid-step and will not accept a body being
			# freed or resized from inside a contact callback.
			call_deferred("absorb", other, closing)
		else:
			bounced.emit(self, other, closing)
		return
	if other.active:
		# The active ring owns the decision, so that one contact is never resolved twice.
		return
	# Passive against passive. Both bodies report the contact; only one should score it.
	if get_instance_id() < other.get_instance_id():
		bounced.emit(self, other, closing)


## Area-conserving, momentum-conserving. Densities are shared, so the new mass is
## exactly the sum of the two and the new velocity falls out of p = mv.
func absorb(other: Ring, speed: float) -> void:
	if other == null or not is_instance_valid(other) or other.is_queued_for_deletion():
		return
	# Momentum as it was on approach, for the same reason: by the time this deferred
	# call runs, the solver has already bounced both bodies apart.
	var momentum := _prev_velocity * mass + other._prev_velocity * other.mass
	var centroid := (position * area + other.position * other.area) / (area + other.area)
	area += other.area
	apply_area()
	position = centroid
	linear_velocity = momentum / mass
	_prev_velocity = linear_velocity
	at_rest = false
	_slow_for = 0.0
	absorbed.emit(self, other, speed)
	other.queue_free()


func _draw() -> void:
	var outer := outer_radius()
	var inner := inner_radius()
	var body_colour := Color(0.86, 0.90, 0.95)
	if lit:
		body_colour = Color(1.0, 0.78, 0.30)
	if active:
		body_colour = Color(0.42, 0.94, 0.86)
	# One arc of the right width is the whole ring: no hole to punch, and it composites
	# correctly over the water instead of painting a disc of pool colour in the middle.
	var mid := (outer + inner) * 0.5
	draw_arc(Vector2.ZERO, mid, 0.0, TAU, 48, body_colour, outer - inner, true)
	draw_arc(Vector2.ZERO, outer, 0.0, TAU, 48, Color(0.06, 0.16, 0.24, 0.5), 2.0, true)
