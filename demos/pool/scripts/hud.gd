extends CanvasLayer

## The counters, and the one upgrade page the first playable asks for.
##
## Every upgrade here changes behaviour you can watch in the physics or hear in the mix
## rather than a number on a sheet: slicker water changes how far a ring glides before
## the water grabs it, a heavier striker plows instead of ricocheting, cheaper merging
## drops the speed a contact needs to absorb rather than bounce. That is the brief's
## upgrade philosophy and it is also the only kind of upgrade this game can verify.

@onready var game: PoolGame = get_parent()

var costs := {
	"SlickerWater": 40.0,
	"HeavierStriker": 60.0,
	"ExtraShot": 120.0,
	"JetPressure": 50.0,
}
var bought := {}


func _process(_delta: float) -> void:
	$Stats/Points.text = "Likes  %0.0f" % game.likes
	$Stats/Shots.text = "Shots  %d / %d     Set %d" % [game.shots_left, game.shots_per_set, game.sets_played]
	$Stats/Settled.text = "Chain  x%d      Multiplier  x%0.2f      Lit left  %d" % [
		game.chain, game.multiplier, game.lit_left]
	# The chain as a phrase, which is what it is. A descending run of MIDI numbers is
	# the brief's central audio claim, and putting it on screen means it can be watched
	# where it cannot be heard.
	$Stats/Feel.text = "phrase  %s" % [", ".join(Array(game.last_phrase).map(func(n): return str(n)))]
	for key in costs:
		var button := get_node("Upgrades/" + key) as Button
		button.disabled = game.likes < _price(key)
		button.text = "%s  (%0.0f)" % [_label(key), _price(key)]


func _label(key: String) -> String:
	match key:
		"SlickerWater": return "Slicker Water x%d" % (1 + bought.get(key, 0))
		"HeavierStriker": return "Heavier Striker x%d" % (1 + bought.get(key, 0))
		"ExtraShot": return "Extra Shot x%d" % (1 + bought.get(key, 0))
		_: return "Jet Pressure x%d" % (1 + bought.get(key, 0))


func _price(key: String) -> float:
	return costs[key] * pow(1.7, bought.get(key, 0))


func _buy(key: String) -> bool:
	var price := _price(key)
	if game.likes < price:
		return false
	game.likes -= price
	bought[key] = bought.get(key, 0) + 1
	return true


## Applied to the rings that exist now *and* stored on the game so rings spawned later
## inherit it. Forgetting the second half is how an upgrade appears to work for one set
## and then silently stops.

func _on_slicker_water_pressed() -> void:
	if not _buy("SlickerWater"):
		return
	game.water_slickness *= 0.85
	game.apply_water_to_existing()


func _on_heavier_striker_pressed() -> void:
	if not _buy("HeavierStriker"):
		return
	game.striker_mass_scale *= 1.35


func _on_extra_shot_pressed() -> void:
	if not _buy("ExtraShot"):
		return
	game.shots_per_set += 1
	game.shots_left += 1


func _on_jet_pressure_pressed() -> void:
	if not _buy("JetPressure"):
		return
	var jet := game.get_node("Jet") as Jet
	jet.max_pressure *= 1.4
	jet.force *= 1.15
