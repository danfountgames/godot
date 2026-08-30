class_name PoolCurrent
extends Node2D

## Pull and push, radiating from the paddle. The one continuous verb in the game.
##
## It does four different jobs at once, and that is what makes it more than a gimmick:
##
##   pull   bends the striker back towards you, vacuums Likes into range, and drags the
##          loose rings down towards the edge you are defending
##   push   bends the striker away, opens formations, drives loose rings into anchored
##          ones, and keeps the dangerous floats off you
##
## So pull gathers reward and brings danger with it; push buys safety and sends the
## reward away. Neither is the safe choice, which is the point.

## -1 is full pull, +1 is full push, 0 is still water. Set it and hold it; this is the
## verb, and a person's trigger ends up here.
@export var strength: float = 0.0
@export var reach: float = 520.0
## What the current does to things that are subject to the water, per second at full
## strength and zero distance.
@export var force: float = 900.0
## How sharply it can bend the striker, which does not obey the water at all - see
## Striker.steer. Kept separate because turning a ball and shoving a float are different
## quantities that happen to share a trigger.
@export var steering: float = 1.0

var origin: Vector2 = Vector2.ZERO


func falloff(p_at: Vector2) -> float:
	var distance := origin.distance_to(p_at)
	if distance >= reach:
		return 0.0
	# Linear rather than inverse-square: an inverse-square current is either useless at
	# range or uncontrollable up close, and the player needs it legible.
	return 1.0 - distance / reach


## Applies to everything that floats. Returns the impulse for this frame so the caller can
## hand it to whatever it is holding.
func impulse_for(p_at: Vector2, p_mass: float, p_delta: float) -> Vector2:
	var reachability := falloff(p_at)
	if reachability <= 0.0 or is_zero_approx(strength):
		return Vector2.ZERO
	var towards := (origin - p_at).normalized()
	# strength is negative for pull, so the sign carries the direction and nothing here
	# has to know which mode it is in.
	return towards * (-strength) * force * reachability * p_delta * p_mass


## Bends the striker. Positive pull turns it towards the paddle; push turns it away.
func steer_striker(p_striker: Striker, p_delta: float) -> void:
	if is_zero_approx(strength):
		return
	var reachability := falloff(p_striker.global_position)
	if reachability <= 0.0:
		return
	var towards := origin - p_striker.global_position
	p_striker.steer(towards, -strength * steering * reachability, p_delta)


func _draw() -> void:
	if is_zero_approx(strength):
		return
	# Something has to be visible or a continuous verb is invisible. Rings of water,
	# collapsing inward on pull and spreading outward on push.
	var pulling := strength < 0.0
	var tint := Color(0.55, 0.9, 1.0, 0.16) if pulling else Color(1.0, 0.75, 0.45, 0.16)
	var at := to_local(origin)
	for i in 4:
		var fraction := (float(i) + 1.0) / 5.0
		draw_arc(at, reach * fraction, 0.0, TAU, 48, tint, 2.0 + absf(strength) * 2.0, true)
