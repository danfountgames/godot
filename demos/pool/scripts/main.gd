class_name PoolGame
extends Node2D

## POOL, first playable: the brief's section 8.
##
## One rectangular pool, striker plus a field of rings, bounce-or-merge decided by
## impact speed, a chain multiplier, likes, one jet, three shots.
##
## The rule the whole thing hangs on: the ring you shot is *active* until it comes to
## rest, and while it is active any hard enough contact absorbs what it hit. Everything
## else bounces. Merges conserve area and momentum, so a chain makes you bigger, heavier
## and slower - the shot paces itself and there is no timer anywhere in this file.
##
## One design decision is aimed squarely at being drivable by an agent rather than only
## by a person: every verb is reachable as a *property*, not only as input. Setting
## `queued_shot` fires the striker; setting `jet_held` runs the jet; setting
## `restart_requested` rebuilds the board. A person drags and holds a key, and both
## routes end in the same call. Without that, driving this game from a tool means
## simulating a drag, and a drag is a bad unit of intent to assert about.

@export var shots_per_set: int = 3
@export var field_rings: int = 12
@export var lit_rings: int = 5
@export var striker_area: float = 2400.0
@export var field_area_min: float = 700.0
@export var field_area_max: float = 1900.0
## Centre to centre in the rack. A ring of the largest field area is about 73px
## across, so this is a gap of about a ring's width - loose enough to travel through
## and tight enough that a merge leaves you touching the next one.
@export var rack_spacing: float = 78.0
@export var max_shot_speed: float = 380.0

## --- The water --------------------------------------------------------------------
## These live here rather than on each ring: how the water behaves is a property of the
## pool, and a value on a ring spawned at runtime has no counterpart in the scene, so it
## could be tuned in the running game and never promoted.
##
## The numbers come from measuring. Under linear damping g a ring launched at v0 covers
## (v0/g)(1-e^-gt), so 320 px/s against g=0.35 puts 565px at 2.74s over this pool. The
## glide threshold then has to sit low enough that the ring is still gliding when it
## gets there - at 220 the water grabbed after 300px and nothing could cross at all.
@export var glide_speed: float = 80.0
@export var glide_damp: float = 0.35
@export var grab_damp: float = 4.5
## A graze is a bounce. Only a hit with momentum behind it merges, which is what stops
## aiming becoming irrelevant as the active ring grows, and what gives the jets a job.
## Found by trying four settings against the running game, not by taste. Four things
## have to hold at once and they pull against each other: momentum is conserved on a
## merge, so absorbing a ring of your own size halves your speed and the next merge
## has to still clear this threshold. A field lighter than the striker is what buys
## the chain its length.
##
## Lowering it further makes chains *worse*, which was the surprise. At 35 a slow
## graze merges, so the ring is heavy before it is through the rack and stops early:
## chains went 4,3,3 at fifty and 1,3,2 at thirty-five. The threshold is what makes
## merging a skill rather than a certainty, which is the brief's own open question
## answered by measurement.
@export var merge_speed: float = 50.0

## --- Likes ---------------------------------------------------------------------------
## Event-driven only. Nothing generates while idle, which is also what keeps the music
## honest: silence when nothing is happening.
@export var like_per_bounce: float = 1.0
@export var like_per_merge: float = 12.0
@export var chain_gain: float = 0.35
@export var on_beat_bonus: float = 1.5

## --- Upgrade state ------------------------------------------------------------------
var water_slickness: float = 1.0
var striker_mass_scale: float = 1.0

var likes: float = 0.0
var multiplier: float = 1.0
var chain: int = 0
var best_chain: int = 0
var best_multiplier: float = 1.0
var shots_left: int = 0
var sets_played: int = 0
var lit_left: int = 0
var rings_in_play: int = 0
var set_cleared: bool = false
var last_shot_speed: float = 0.0
var bounces: int = 0
var merges: int = 0
var on_beat_hits: int = 0

## --- The agent-facing verbs ---------------------------------------------------------
var queued_shot: Vector2 = Vector2.ZERO
var jet_held: bool = false
var restart_requested: bool = false

