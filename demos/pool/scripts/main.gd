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
@export var anchored_count: int = 26
@export var loose_count: int = 8
@export var heavy_count: int = 2
@export var armoured_count: int = 6
@export var popper_count: int = 1
@export var collector_count: int = 1

## The tail, and the drain that ends it.
##
## Measured over twenty boards: the median board ran 161 seconds and 79% of it was gone in
## the first thirty. The last quarter took 98 seconds - 5.7 times the first - and 71% of
## all board time had no scoring event in it at all, because it was spent hunting two or
## three survivors round the corners of an empty pool. One board went 210 consecutive
## seconds with nothing happening.
##
## So the pool drains. Once the board is down to its last few rings and nothing has been
## hit for a while, the survivors are cut loose and pulled towards the middle where the
## striker can reach them; if the quiet continues, the drain takes them and the board is
## over. It only arms in the tail, so it can never be waited out for a free clear.
@export var drain_threshold: int = 6
@export var stall_seconds: float = 8.0
@export var drain_seconds: float = 8.0
@export var drain_pull: float = 340.0

## The meter, and the two things that empty it.
@export var meter_max: float = 100.0
## Measured: at 4.0 the meter was full 3.7 to 11.2 seconds into a board and then sat at
## maximum for 95% of it, earning 488-564 charge against a tank of 100. Four fifths of
## every board's reward was thrown away, and the two things the meter buys never competed
## because there was always enough for both.
##
## 1.5 was derived against a 28-ring board, and then the board grew to 44. A ring scatters
## Likes in proportion to its size, so the supply went with it - 105 Likes a board became
## about 150, the meter was still at maximum for 84-100% of every board, and the choice
## was still not a choice. 0.9 puts a board's whole take at roughly 135 against a tank of
## 100: about one Party Wave, or three or four barriers, and never both.
@export var like_charge: float = 0.9
@export var shield_cost: float = 35.0
@export var shield_seconds: float = 2.5
@export var party_wave_cost: float = 100.0
@export var party_wave_speed: float = 900.0
## A wave used to pay for itself: it kills a board's worth of rings at once, every kill
## scatters Likes, the wave pulls them all to the lounger, and the meter came back from 4
## to 100 in 1.6 seconds. Spamming it cleared a board in 9.6 seconds. The wave was
## cheapest exactly when it was strongest, which is backwards, so the meter is shut for a
## few seconds afterwards and the wave's own debris does not refund it.
@export var wave_lockout_seconds: float = 3.0

