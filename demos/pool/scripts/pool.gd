class_name Pool
extends Node2D

## The pool: three walls and the water. The bottom edge is open, because the bottom edge
## is the thing you are defending.
##
## Built in code rather than authored as nodes so the arena is one number. Later pools in
## the brief are kidney-shaped, lane-shaped and infinity-edged; keeping the geometry here
## makes those a subclass rather than a rebuild.

@export var size: Vector2 = Vector2(840, 560)
@export var wall_thickness: float = 24.0
@export var lane_spacing: float = 120.0


func _ready() -> void:
	_build_walls()


func _build_walls() -> void:
	# No bottom. A striker that gets past the lounger is lost, and that is the only way
	# to lose, so the edge has to be genuinely open rather than a wall that forgives.
	var edges := {
		"Top": [Vector2(size.x * 0.5, -wall_thickness * 0.5), Vector2(size.x, wall_thickness)],
		"Left": [Vector2(-wall_thickness * 0.5, size.y * 0.5), Vector2(wall_thickness, size.y)],
		"Right": [Vector2(size.x + wall_thickness * 0.5, size.y * 0.5), Vector2(wall_thickness, size.y)],
	}

	var surface := PhysicsMaterial.new()
	# Perfectly elastic, like the striker. A wall that takes 5% off is drag wearing a
	# different hat, and after a dozen rebounds the ball is limping.
	surface.bounce = 1.0
	surface.friction = 0.0

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
	var lane := Color(1, 1, 1, 0.05)
	var x := lane_spacing
	while x < size.x:
		draw_line(Vector2(x, 0), Vector2(x, size.y), lane, 2.0)
		x += lane_spacing
	# The open edge, marked so it reads as a drop rather than as a wall.
	draw_line(Vector2(0, size.y), Vector2(size.x, size.y), Color(1, 0.32, 0.35, 0.65), 4.0)
