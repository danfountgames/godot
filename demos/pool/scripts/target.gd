class_name Target
extends RigidBody2D

## A ring in the water, and the thing the striker is trying to destroy.
##
## The differentiator is that these have **real holes** you can shoot through. Getting
## that right in two dimensions took a rebuild, and the reason is worth keeping:
##
## The first version made the rim out of twelve circle colliders in a closed loop, with
## the middle genuinely empty, and expected threading to fall out of ordinary physics. It
## cannot. **A 2D annulus encloses its own hole** - there is no direction a ball can come
## from that reaches the middle without crossing the rim first. A full board produced
## twenty-eight rim hits and zero threads, and it would never have produced one.
##
## What the picture actually means is depth: the striker is small and rides low, the rings
## are inflatables floating on the surface, and a shot through the middle passes *under*
## the rim. So the rim is not a collider against the striker at all. The ring is a sensor,
## and the pass is judged on the striker's line: miss the centre by less than the hole and
## you are through it, clip the rim and you bounce off it. Loose rings still collide with
## each other and the walls as bodies - that part was never the problem.

signal destroyed(target: Target, at: Vector2, worth: int)
signal rim_hit(target: Target, at: Vector2, remaining: int)
signal threaded(target: Target)
## The striker clipped the rim: the game reflects it and scores the hit.
signal struck(target: Target, striker: Striker)
## One ring hit another hard enough to matter. This is push's entire payoff and it was
## missing: rings shouldered each other about all day and nothing ever called
## take_rim_hit, so a measured forty-five seconds of held push against a full board
## destroyed zero targets and moved none off it. The README promised this; the code did
## not do it.
signal collided(hitter: Target, hit: Target, speed: float)

enum Kind {
	ANCHORED, ## Level geometry. Does not move; flexes when the current pushes it.
	LOOSE, ## Drifts on the current, collides with everything, and is the spectacle.
	HEAVY, ## Slow, hard to shift, and dangerous once it is moving.
	ARMOURED, ## Takes several rim hits.
	POPPER, ## Destroys its neighbours when it goes.
	COLLECTOR, ## Eats loose Likes until it is destroyed.
}

@export var kind: Kind = Kind.ANCHORED
@export var outer_radius: float = 34.0
## The hole, as a fraction of the outer radius. Measured against the alternative: at 0.6
## with an 11px striker, a full board of twenty collisions produced *zero* threads - the
## gap was real but needed about seven pixels of accuracy at 430 px/s, which is not a
## skill, it is a coincidence.
##
## Raised from 0.66 when the board grew from twenty-eight rings to fifty and the rings
## shrank to fit. Threading is the game's identity and its window is `hole - striker`, so
## a smaller ring silently makes the whole mechanic rarer; 0.72 against a 6.5px striker
## puts the smallest ring here back to about nine pixels of aim and the biggest to
## fifteen, which is roughly where the thirty-four-pixel rings were.
@export var hole_ratio: float = 0.72
## A tight ring: the hole is too small for the striker, so it is a wall with a decoration
## in the middle. Roughly a third of a board. Without these, every ring was threadable
## and none of them looked any different, so the question the mechanic asks - which of
## these can I shoot through - had the same answer everywhere.
@export var tight: bool = false
@export var tight_hole_ratio: float = 0.34
@export var rim_segments: int = 12
@export var hit_points: int = 1
@export var worth: int = 10
## How fast one ring has to meet another to damage it. Below this a pile of floats is
## scenery, which is what a pool of inflatables should mostly be.
@export var impact_speed: float = 200.0

var alive: bool = true
var threads: int = 0

var _hole: Area2D
var _flex: float = 0.0
var _impact_cooldown: float = 0.0


func hole_radius() -> float:
	return outer_radius * (tight_hole_ratio if tight else hole_ratio)


## Can a striker of this radius pass through the middle? The one question the whole
## mechanic rests on, answered by geometry rather than by a table.
func threadable_by(p_radius: float) -> bool:
	return p_radius < hole_radius() - 2.0