## Threading pays mostly through the multiplier rather than through a flat bonus, so a
## streak is worth holding and a single thread is not a coin you picked up. Measured
## against the alternative: at a bonus of 25 and no working multiplier, threads were 31
## points each and half of every board's score, and the "trade a hit for a multiplier"
## the design describes was not a trade anybody ever made.
@export var thread_bonus: int = 10
@export var multiplier_per_thread: float = 0.25
@export var multiplier_max: float = 5.0

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
## Rings driven into each other hard enough to damage. The only readout that tells you
## whether push did anything.
var ring_impacts: int = 0
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
## Seconds since anything last scored. The single most useful number for judging pacing,
## and the thing the drain watches.
var quiet_seconds: float = 0.0
var drain_open: bool = false
var wave_lock_left: float = 0.0
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
## Whether this trip up the pool has threaded anything yet. The multiplier is spent by
## the *round trip*, not by the clock.
var _threaded_this_trip: bool = false
## The biggest ring the current grid can hold without the rack overlapping itself.
var _max_radius: float = 30.0
var _quiet: float = 0.0
var _drain_left: float = 0.0
var _wave_lock: float = 0.0
## Rings already threaded on this trip up the pool, by instance id.
var _threaded_rings: Dictionary = {}
## Everything a cocktail can change, as it was before any cocktail had.
var _defaults: Dictionary = {}

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
	_snapshot_defaults()
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
	drain_open = false
	_drain_left = 0.0
	_quiet = 0.0
	quiet_seconds = 0.0
	_wave_lock = 0.0
	wave_lock_left = 0.0
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
		ring_impacts = 0
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
	#
	# Nine by six rather than seven by four, because the grid was the real difficulty
	# ceiling. The plan was silently truncated to however many slots existed, so no export
	# on this script could put more than twenty-eight rings on a board; at peak the striker
	# kills about one a second, and a thirty-second head was baked into the geometry. The
	# rings shrink to suit, which is a fairer trade than a sparser board: a smaller ring is
	# a harder ring.
	var columns := 9
	var rows := 6
	var spacing := Vector2(84.0, 74.0)
	var span := Vector2(spacing.x * float(columns - 1), spacing.y * float(rows - 1))
	var margin := Vector2((_pool.size.x - span.x) * 0.5, 56.0)
	# Derived, never guessed. The last time these two numbers were set independently the
	# rack was born inside itself and the loose rings blew apart on frame one.
	_max_radius = minf(spacing.x, spacing.y) * 0.5 - 7.0

	# Jittered, and not every slot filled.
	#
	# A perfect grid has vertical corridors, and a dead-straight rebound returns the ball
	# to the same x for ever: held in one column the game produced eleven threads every
	# fifteen seconds, zero rim hits for two solid minutes, and a board frozen at nine
	# targets - 23 points a second at no risk, which is twice what playing properly pays.
	# The gaps also open a sightline to the back rows, which were otherwise always behind
	# a wall of other rings and reachable only by luck.
	var slots: Array[Vector2] = []
	for row in rows:
		for column in columns:
			slots.append(margin + Vector2(
					span.x * float(column) / float(columns - 1) + randf_range(-14.0, 14.0),
					span.y * float(row) / float(rows - 1) + randf_range(-10.0, 10.0)))
	slots.shuffle()
	slots.resize(maxi(8, slots.size() - 10))

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

	_trim_to_fit(plan, slots.size())
	plan.shuffle()
	for i in mini(plan.size(), slots.size()):
		_add_target(plan[i], slots[i])


## More rings asked for than there is room for, resolved by taking them off the crowd.
##
## This used to be a truncation of the plan, which is built one kind at a time - so
## `anchored_count: 60` gave twenty-eight anchored rings and *silently deleted every other
## kind on the board*. The specials are the board's variety and there are never many of
## them; the filler is what should give way.
func _trim_to_fit(p_plan: Array[Target.Kind], p_room: int) -> void:
	for expendable in [Target.Kind.ANCHORED, Target.Kind.LOOSE, Target.Kind.ARMOURED]:
		var i := p_plan.size() - 1
		while i >= 0 and p_plan.size() > p_room:
			if p_plan[i] == expendable:
				p_plan.remove_at(i)
			i -= 1
	while p_plan.size() > p_room:
		p_plan.remove_at(p_plan.size() - 1)


func _add_target(p_kind: Target.Kind, p_at: Vector2) -> Target:
	var node := RigidBody2D.new() as Node
	node.set_script(TARGET_SCRIPT)
	var target := node as Target
	target.kind = p_kind
	# A wider spread of sizes, and a third of them tight.
	#
	# Every ring used to be threadable and none of them looked any different, so "which
	# of these can I shoot through" - the question the whole mechanic asks - had the same
	# answer everywhere and no way to read it. A tight ring is a wall with a hole too
	# small for the striker, and Target draws the difference.
	# Sized against the grid rather than against a taste in numbers, so widening the board
	# cannot silently reintroduce the overlapping rack. Heavy rings take the top of the
	# range: they are supposed to read as the thing in the way.
	target.outer_radius = _max_radius if p_kind == Target.Kind.HEAVY \
			else randf_range(_max_radius * 0.72, _max_radius)
	target.tight = randf() < 0.34
	target.position = p_at
	_serial += 1
	target.name = "%s%d" % [Target.Kind.keys()[p_kind].capitalize(), _serial]
	target.destroyed.connect(_on_target_destroyed)
	target.struck.connect(_on_struck)
	target.rim_hit.connect(_on_rim_hit)
	target.threaded.connect(_on_threaded)
	target.collided.connect(_on_rings_collided)
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
	# The top edge of the lounger, not its centre. Aimed at the centre, pull steered the
	# ball at a point 22px *below* the surface it has to land on, and measurably pulled
	# grazing balls under the end of the lounger that no current at all would have saved.
	_current.origin = _paddle.global_position - Vector2(0.0, _paddle.thickness)
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

	if _wave_lock > 0.0:
		_wave_lock = maxf(0.0, _wave_lock - delta)
	wave_lock_left = _wave_lock

	if striker_in_play:
		board_seconds += delta

	_apply_current(delta)
	_sweep_lost_targets()
	_watch_striker()
	_collect_likes()
	_recount()
	_run_drain(delta)