## --- Measurements the brief asks for -------------------------------------------------
## Section 2 states one number as a feel target: everything at rest within about 5 to 6
## seconds of the last event. Neither that nor the shape of a chain is checkable by
## looking at a file, so the game measures itself and leaves the answers where a tool
## can read them in one call.
var everything_at_rest: bool = true
var settle_time: float = 0.0
var crossing_time: float = 0.0
var shot_distance: float = 0.0
var shot_duration: float = 0.0
var shot_peak_speed: float = 0.0
var first_impact_time: float = -1.0
var first_impact_speed: float = 0.0
var chain_this_shot: int = 0
## As the shot ended, not as the board stands. Reading the active ring afterwards
## reports the next striker, so a three-merge chain came back as a ring that had
## never grown.
var shot_end_area: float = 0.0
var shot_end_distance: float = 0.0
## Active-ring speed per physics frame since launch, so one read gets the whole flight
## rather than three samples of a shot that is already over.
var shot_trace: PackedFloat32Array = PackedFloat32Array()
## The pitches the last chain played, in order. A descending run here is the brief's
## central audio claim, checkable without a speaker.
var last_phrase: PackedInt32Array = PackedInt32Array()

## The board, as one read. A player looks at the pool; anything driving this game from
## outside has to be able to as well, and asking twenty nodes for their position one
## round trip at a time is not looking at a board.
var active_position: Vector2 = Vector2.ZERO
var active_area: float = 0.0
var lit_positions: PackedVector2Array = PackedVector2Array()
var ring_positions: PackedVector2Array = PackedVector2Array()

var _pool: Pool
var _jet: Jet
var _music: PoolMusic
var _rings: Node2D
var _active: Ring
var _aim_from: Vector2 = Vector2.ZERO
var _aiming: bool = false
var _motion_for: float = 0.0
var _tracing: bool = false
## A striker that has not been shot yet must not resolve the shot. It spawns at
## rest, and a ring at rest reports itself so a fifth of a second later - which
## resolved the shot, spawned the next striker, and did it again for ever.
var _shot_live: bool = false
## When something last happened, in seconds. The brief measures settling from the
## last event rather than from the launch, and the two differ by a whole chain.
var _last_event_at: float = -1.0
var _crossing_pending: bool = false
var _last_active_at: Vector2 = Vector2.ZERO
var _serial: int = 0

const TRACE_LIMIT := 1200
const RING_SCRIPT := preload("res://scripts/ring.gd")


func _ready() -> void:
	_pool = $Pool
	_jet = $Jet
	_music = $Music
	_rings = $Rings
	randomize()
	_begin_run()


## --- Building the board ---------------------------------------------------------------

func _begin_run() -> void:
	for child in _rings.get_children():
		_rings.remove_child(child)
		child.queue_free()
	_serial = 0
	likes = 0.0
	multiplier = 1.0
	chain = 0
	best_chain = 0
	best_multiplier = 1.0
	sets_played = 0
	bounces = 0
	merges = 0
	on_beat_hits = 0
	if _music != null:
		_music.reset_log()
	_begin_set(true)


## Survivors carry into the next set as obstacles and ammunition, so this only adds. A
## hard reset is `restart_requested`, and it is a different thing on purpose: a set that
## silently cleared the table would hide whether the player left themselves a good board.
func _begin_set(fresh: bool) -> void:
	if fresh:
		for child in _rings.get_children():
			_rings.remove_child(child)
			child.queue_free()
	sets_played += 1
	shots_left = shots_per_set
	set_cleared = false
	multiplier = 1.0
	_spawn_field()
	_spawn_striker()
	_recount()


## Tops the board up to field_rings and the targets up to lit_rings, in slots nothing
## is sitting in. Laying out a whole new field each set was the reason lit_left went up
## over a set instead of down: the survivors are meant to be most of the next board, and
## adding twelve more on top of them buries the thing the player just built.
func _spawn_field() -> void:
	_recount()
	var free := _free_slots(52.0)
	free.shuffle()
	var want := maxi(0, field_rings - _field_count())
	var want_lit := maxi(0, lit_rings - lit_left)
	for i in mini(want, free.size()):
		var ring := _add_ring(free[i], randf_range(field_area_min, field_area_max))
		ring.lit = i < want_lit
	_recount()


func _field_count() -> int:
	var count := 0
	for child in _rings.get_children():
		var ring := child as Ring
		if ring != null and ring != _active and not ring.is_queued_for_deletion():
			count += 1
	return count


## The candidate lattice, minus anything already occupied. Returning the free ones
## rather than a "is this clear" predicate keeps the caller from spawning two rings into
## the same gap in one pass.
func _free_slots(clearance: float) -> Array[Vector2]:
	var out: Array[Vector2] = []
	var centre := Vector2(_pool.size.x * 0.5, _pool.size.y * 0.32)
	# Rows of 3, 4, 5, racked. Close enough that absorbing one leaves you inside the
	# next, which is the only way a chain happens at all. Spread over the whole pool the
	# rings sat 246px apart and a ring is 60px across, so every shot chained exactly
	# once and then sailed through the gaps for two seconds.
	var row_counts := [3, 4, 5]
	for row in row_counts.size():
		var count: int = row_counts[row]
		for column in count:
			var at := centre + Vector2(
				(float(column) - float(count - 1) * 0.5) * rack_spacing,
				(float(row) - 1.0) * rack_spacing * 0.92)
			if _is_clear(at, clearance):
				out.append(at)
	return out


