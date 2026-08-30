class_name PoolGame
extends Node2D

## POOL — a breakout game played with rubber rings, where you control the pool's current.
##
## The loop: move the lounger along the near edge, rebound the striker into the rings,
## and hold pull or push to bend it, vacuum up Likes, and shove the loose floats about.
## Clear every target before you run out of strikers. The meter you fill from Likes buys
## either a splash barrier or a Party Wave, and it will not buy both.
##
## What this deliberately is **not** any more: the shot-based settling game. There are no
## turns, nothing waits for the water to still, and nothing on the board pays out for
## sitting there. Those belong outside a level, if anywhere.
##
## Two rules carry the identity, and both are geometry rather than special cases:
##
##   * **The striker does not obey the water.** Everything else drifts and settles; the
##     ball holds its speed and the current only steers it. A breakout ball with drag is
##     a ball that stops halfway up the board.
##   * **The rings have real holes.** The rim is a ring of colliders and the middle is
##     empty, so a small enough striker threads a big enough ring under ordinary physics.
##     Threading is not a feature; it is what honest geometry does.
##
## Every verb is reachable as a property as well as by input, because a drag is a bad unit
## of intent to assert about:
##
##   paddle_x            where the lounger is going
##   current             -1 full pull .. +1 full push, held
##   shield_requested    spend meter on the barrier
##   party_wave_requested spend meter on the wave
##   launch_requested    let the waiting striker go
##   restart_requested   rebuild the board

@export var strikers_per_board: int = 3
@export var anchored_count: int = 16
@export var loose_count: int = 6
@export var heavy_count: int = 2
@export var armoured_count: int = 2
@export var popper_count: int = 1
@export var collector_count: int = 1

## The meter, and the two things that empty it.
@export var meter_max: float = 100.0
@export var like_charge: float = 4.0
@export var shield_cost: float = 35.0
@export var shield_seconds: float = 2.5
@export var party_wave_cost: float = 100.0
@export var party_wave_speed: float = 900.0

@export var thread_bonus: int = 25
@export var multiplier_per_thread: float = 0.25
@export var multiplier_decay: float = 0.35

## The cocktails: the run's upgrades, taken one per cleared board.
##
## Every one changes something you can watch happen rather than a number on a sheet,
## which is the same rule the targets follow. Three are offered and one is taken, so a
## run is a sequence of small refusals as much as of choices.
const COCKTAILS := {
	"stronger_current": "Rip Tide: the current pulls and steers half again as hard",
	"wider_lounger": "Double Lounger: a wider paddle, and a wider angle off the ends",
	"longer_shield": "Slow Set: the splash barrier lasts twice as long",
	"sharper_threads": "Needle: threading is worth more and builds the multiplier faster",
	"extra_striker": "Spare Ring: one more striker every board",
	"cheap_wave": "Happy Hour: the Party Wave costs a third less",
	"deep_pockets": "Big Glass: a bigger meter, so a wave can be banked while you defend",
}

## --- The verbs -----------------------------------------------------------------------
var paddle_x: float = 0.0
var current: float = 0.0
var shield_requested: bool = false
var party_wave_requested: bool = false
var launch_requested: bool = false
var restart_requested: bool = false
## Take one of `cocktail_offer` by name. That is also what starts the next board, so the
## pause between boards ends when the player has decided rather than on a timer.
var cocktail_choice: String = ""