## The end of a board, taken out of the player's hands once it stops being a game.
##
## Arms only in the tail - `drain_threshold` rings or fewer - so it can never be waited
## out for a free clear of a full board, and only while a striker is actually in play.
## Two stages, because the first one is still a game: the survivors are cut loose and
## dragged into the middle where they can be reached, and the player gets `drain_seconds`
## to take them. Miss that and the pool takes them, at full value, because they were
## survived rather than skipped.
func _run_drain(delta: float) -> void:
	if board_cleared or board_failed or run_over or not striker_in_play:
		return
	_quiet += delta
	quiet_seconds = _quiet
	if not drain_open:
		if targets_left <= drain_threshold and targets_left > 0 and _quiet >= stall_seconds:
			drain_open = true
			_drain_left = drain_seconds
			last_event = "the drain opens"
			for child in _targets.get_children():
				var loosened := child as Target
				if loosened != null and loosened.alive:
					loosened.release()
			if _music != null:
				_music.play("layer", 20000.0, targets_left)
		return

	var mouth := Vector2(_pool.size.x * 0.5, _pool.size.y * 0.62)
	for child in _targets.get_children():
		var target := child as Target
		if target == null or not target.alive:
			continue
		target.apply_current((mouth - target.global_position).normalized() * drain_pull * delta)
	_drain_left -= delta
	if _drain_left > 0.0:
		return
	last_event = "drained"
	for child in _targets.get_children():
		var last := child as Target
		if last != null and last.alive:
			last.take_rim_hit(last.global_position, 99)


## Something scored. The drain is watching this and nothing else, because "is anything
## happening" is a question about events, not about whether objects still exist.
func _stir() -> void:
	_quiet = 0.0
	quiet_seconds = 0.0


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
			# Not progress. Measured: holding pull with no ball in play at all removed five
			# of twenty-eight targets in five seconds and banked 154 points, because a ring
			# dragged out of the open edge was deleted and counted as cleared. A ring you
			# let out of the pool is a ring you did not break, so it costs a step of the
			# streak and pays nothing.
			multiplier = maxf(1.0, multiplier - multiplier_per_thread)
			last_event = "%s went down the drain" % target.name
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
		# Where it *crossed* the line, not where it happens to be this frame.
		#
		# The ball travels 7.2px per physics step, so resolving the rebound at the
		# post-step position quantised the contact point by up to that much - against a
		# game that needs 1.5 to 9.3px of accuracy to put a shot through a hole. Measured:
		# deliberate aiming threaded the ring it aimed at 17% of the time, which is
		# *below* the 37-49% chance rate, and aimed play scored no better than random.
		# Aiming was not hard, it was not connected. Back-projecting along the velocity
		# to the exact crossing costs one division and gives the player their skill back.
		var line := paddle_top - _striker.radius
		var overshoot := (_striker.position.y - line) / maxf(1.0, _striker.linear_velocity.y)
		var contact := Vector2(_striker.position.x - _striker.linear_velocity.x * overshoot, line)
		var across := absf(contact.x - _paddle.position.x)
		if across <= _paddle.width * 0.5 + _striker.radius:
			# Aimed by where it lands on the lounger; the incoming angle is thrown away.
			_striker.linear_velocity = _paddle.rebound_direction(contact) * _striker.speed
			_striker.position = Vector2(contact.x, line - 1.0)
			_on_returned_to_lounger()
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
		_threaded_this_trip = false
		_threaded_rings.clear()
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
			if _wave_lock <= 0.0:
				meter = minf(meter_max, meter + like_charge)
			_stir()
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
	_stir()
	if _music != null:
		_music.play("bounce", 2400.0, int(multiplier))
	striker.deflect_from(target.global_position)
	if not target.freeze:
		# A loose ring takes the shot as well as the damage, which is how the striker
		# rearranges the board rather than just clearing it.
		target.apply_current((target.global_position - striker.global_position).normalized() * 260.0)
	target.take_rim_hit(striker.global_position)


