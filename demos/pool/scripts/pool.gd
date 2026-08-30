class_name Pool
extends Node2D

## The rectangular pool: four walls and the water.
##
## Built in code rather than authored as nodes so the arena is one number. The brief's
## later pools are kidney-shaped, lane-shaped and infinity-edged; keeping the geometry
## here makes those a subclass rather than a rebuild.

@export var size: Vector2 = Vector2(1024, 560)
@export var wall_thickness: float = 24.0
@export var wall_bounce: float = 0.72
## Closed on all four sides. Rings are never lost: survivors carry into the next set as
## obstacles and ammunition, so a ring leaving the table would be losing the board state
## the player just built.
@export var lane_spacing: float = 128.0

var bounds: Rect2:
	get:
		return Rect2(Vector2.ZERO, size)


func _ready() -> void:
	_build_walls()


func _build_walls() -> void:
	var edges := {
		"Top": [Vector2(size.x * 0.5, -wall_thickness * 0.5), Vector2(size.x, wall_thickness)],
		"Bottom": [Vector2(size.x * 0.5, size.y + wall_thickness * 0.5), Vector2(size.x, wall_thickness)],
		"Left": [Vector2(-wall_thickness * 0.5, size.y * 0.5), Vector2(wall_thickness, size.y)],
		"Right": [Vector2(size.x + wall_thickness * 0.5, size.y * 0.5), Vector2(wall_thickness, size.y)],
	}

	var surface := PhysicsMaterial.new()
	surface.bounce = wall_bounce
	surface.friction = 0.1

	for wall_name in edges:
		var body := StaticBody2D.new()
		body.name = wall_name
		body.position = edges[wall_name][0]
		body.physics_material_override = surface
		var shape := RectangleShape2D.new()
		shape.size = edges[wall_name][1]
		var collider := CollisionShape2D.new()
		collider.shape = shape
		body.add_child(collider)
		add_child(body)


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), Color(0.09, 0.42, 0.58))
	# Lap lanes. Decoration, and a coarse ruler for judging a shot by eye.
	var lane := Color(1, 1, 1, 0.06)
	var x := lane_spacing
	while x < size.x:
		draw_line(Vector2(x, 0), Vector2(x, size.y), lane, 2.0)
		x += lane_spacing
	# The shooting edge.
	draw_line(Vector2(0, size.y - 8.0), Vector2(size.x, size.y - 8.0), Color(1, 1, 1, 0.18), 2.0)