func _ready() -> void:
	gravity_scale = 0.0
	_configure_kind()
	_build_rim()
	contact_monitor = true
	max_contacts_reported = 8

	# The whole ring as a sensor. The striker is on its own layer and passes through every
	# target body; what happens when it arrives is decided here, on its line, rather than
	# by a solver that has no way to tell a thread from a graze.
	_hole = Area2D.new()
	_hole.name = "Sensor"
	var hole_shape := CircleShape2D.new()
	hole_shape.radius = outer_radius
	var hole_collider := CollisionShape2D.new()
	hole_collider.shape = hole_shape
	_hole.add_child(hole_collider)
	_hole.collision_layer = 0
	_hole.collision_mask = 2 # the striker's layer, and nothing else
	_hole.body_entered.connect(_on_sensor_entered)
	add_child(_hole)

	body_entered.connect(_on_body_entered)


func _configure_kind() -> void:
	var surface := PhysicsMaterial.new()
	surface.bounce = 0.6
	surface.friction = 0.1
	physics_material_override = surface

	match kind:
		Kind.ANCHORED:
			# Immovable, but not inert: freeze_mode STATIC would make it a wall, and the
			# brief is explicit that even anchored targets should visibly answer the
			# current. It flexes in _draw instead, which costs nothing and reads as the
			# water pulling at something that is tied down.
			freeze = true
			freeze_mode = RigidBody2D.FREEZE_MODE_STATIC
			mass = 1000.0
		Kind.LOOSE:
			mass = 1.0
			linear_damp = 1.6
			angular_damp = 2.0
		Kind.HEAVY:
			mass = 6.0
			linear_damp = 2.4
			angular_damp = 3.0
		Kind.ARMOURED:
			freeze = true
			freeze_mode = RigidBody2D.FREEZE_MODE_STATIC
			mass = 1000.0
			hit_points = maxi(hit_points, 3)
			worth = worth * 3
		Kind.POPPER:
			mass = 0.8
			linear_damp = 1.6
			worth = worth * 2
		Kind.COLLECTOR:
			freeze = true
			freeze_mode = RigidBody2D.FREEZE_MODE_STATIC
			mass = 1000.0
			worth = worth * 2


## The body, for everything that is not the striker: loose rings shouldering each other
## about, the walls, the party wave. Still a ring of colliders rather than a disc, so a
## pile of floats interlocks the way inflatables do instead of stacking like coins.
func _build_rim() -> void:
	# Layer 16, not layer 1. The striker's mask is layer 1 (the walls), and Godot pairs two
	# bodies when *either* mask holds the other's layer - so a target sharing the walls'
	# layer was solid to the striker no matter what the sensor said. Masking 1 and 16 keeps
	# what this body is for: shouldering other rings, the walls, and the lounger.
	collision_layer = 16
	collision_mask = 1 | 16
	var mid := (outer_radius + hole_radius()) * 0.5
	var thickness := maxf(3.0, (outer_radius - hole_radius()) * 0.5)
	for i in rim_segments:
		var angle := TAU * float(i) / float(rim_segments)
		var shape := CircleShape2D.new()
		shape.radius = thickness
		var collider := CollisionShape2D.new()
		collider.shape = shape
		collider.position = Vector2(cos(angle), sin(angle)) * mid
		add_child(collider)


## Cut loose from its anchor, so the water can have it. Used when the drain opens at the
## end of a board: the last few rings stop being level geometry and become flotsam that
## comes to the player, rather than three survivors to be hunted round the corners.
func release() -> void:
	if not freeze:
		return
	freeze = false
	mass = 2.0
	linear_damp = 1.4
	angular_damp = 2.0
	queue_redraw()


## Push from the current. Anchored rings cannot move, so they flex - which is the only
## honest way to say "the water is pulling at this" about something bolted down.
func apply_current(p_push: Vector2) -> void:
	if not alive:
		return
	if freeze:
		_flex = clampf(_flex + p_push.length() * 0.0016, 0.0, 1.0)
		queue_redraw()
		return
	sleeping = false
	apply_central_impulse(p_push * mass)