## One ring driven into another. Push's whole payoff, and until now it did not exist: the
## signal was never emitted, so holding push against a full board for forty-five seconds
## destroyed nothing and moved nothing off. It scores less than a struck rim because the
## striker is not the one doing the work - what push buys is a board rearranged, not
## points.
func _on_rings_collided(hitter: Target, hit: Target, speed: float) -> void:
	if not hitter.alive or not hit.alive:
		return
	ring_impacts += 1
	score += int(round(multiplier))
	_stir()
	if _music != null:
		_music.play("bounce", hit.outer_radius * hit.outer_radius * PI, 1)
	last_event = "%s slammed into %s at %d" % [hitter.name, hit.name, int(speed)]
	hit.take_rim_hit(hit.global_position)


func _on_rim_hit(_target: Target, _at: Vector2, _remaining: int) -> void:
	# An armoured ring surviving a hit. Scored by _on_rim_struck already; this exists so
	# the survivor can be told apart from the kill in a trace.
	pass


## The striker is back. One thread a trip keeps the streak; a trip that threaded nothing
## costs a step.
##
## This replaces a decay on a timer, which was measurable nonsense: break-even was one
## thread every 0.71s against a measured mean gap of 10.8s, so the multiplier spent 0.1%
## of nine boards above 1.5 and was never once above 1.7. A number that is displayed,
## always 1.0, and provably unreachable teaches the player to stop reading it. Per trip
## is a rule you can play to.
func _on_returned_to_lounger() -> void:
	if _threaded_this_trip:
		last_event = "streak held at x%0.2f" % multiplier
	else:
		multiplier = maxf(1.0, multiplier - multiplier_per_thread)
	_threaded_this_trip = false
	# A fresh trip, so every ring is worth threading again. Held between rebounds, this is
	# what closes the pin-and-repeat exploit: a striker parked near the top edge threaded
	# the *same* rings 494 times for 15,454 points - seventeen times what clearing a board
	# properly pays - while destroying ten targets of twenty-eight. Threading a ring you
	# already threaded on this trip is a trick, not an aim.
	_threaded_rings.clear()


func _on_threaded(target: Target) -> void:
	# Through the middle without touching the rim. The one thing in this game that is
	# purely skill, so it is the one thing that builds the multiplier.
	#
	# Once per ring per trip. Threading a chain of different rings on one pass is the
	# best thing you can do here and it still pays every time; threading the same ring
	# over and over from a ball you have pinned in a corner is not that.
	if _threaded_rings.has(target.get_instance_id()):
		last_event = "threaded %s again - no score" % target.name
		return
	_threaded_rings[target.get_instance_id()] = true
	threads += 1
	_stir()
	if _striker != null and is_instance_valid(_striker):
		_striker.threads_this_life += 1
	multiplier = minf(multiplier_max, multiplier + multiplier_per_thread)
	_threaded_this_trip = true
	score += int(round(thread_bonus * multiplier))
	if _wave_lock <= 0.0:
		meter = minf(meter_max, meter + like_charge)
	last_event = "threaded %s" % target.name
	if _music != null:
		# Its own voice. A thread and a kill of the same ring both played "merge" pitched
		# by area, which is the same note - so the one moment the game should celebrate
		# sounded exactly like the ordinary one.
		_music.play("thread", target.outer_radius * target.outer_radius * PI, threads)
		_music.set_chain(threads)