func _is_clear(at: Vector2, clearance: float) -> bool:
	for child in _rings.get_children():
		var ring := child as Ring
		if ring == null or ring.is_queued_for_deletion():
			continue
		if at.distance_to(ring.position) < clearance + ring.outer_radius():
			return false
	return true


## Along the shooting edge, in the first place with room for it. Spawning at a fixed
## point regardless of what was already parked there gave a first impact one frame after
## the launch, which is not a shot.
func _spawn_striker() -> void:
	var edge := _pool.size.y - 70.0
	var at := Vector2(_pool.size.x * 0.5, edge)
	var clearance := sqrt(striker_area / PI) + 12.0
	for step in 12:
		# 0, +86, -86, +172, -172 ... so the striker walks outward from the middle.
		var offset := float(floori(float(step + 1) / 2.0)) * 86.0 * (1.0 if step % 2 == 0 else -1.0)
		var candidate := Vector2(clampf(_pool.size.x * 0.5 + offset, 60.0, _pool.size.x - 60.0), edge)
		if _is_clear(candidate, clearance):
			at = candidate
			break
	_active = _add_ring(at, striker_area)
	_active.mass *= striker_mass_scale
	_active.active = true
	_active.queue_redraw()


func _add_ring(at: Vector2, a: float) -> Ring:
	var node := RigidBody2D.new() as Node
	node.set_script(RING_SCRIPT)
	var ring := node as Ring
	ring.area = a
	ring.position = at
	ring.water_scale = water_slickness
	ring.glide_speed = glide_speed
	ring.glide_damp = glide_damp
	ring.grab_damp = grab_damp
	ring.merge_speed = merge_speed
	# Named, because an unnamed RigidBody2D comes back from the running game as
	# "@RigidBody2D@270" and nothing outside can address it twice running.
	_serial += 1
	ring.name = "Ring%d" % _serial
	ring.came_to_rest.connect(_on_came_to_rest)
	ring.bounced.connect(_on_bounced)
	ring.absorbed.connect(_on_absorbed)
	_rings.add_child(ring)
	return ring


func apply_water_to_existing() -> void:
	for child in _rings.get_children():
		var ring := child as Ring
		if ring != null:
			ring.water_scale = water_slickness
			ring.glide_speed = glide_speed
			ring.glide_damp = glide_damp
			ring.grab_damp = grab_damp
			ring.merge_speed = merge_speed


func _recount() -> void:
	rings_in_play = 0
	lit_left = 0
	lit_positions = PackedVector2Array()
	ring_positions = PackedVector2Array()
	for child in _rings.get_children():
		var ring := child as Ring
		if ring == null or ring.is_queued_for_deletion():
			continue
		rings_in_play += 1
		ring_positions.append(ring.position)
		if ring.lit:
			lit_left += 1
			lit_positions.append(ring.position)
	if _active != null and is_instance_valid(_active):
		active_position = _active.position
		active_area = _active.area


## --- The loop -------------------------------------------------------------------------

func _physics_process(delta: float) -> void:
	if restart_requested:
		restart_requested = false
		_begin_run()
		return

	_jet.firing = jet_held and _jet.pressure > 0.0

	if queued_shot != Vector2.ZERO:
		_launch(queued_shot)
		queued_shot = Vector2.ZERO

	var moving := 0
	for child in _rings.get_children():
		var ring := child as Ring
		if ring != null and not ring.at_rest and not ring.is_queued_for_deletion():
			moving += 1
	everything_at_rest = moving == 0

	if moving > 0:
		_motion_for += delta
	elif _motion_for > 0.0:
		_motion_for = 0.0
		# Cleared when the active ring rests, which is normally the same instant the
		# board does - so measuring this off _tracing recorded nothing at all.
		if _last_event_at >= 0.0:
			settle_time = Time.get_ticks_msec() / 1000.0 - _last_event_at
			_last_event_at = -1.0

	_trace(delta)
	_recount()


## Everything about the shot the brief states a number for, measured while it happens.
## Crossing is counted along the path travelled rather than as a position reached: a
## shot that hits a ring forty pixels in never reaches the far end, and reporting 0.0
## for that would read as "instant" rather than "never got there".
func _trace(delta: float) -> void:
	if not _tracing:
		return
	if _active == null or not is_instance_valid(_active):
		_tracing = false
		return
	var speed := _active.linear_velocity.length()
	shot_distance += _active.position.distance_to(_last_active_at)
	_last_active_at = _active.position
	shot_duration += delta
	shot_peak_speed = maxf(shot_peak_speed, speed)
	if shot_trace.size() < TRACE_LIMIT:
		shot_trace.append(speed)
	if _crossing_pending and shot_distance >= _pool.size.y:
		crossing_time = shot_duration
		_crossing_pending = false