func _physics_process(delta: float) -> void:
	if _flex > 0.0:
		_flex = maxf(0.0, _flex - delta * 1.8)
		queue_redraw()
	if _impact_cooldown > 0.0:
		_impact_cooldown = maxf(0.0, _impact_cooldown - delta)


## Another ring, arriving. `body_entered` fires on both of them, so the cooldown is set on
## the pair and only one of the two reports the hit; without that a resting pile grinds
## itself to pieces on re-entry noise.
func _on_body_entered(body: Node) -> void:
	var other := body as Target
	if other == null or not alive or not other.alive:
		return
	if _impact_cooldown > 0.0 or other._impact_cooldown > 0.0:
		return
	var relative := (linear_velocity - other.linear_velocity).length()
	if relative < impact_speed:
		return
	note_impact()
	other.note_impact()
	collided.emit(self, other, relative)


func note_impact() -> void:
	_impact_cooldown = 0.25


func take_rim_hit(p_at: Vector2, p_damage: int = 1) -> void:
	if not alive:
		return
	hit_points -= p_damage
	if hit_points > 0:
		rim_hit.emit(self, p_at, hit_points)
		queue_redraw()
		return
	alive = false
	destroyed.emit(self, global_position, worth)


## Thread or clip, decided on the striker's line rather than on where it happens to be
## the moment it crosses the boundary.
##
## The miss distance is the perpendicular from the ring's centre to the line the striker
## is travelling along. If the whole ball clears that by less than the hole, it is going
## through the middle; otherwise it is going to hit the rim. Judged from the line and not
## from the entry point because a ball entering near the edge on a path straight through
## the centre is a thread, and a ball entering dead centre on a tangent is not.
func _on_sensor_entered(body: Node) -> void:
	if not alive:
		return
	var striker := body as Striker
	if striker == null:
		return
	var to_centre := global_position - striker.global_position
	var heading := striker.linear_velocity.normalized()
	if heading.is_zero_approx():
		return
	var miss := absf(to_centre.cross(heading))
	if miss + striker.radius <= hole_radius():
		threads += 1
		threaded.emit(self)
		return
	struck.emit(self, striker)


func _draw() -> void:
	var outer := outer_radius
	var inner := hole_radius()
	var colour := Color(0.86, 0.90, 0.95)
	match kind:
		Kind.ANCHORED:
			colour = Color(1.0, 0.78, 0.36)
		Kind.LOOSE:
			colour = Color(0.55, 0.85, 1.0)
		Kind.HEAVY:
			colour = Color(0.75, 0.55, 0.95)
		Kind.ARMOURED:
			colour = Color(0.72, 0.78, 0.84)
		Kind.POPPER:
			colour = Color(1.0, 0.45, 0.55)
		Kind.COLLECTOR:
			colour = Color(0.45, 1.0, 0.62)
	# Damage reads as the ring going pale before it goes.
	if hit_points > 1:
		colour = colour.lerp(Color(1, 1, 1), 0.25)
	var mid := (outer + inner) * 0.5
	var width := (outer - inner) * (1.0 + _flex * 0.25)
	draw_arc(Vector2.ZERO, mid, 0.0, TAU, 40, colour, width, true)
	# The hole states whether it is a way through. A bright inner edge means the striker
	# fits; a dark one means it does not. Two pixels of 55%-alpha line over a dark pool
	# said nothing at all, and "can I shoot through this" is the only question the player
	# is ever asking.
	if threadable_by(Striker.DEFAULT_RADIUS):
		draw_arc(Vector2.ZERO, inner, 0.0, TAU, 32, Color(0.55, 1.0, 0.95, 0.85), 3.0, true)
	else:
		draw_circle(Vector2.ZERO, inner, Color(0.06, 0.16, 0.24, 0.9))