func _on_target_destroyed(target: Target, at: Vector2, worth: int) -> void:
	targets_destroyed += 1
	score += int(round(worth * multiplier))
	_stir()
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
		# Scaled by the multiplier, so the streak is worth carrying into the last ring
		# rather than abandoned once the board is nearly done.
		score += int(round((250 * boards_cleared + 120 * strikers_left) * multiplier))
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


## Everything a cocktail can move, remembered before any of them has. A run that quietly
## inherited the last run's upgrades would look like a difficulty curve and be a bug.
##
## Snapshotted rather than written out a second time. The hand-written version drifted
## twice - it was still restoring a 62-degree bounce angle and a thread bonus of 25 after
## both had been tuned away - so restarting a run silently played a slightly different
## game from the one that had just been measured. A list of defaults kept in two places is
## a list of defaults kept in one place and a bug.
func _snapshot_defaults() -> void:
	_defaults = {
		"force": _current.force,
		"steering": _current.steering,
		"width": _paddle.width,
		"max_bounce_degrees": _paddle.max_bounce_degrees,
		"shield_seconds": shield_seconds,
		"thread_bonus": thread_bonus,
		"multiplier_per_thread": multiplier_per_thread,
		"strikers_per_board": strikers_per_board,
		"party_wave_cost": party_wave_cost,
		"meter_max": meter_max,
	}


func _reset_cocktail_effects() -> void:
	if _defaults.is_empty():
		return
	_current.force = _defaults["force"]
	_current.steering = _defaults["steering"]
	_paddle.width = _defaults["width"]
	_paddle.max_bounce_degrees = _defaults["max_bounce_degrees"]
	_paddle.rebuild()
	_paddle.set_limits(0.0, _pool.size.x)
	shield_seconds = _defaults["shield_seconds"]
	thread_bonus = _defaults["thread_bonus"]
	multiplier_per_thread = _defaults["multiplier_per_thread"]
	strikers_per_board = _defaults["strikers_per_board"]
	party_wave_cost = _defaults["party_wave_cost"]
	meter_max = _defaults["meter_max"]


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
	# Shut the meter. Everything below kills a board's worth of rings at once, every kill
	# scatters Likes, and the wave then hoovers them all into the lounger - so the wave
	# bought itself back in 1.6 seconds and a board could be cleared in 9.6 by pressing
	# this repeatedly. A wave has to be paid for out of play that came before it.
	_wave_lock = wave_lockout_seconds
	wave_lock_left = _wave_lock
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


## Built into locals and published in one go.
##
## Clearing the counters and refilling them in place meant a reader could catch the
## middle of the rebuild: `targets_left` came back as 1 when the truth was 21, about once
## in a hundred reads, which faked a cleared board and corrupted a whole measurement run.
## Anything a tool can read has to be assigned, never accumulated in public.
func _recount() -> void:
	var count := 0
	var anchored := 0
	var loose := 0
	var targets := PackedVector2Array()
	var loosies := PackedVector2Array()
	for child in _targets.get_children():
		var target := child as Target
		if target == null or not target.alive or target.is_queued_for_deletion():
			continue
		count += 1
		targets.append(target.position)
		if target.freeze:
			anchored += 1
		else:
			loose += 1
			loosies.append(target.position)
	var likes := 0
	var like_at := PackedVector2Array()
	for child in _likes.get_children():
		if child is Like:
			likes += 1
			like_at.append((child as Like).position)
	targets_left = count
	anchored_left = anchored
	loose_left = loose
	target_positions = targets
	loose_positions = loosies
	likes_loose = likes
	like_positions = like_at
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