## --- What a playtester reads ------------------------------------------------------
var score: int = 0
var multiplier: float = 1.0
var meter: float = 0.0
var strikers_left: int = 0
var targets_left: int = 0
var anchored_left: int = 0
var loose_left: int = 0
var likes_loose: int = 0
var likes_collected: int = 0
var threads: int = 0
var rim_hits: int = 0
var targets_destroyed: int = 0
var board_cleared: bool = false
var board_failed: bool = false
var boards_played: int = 0
## --- The run --------------------------------------------------------------------------
## A board on its own is a test of control with nothing at stake. The run is what makes
## losing a striker cost something, and it is where the brief's incremental half lives -
## outside the board, never inside it.
var boards_cleared: int = 0
var run_over: bool = false
var choosing_cocktail: bool = false
var cocktail_offer: PackedStringArray = PackedStringArray()
var cocktails_taken: PackedStringArray = PackedStringArray()
var shield_active: bool = false
var shield_left: float = 0.0
var party_waves: int = 0
var striker_in_play: bool = false
var striker_position: Vector2 = Vector2.ZERO
var striker_speed_now: float = 0.0
var paddle_position: Vector2 = Vector2.ZERO
var target_positions: PackedVector2Array = PackedVector2Array()
var loose_positions: PackedVector2Array = PackedVector2Array()
var like_positions: PackedVector2Array = PackedVector2Array()
## Seconds since the board started. A breakout level is meant to last one to three
## minutes; nothing else here would tell you whether it does.
var board_seconds: float = 0.0
var last_event: String = ""

var _pool: Pool
var _paddle: Paddle
var _current: PoolCurrent
var _music: PoolMusic
var _targets: Node2D
var _strikers: Node2D
var _likes: Node2D
var _striker: Striker
var _serial: int = 0

const TARGET_SCRIPT := preload("res://scripts/target.gd")
const STRIKER_SCRIPT := preload("res://scripts/striker.gd")
const LIKE_SCRIPT := preload("res://scripts/like.gd")


func _ready() -> void:
	_pool = $Pool
	_paddle = $Paddle
	_current = $Current
	_music = $Music
	_targets = $Targets
	_strikers = $Strikers
	_likes = $Likes
	randomize()
	_paddle.position = Vector2(_pool.size.x * 0.5, _pool.size.y - 34.0)
	_paddle.set_limits(0.0, _pool.size.x)
	paddle_x = _paddle.position.x
	_begin_board(true)


## --- Building a board ---------------------------------------------------------------

func _begin_board(p_fresh: bool) -> void:
	for group in [_targets, _strikers, _likes]:
		for child in group.get_children():
			group.remove_child(child)
			child.queue_free()
	_serial = 0
	_striker = null
	striker_in_play = false
	board_cleared = false
	board_failed = false
	board_seconds = 0.0
	strikers_left = strikers_per_board
	shield_active = false
	shield_left = 0.0
	if p_fresh:
		# A fresh *run*, not merely a fresh board. Everything the run accumulates resets
		# here and nowhere else, so clearing a board keeps what it earned.
		score = 0
		boards_cleared = 0
		run_over = false
		choosing_cocktail = false
		cocktail_offer = PackedStringArray()
		cocktails_taken = PackedStringArray()
		_reset_cocktail_effects()
		meter = 0.0
		multiplier = 1.0
		threads = 0
		rim_hits = 0
		targets_destroyed = 0
		likes_collected = 0
		party_waves = 0
		boards_played = 0
		if _music != null:
			_music.reset_log()
	boards_played += 1
	_lay_out_board()
	_serve()
	_recount()


## Roughly two thirds anchored, a fifth loose, the rest special.
##
## The mix is the readability rule. A board where everything drifts has no state a player
## can read - the remaining targets *are* the level - so the structure has to be mostly
## fixed, with enough loose objects to make the current worth holding.
## How much harder board N is than board one.
##
## Only two things climb: how much there is, and how much of it takes more than one hit.
## Speeding the ball up or shrinking the lounger would make a later board a *different*
## game rather than a harder one, and the player has spent the whole run learning this one.
func _difficulty() -> int:
	return boards_cleared


