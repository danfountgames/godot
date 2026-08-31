extends CanvasLayer

## The four things a player has to be able to read without looking away from the ball:
## what they have scored, what the multiplier is doing, how much meter they are holding,
## and how many strikers are left.

@onready var game: PoolGame = get_parent()


func _process(_delta: float) -> void:
	$Stats/Points.text = "Likes  %d          x%0.2f" % [game.score, game.multiplier]
	$Stats/Shots.text = "Strikers  %s        Targets  %d  (%d anchored, %d loose)" % [
		"o".repeat(game.strikers_left), game.targets_left, game.anchored_left, game.loose_left]
	# The meter and what it will buy. Naming both costs is the point: they compete, and a
	# player who cannot see that they compete is not making the choice.
	var bar := int(round(game.meter / game.meter_max * 20.0))
	$Stats/Settled.text = "Meter [%s%s] %s%s" % [
		"#".repeat(bar), ".".repeat(20 - bar),
		"SHIELD " if game.meter >= game.shield_cost else "",
		"PARTY WAVE" if game.meter >= game.party_wave_cost else ""]
	var state := "shield %0.1fs" % game.shield_left if game.shield_active else game.last_event
	if game.drain_open:
		state = "DRAIN OPEN - %0.0fs" % maxf(0.0, game.drain_seconds - (game.quiet_seconds - game.stall_seconds))
	elif game.wave_lock_left > 0.0:
		state = "meter shut %0.1fs" % game.wave_lock_left
	# `likes in water` earns its place on a crowded line: on every board played without the
	# current, ninety to a hundred and thirty Likes sat uncollected and the interface never
	# mentioned it once. It is the single number that makes pull's value legible, and
	# without it a player can lose the whole reward economy and never learn that they have.
	$Stats/Feel.text = "threads %d   rim hits %d   ring hits %d   likes in water %d   %0.0fs   %s" % [
		game.threads, game.rim_hits, game.ring_impacts, game.likes_loose,
		game.board_seconds, state]
	for key in ["SlickerWater", "HeavierStriker", "ExtraShot", "JetPressure"]:
		var button := get_node_or_null("Upgrades/" + key) as Button
		if button != null:
			button.visible = false
	var title := get_node_or_null("Upgrades/UpgradeTitle") as Label
	if title != null:
		# The upgrade page belongs outside a board, not inside one. Left in the scene as
		# the place it will go rather than deleted and rebuilt.
		title.text = "COCKTAILS\n(between boards)"