func _launch(shot: Vector2) -> void:
	if _active == null or not is_instance_valid(_active) or shots_left <= 0:
		return
	if not _active.active:
		return
	var power := clampf(shot.length(), 0.0, 1.0)
	var velocity := shot.normalized() * max_shot_speed * power
	_active.at_rest = false
	_active.linear_velocity = velocity
	_active.sleeping = false
	last_shot_speed = velocity.length()
	shots_left -= 1
	chain = 0
	chain_this_shot = 0
	last_phrase = PackedInt32Array()
	crossing_time = 0.0
	shot_distance = 0.0
	shot_duration = 0.0
	shot_peak_speed = 0.0
	first_impact_time = -1.0
	first_impact_speed = 0.0
	shot_trace = PackedFloat32Array()
	_last_active_at = _active.position
	_crossing_pending = true
	_tracing = true
	_shot_live = true
	# The release itself is playable: land it on a downbeat and the whole shot starts
	# with the bonus already banked.
	if _music != null and _music.on_beat():
		likes += on_beat_bonus * multiplier
		on_beat_hits += 1


## --- What the rings report --------------------------------------------------------------

func _on_bounced(ring: Ring, _other: Node, speed: float) -> void:
	if speed < 8.0:
		# Resting contact, not an event. Paying for it would turn a settled pile into
		# the passive income this game does not have.
		return
	bounces += 1
	_last_event_at = Time.get_ticks_msec() / 1000.0
	likes += like_per_bounce * multiplier
	if _music != null:
		_music.play("bounce", ring.area, chain)
	# Only the shot's own first contact. Two field rings knocking together across the
	# pool used to set this, which reported an impact of 48 px/s while the striker was
	# still travelling at 275.
	if _tracing and first_impact_time < 0.0 and ring == _active:
		first_impact_time = shot_duration
		first_impact_speed = speed


func _on_absorbed(eater: Ring, eaten: Ring, speed: float) -> void:
	merges += 1
	_last_event_at = Time.get_ticks_msec() / 1000.0
	chain += 1
	chain_this_shot = chain
	best_chain = maxi(best_chain, chain)
	multiplier = 1.0 + float(chain) * chain_gain
	best_multiplier = maxf(best_multiplier, multiplier)
	likes += like_per_merge * float(chain) * multiplier
	if eaten.lit:
		lit_left = maxi(0, lit_left - 1)
	rings_in_play = maxi(0, rings_in_play - 1)
	if _tracing and first_impact_time < 0.0:
		first_impact_time = shot_duration
		first_impact_speed = speed
	if _music != null:
		# Pitch comes from the ring's new area, so a chain plays a descending run as the
		# active ring grows. That is the multiplier made audible.
		var midi := _music.midi_for_area(eater.area)
		_music.play("merge", eater.area, chain)
		_music.set_chain(chain)
		last_phrase.append(midi)
		if _music.on_beat():
			likes += on_beat_bonus * multiplier
			on_beat_hits += 1
	if lit_left == 0:
		set_cleared = true


func _on_came_to_rest(ring: Ring) -> void:
	if ring != _active or not _shot_live:
		return
	_shot_live = false
	shot_end_area = ring.area
	shot_end_distance = shot_distance
	# The shot has resolved: the active ring locks in as a passive one and bounces from
	# here on. Chain state stays readable until the next launch overwrites it.
	ring.active = false
	ring.queue_redraw()
	_tracing = false
	_recount()
	if set_cleared or shots_left <= 0:
		# Soft fail, as the brief asks. No lives, no restart: the survivors stay on the
		# table and the next set is laid out around them.
		_begin_set(false)
	else:
		_spawn_striker()


## --- Human input --------------------------------------------------------------------
## Drag back from the striker and release, billiards style. Both this and the property
## route above end in _launch, so what an agent asserts about is what a person does.

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			_aim_from = get_global_mouse_position()
			_aiming = true
		elif _aiming:
			_aiming = false
			var drag := _aim_from - get_global_mouse_position()
			if drag.length() > 12.0:
				_launch(drag.normalized() * clampf(drag.length() / 260.0, 0.0, 1.0))
	elif event is InputEventKey and event.keycode == KEY_SPACE:
		jet_held = event.pressed