func _lay_out_board() -> void:
	# Spacing first, count second. The slots have to be further apart than the biggest
	# ring is wide or the board is born overlapping - which does not look like a layout
	# mistake, it looks like the physics exploding, because that is what happens next.
	var columns := 7
	var rows := 4
	var spacing := Vector2(104.0, 94.0)
	var span := Vector2(spacing.x * float(columns - 1), spacing.y * float(rows - 1))
	var margin := Vector2((_pool.size.x - span.x) * 0.5, 70.0)

	var slots: Array[Vector2] = []
	for row in rows:
		for column in columns:
			slots.append(margin + Vector2(
					span.x * float(column) / float(columns - 1),
					span.y * float(row) / float(rows - 1)))
	slots.shuffle()

	var harder := _difficulty()
	var plan: Array[Target.Kind] = []
	for i in anchored_count + harder:
		plan.append(Target.Kind.ANCHORED)
	for i in loose_count + int(harder / 2.0):
		plan.append(Target.Kind.LOOSE)
	for i in heavy_count:
		plan.append(Target.Kind.HEAVY)
	for i in armoured_count + harder:
		plan.append(Target.Kind.ARMOURED)
	for i in popper_count:
		plan.append(Target.Kind.POPPER)
	for i in collector_count:
		plan.append(Target.Kind.COLLECTOR)

	for i in mini(plan.size(), slots.size()):
		_add_target(plan[i], slots[i])


func _add_target(p_kind: Target.Kind, p_at: Vector2) -> Target:
	var node := RigidBody2D.new() as Node
	node.set_script(TARGET_SCRIPT)
	var target := node as Target
	target.kind = p_kind
	# A spread of sizes, because threading is only interesting when some rings are big
	# enough to go through and some are not.
	target.outer_radius = randf_range(28.0, 46.0)
	target.position = p_at
	_serial += 1
	target.name = "%s%d" % [Target.Kind.keys()[p_kind].capitalize(), _serial]
	target.destroyed.connect(_on_target_destroyed)
	target.struck.connect(_on_struck)
	target.rim_hit.connect(_on_rim_hit)
	target.threaded.connect(_on_threaded)
	_targets.add_child(target)
	return target


## A striker waiting on the lounger. It does not move until it is launched, so a board
## always opens with the player deciding when to start.
func _serve() -> void:
	if strikers_left <= 0:
		board_failed = true
		last_event = "out of strikers"
		return
	var node := RigidBody2D.new() as Node
	node.set_script(STRIKER_SCRIPT)
	_striker = node as Striker
	_striker.name = "Striker%d" % strikers_left
	_striker.position = _paddle.position + Vector2(0, -26)
	_strikers.add_child(_striker)
	striker_in_play = false


func _launch() -> void:
	if _striker == null or not is_instance_valid(_striker) or _striker.launched:
		return
	_striker.launch(Vector2.UP.rotated(randf_range(-0.35, 0.35)))
	striker_in_play = true
	last_event = "launched"


## --- The loop --------------------------------------------------------------------------

func _physics_process(delta: float) -> void:
	if restart_requested:
		restart_requested = false
		_begin_board(true)
		return

	if not cocktail_choice.is_empty():
		var wanted := cocktail_choice
		cocktail_choice = ""
		_take_cocktail(wanted)
		return

	if choosing_cocktail or run_over:
		# Between boards, or the run is finished. Nothing moves and nothing scores; the
		# only thing that happens next is a decision.
		return

	_paddle.wanted_x = paddle_x
	_current.strength = clampf(current, -1.0, 1.0)
	_current.origin = _paddle.global_position
	_current.queue_redraw()

	if launch_requested:
		launch_requested = false
		_launch()

	if shield_requested:
		shield_requested = false
		_raise_shield()
	if party_wave_requested:
		party_wave_requested = false
		_release_party_wave()

	if shield_active:
		shield_left = maxf(0.0, shield_left - delta)
		shield_active = shield_left > 0.0

	if striker_in_play:
		board_seconds += delta

	_apply_current(delta)
	_sweep_lost_targets()
	_watch_striker()
	_collect_likes()
	# The multiplier is a thing you are holding, not a thing you have. It bleeds back to
	# one whenever you are not threading, so a streak has to be maintained.
	multiplier = maxf(1.0, multiplier - multiplier_decay * delta)
	_recount()


