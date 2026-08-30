class_name Jet
extends Area2D

## A wall jet. Hold it and it blasts a directed current at everything gliding through.
##
## This is the brief's answer to breakout's agency problem, and it is deliberately a
## field rather than a steering wheel: it pushes whatever is in the cone, including
## things the player would rather it did not.

@export var direction: Vector2 = Vector2.UP
@export var force: float = 2600.0
@export var reach: float = 320.0
@export var half_width: float = 130.0

## Pressure is what stops the jet being a steering wheel. It drains while held and
## refills slowly, so the player gets a few short bursts per shot, not continuous
## control.
@export var max_pressure: float = 2.2
@export var refill_rate: float = 0.55

var pressure: float = max_pressure
var firing: bool = false

var _inside: Array[Ring] = []


func _ready() -> void:
	var shape := RectangleShape2D.new()
	shape.size = Vector2(half_width * 2.0, reach)
	var collider := CollisionShape2D.new()
	collider.shape = shape
	collider.position = direction.normalized() * reach * 0.5
	add_child(collider)
	body_entered.connect(_on_body_entered)
	body_exited.connect(_on_body_exited)


func _physics_process(delta: float) -> void:
	if firing and pressure > 0.0:
		pressure = maxf(0.0, pressure - delta)
		var push := direction.normalized() * force * delta
		for ring in _inside:
			if not is_instance_valid(ring) or ring.is_queued_for_deletion():
				continue
			# Rings at rest are pushed too. Shepherding passive rings into a line for
			# the next shot is one of the two things the jet is for, so a resting ring
			# has to be wakeable - and a sleeping body ignores an impulse.
			ring.sleeping = false
			ring.at_rest = false
			# Scaled by mass so a big ring is shoved less than a small one, which is
			# the whole reason a chain makes you heavy.
			ring.apply_central_impulse(push * ring.mass)
	else:
		pressure = minf(max_pressure, pressure + refill_rate * delta)
	if pressure <= 0.0:
		firing = false


func _on_body_entered(body: Node) -> void:
	var ring := body as Ring
	if ring != null and not _inside.has(ring):
		_inside.append(ring)


func _on_body_exited(body: Node) -> void:
	var ring := body as Ring
	if ring != null:
		_inside.erase(ring)