func _apply_current(delta: float) -> void:
	if is_zero_approx(_current.strength):
		return
	if _striker != null and is_instance_valid(_striker):
		_current.steer_striker(_striker, delta)
	for child in _targets.get_children():
		var target := child as Target
		if target != null and target.alive:
			target.apply_current(_current.impulse_for(target.global_position, 1.0, delta))
	for child in _likes.get_children():
		var like := child as Like
		if like != null:
			like.apply_current(_current.impulse_for(like.global_position, 1.0, delta))


## The striker's relationship with the bottom edge, which is the only way to lose.
## A loose ring pulled out of the open edge is out of the game. Removed rather than left
## drifting below the pool, where it would keep counting towards "targets left" and make
## the board unclearable - the failure this most resembles is a level that cannot be
## finished for reasons off screen.
func _sweep_lost_targets() -> void:
	for child in _targets.get_children():
		var target := child as Target
		if target == null or not target.alive or target.freeze:
			continue
		if target.position.y > _pool.size.y + 60.0:
			target.alive = false
			target.queue_free()
			last_event = "%s drifted out" % target.name
			call_deferred("_check_cleared")


func _watch_striker() -> void:
	if _striker == null or not is_instance_valid(_striker):
		return
	striker_position = _striker.global_position
	striker_speed_now = _striker.linear_velocity.length()
	paddle_position = _paddle.global_position
	if not _striker.launched:
		# Riding the lounger until it is let go.
		_striker.position = _paddle.position + Vector2(0, -26)
		return

	var paddle_top := _paddle.position.y - _paddle.thickness
	if _striker.position.y >= paddle_top - _striker.radius and _striker.linear_velocity.y > 0.0:
		var across := absf(_striker.position.x - _paddle.position.x)
		if across <= _paddle.width * 0.5 + _striker.radius:
			# The rebound is aimed by where it lands on the lounger, and the incoming
			# angle is thrown away. That is what makes the paddle a thing you aim with.
			_striker.linear_velocity = _paddle.rebound_direction(_striker.global_position) * _striker.speed
			_striker.position.y = paddle_top - _striker.radius - 1.0
			if _music != null:
				_music.play("bounce", 2400.0, int(multiplier))
			last_event = "rebound"
			return
		if shield_active:
			# The barrier is a second paddle across the whole edge, and it is why pulling
			# a dangerous pile towards yourself can be the right play.
			_striker.linear_velocity = Vector2(_striker.linear_velocity.x, -absf(_striker.linear_velocity.y)).normalized() * _striker.speed
			last_event = "shielded"
			return

	if _striker.position.y > _pool.size.y + 40.0:
		strikers_left -= 1
		last_event = "striker lost"
		multiplier = 1.0
		_striker.queue_free()
		_striker = null
		striker_in_play = false
		if strikers_left <= 0:
			board_failed = true
			run_over = true
			last_event = "run over after %d board(s) cleared" % boards_cleared
		else:
			_serve()


func _collect_likes() -> void:
	for child in _likes.get_children():
		var like := child as Like
		if like == null:
			continue
		if like.global_position.distance_to(_paddle.global_position) <= _paddle.width * 0.5 + 18.0:
			likes_collected += like.worth
			score += int(round(like.worth * multiplier))
			meter = minf(meter_max, meter + like_charge)
			like.queue_free()
		elif like.global_position.y > _pool.size.y + 30.0:
			# Reward that reached the edge is gone. Pull is how you stop that happening,
			# and pull is what brings the floats down at you.
			like.queue_free()


## --- What the board reports -------------------------------------------------------------

func _on_struck(target: Target, striker: Striker) -> void:
	# Counted here rather than in _on_rim_hit, which only fires for a hit the ring
	# survived - so a board of one-hit rings reported that nothing had ever been hit.
	rim_hits += 1
	score += int(round(2 * multiplier))
	if _music != null:
		_music.play("bounce", 2400.0, int(multiplier))
	striker.deflect_from(target.global_position)
	if not target.freeze:
		# A loose ring takes the shot as well as the damage, which is how the striker
		# rearranges the board rather than just clearing it.
		target.apply_current((target.global_position - striker.global_position).normalized() * 260.0)
	target.take_rim_hit(striker.global_position)


func _on_rim_hit(_target: Target, _at: Vector2, _remaining: int) -> void:
	# An armoured ring surviving a hit. Scored by _on_rim_struck already; this exists so
	# the survivor can be told apart from the kill in a trace.
	pass


func _on_threaded(target: Target) -> void:
	# Through the middle without touching the rim. The one thing in this game that is
	# purely skill, so it is the one thing that builds the multiplier.
	threads += 1
	if _striker != null and is_instance_valid(_striker):
		_striker.threads_this_life += 1
	multiplier += multiplier_per_thread
	score += int(round(thread_bonus * multiplier))
	meter = minf(meter_max, meter + like_charge)
	last_event = "threaded %s" % target.name
	if _music != null:
		_music.play("merge", target.outer_radius * target.outer_radius * PI, threads)


func _on_target_destroyed(target: Target, at: Vector2, worth: int) -> void:
	targets_destroyed += 1
	score += int(round(worth * multiplier))
	if _music != null:
		_music.play("merge", target.outer_radius * target.outer_radius * PI, int(multiplier))
	if target.kind == Target.Kind.POPPER:
		_pop_neighbours(at, target)
	_scatter_likes(at, 2 + int(target.outer_radius / 12.0))
	target.queue_free()
	last_event = "destroyed %s" % target.name
	call_deferred("_check_cleared")


func _pop_neighbours(p_at: Vector2, p_source: Target) -> void:
	for child in _targets.get_children():
		var other := child as Target
		if other == null or other == p_source or not other.alive:
			continue
		if other.global_position.distance_to(p_at) <= 96.0:
			other.take_rim_hit(other.global_position)


func _scatter_likes(p_at: Vector2, p_count: int) -> void:
	for i in p_count:
		var node := RigidBody2D.new() as Node
		node.set_script(LIKE_SCRIPT)
		var like := node as Like
		like.position = p_at + Vector2(randf_range(-14, 14), randf_range(-14, 14))
		_likes.add_child(like)
		like.linear_velocity = Vector2(randf_range(-110, 110), randf_range(-110, 110))


func _check_cleared() -> void:
	_recount()
	if targets_left == 0 and not board_cleared:
		board_cleared = true
		boards_cleared += 1
		# A cleared board pays for the strikers you did not spend. Surviving has to be
		# worth something on its own, or the careful way to play is worth nothing.
		score += 250 * boards_cleared + 120 * strikers_left
		last_event = "board %d cleared" % boards_cleared
		if _music != null:
			_music.play("layer", 30000.0, boards_cleared)
		_offer_cocktails()


## --- Between boards -----------------------------------------------------------------
## Three offered, one taken. The board stops while it is being decided: this is the only
## moment in the game that is not real time, and it is the only one that should be.
func _offer_cocktails() -> void:
	var available: Array[String] = []
	for key in COCKTAILS:
		if not cocktails_taken.has(key):
			available.append(key)
	available.shuffle()
	cocktail_offer = PackedStringArray()
	for i in mini(3, available.size()):
		cocktail_offer.append(available[i])
	if cocktail_offer.is_empty():
		# Everything taken already: nothing to decide, so do not stop for it.
		_next_board()
		return
	choosing_cocktail = true


func _take_cocktail(p_name: String) -> void:
	if not choosing_cocktail or not cocktail_offer.has(p_name):
		last_event = "cocktail '%s' is not on offer" % p_name
		return
	cocktails_taken.append(p_name)
	match p_name:
		"stronger_current":
			_current.force *= 1.5
			_current.steering *= 1.5
		"wider_lounger":
			_paddle.width += 34.0
			_paddle.max_bounce_degrees += 6.0
			_paddle.rebuild()
			_paddle.set_limits(0.0, _pool.size.x)
		"longer_shield":
			shield_seconds *= 2.0
		"sharper_threads":
			thread_bonus += 20
			multiplier_per_thread += 0.15
		"extra_striker":
			strikers_per_board += 1
		"cheap_wave":
			party_wave_cost = maxf(20.0, party_wave_cost - 34.0)
		"deep_pockets":
			meter_max += 60.0
	choosing_cocktail = false
	cocktail_offer = PackedStringArray()
	last_event = "took %s" % p_name
	_next_board()


## Everything a cocktail moved, put back. A run that quietly inherited the last run's
## upgrades would look like a difficulty curve and be a bug.
func _reset_cocktail_effects() -> void:
	if _current != null:
		_current.force = 900.0
		_current.steering = 1.0
	if _paddle != null:
		_paddle.width = 132.0
		_paddle.max_bounce_degrees = 62.0
		_paddle.rebuild()
		if _pool != null:
			_paddle.set_limits(0.0, _pool.size.x)
	shield_seconds = 2.5
	thread_bonus = 25
	multiplier_per_thread = 0.25
	strikers_per_board = 3
	party_wave_cost = 100.0
	meter_max = 100.0


func _next_board() -> void:
	_begin_board(false)


## --- The meter ---------------------------------------------------------------------------
## One meter, two ways to spend it, and they compete. Save for the wave or stay alive; the
## game is not interesting if you can do both.

func _raise_shield() -> void:
	if meter < shield_cost or shield_active:
		last_event = "shield refused"
		return
	meter -= shield_cost
	shield_active = true
	shield_left = shield_seconds
	last_event = "shield up"


func _release_party_wave() -> void:
	if meter < party_wave_cost:
		last_event = "party wave refused"
		return
	meter -= party_wave_cost
	party_waves += 1
	last_event = "party wave"
	if _music != null:
		_music.play("layer", 40000.0, int(multiplier))
	# A wall of water up the pool: everything loose is thrown at the far end and every
	# target in its path takes a hit. It is the offensive half of the meter, and the
	# reason not to spend it on a barrier.
	for child in _targets.get_children():
		var target := child as Target
		if target == null or not target.alive:
			continue
		target.apply_current(Vector2.UP * party_wave_speed)
		target.take_rim_hit(target.global_position)
	for child in _likes.get_children():
		var like := child as Like
		if like != null:
			# Likes come *to* you on a wave rather than away, or the wave would throw its
			# own reward off the board.
			like.apply_central_impulse((_paddle.global_position - like.global_position).normalized() * 260.0)


func _recount() -> void:
	targets_left = 0
	anchored_left = 0
	loose_left = 0
	target_positions = PackedVector2Array()
	loose_positions = PackedVector2Array()
	for child in _targets.get_children():
		var target := child as Target
		if target == null or not target.alive or target.is_queued_for_deletion():
			continue
		targets_left += 1
		target_positions.append(target.position)
		if target.freeze:
			anchored_left += 1
		else:
			loose_left += 1
			loose_positions.append(target.position)
	likes_loose = 0
	like_positions = PackedVector2Array()
	for child in _likes.get_children():
		if child is Like:
			likes_loose += 1
			like_positions.append((child as Like).position)
	if _paddle != null:
		paddle_position = _paddle.global_position


## --- Human input --------------------------------------------------------------------
## Left and right move the lounger, the mouse aims it, holding the two triggers works the
## current, space launches. All of it lands on the same properties an agent sets.

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion:
		paddle_x = get_global_mouse_position().x
	elif event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			current = -1.0 if event.pressed else 0.0
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			current = 1.0 if event.pressed else 0.0
	elif event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_SPACE:
				launch_requested = true
			KEY_SHIFT:
				shield_requested = true
			KEY_ENTER:
				party_wave_requested = true


func _process(delta: float) -> void:
	# Held keys, read rather than evented, because moving the paddle is continuous.
	if Input.is_key_pressed(KEY_LEFT):
		paddle_x -= 620.0 * delta
	if Input.is_key_pressed(KEY_RIGHT):
		paddle_x += 620.0 * delta
