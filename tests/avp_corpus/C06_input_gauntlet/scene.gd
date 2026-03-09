## C6: Input Gauntlet — Input routing + interaction overhead
##
## Content: 100 entities with collision shapes, all tappable/draggable.
## Purpose: Stress-test spatial input routing and measure interaction
## overhead with many interactive entities.
## Hostile: No
##
## Used by: Phase 7 task 7.7 (interaction validation)
##
## Expected result:
## - Tap latency <100ms per entity
## - Correct entity identification for all 100 entities
## - Drag 3D translation accuracy within 1cm
## - 90 FPS maintained with all entities interactive
extends Node3D

const ENTITY_COUNT := 100
const GRID_COLS := 10
const GRID_ROWS := 10
const SPACING := 1.5

signal entity_tapped(entity_name: String, hit_position: Vector3)
signal entity_dragged(entity_name: String, phase: int, translation: Vector3)

var tap_count := 0

func _ready() -> void:
	_build_gauntlet()

func _build_gauntlet() -> void:
	var mesh_types: Array[Mesh] = [
		BoxMesh.new(),
		SphereMesh.new(),
		CylinderMesh.new(),
		CapsuleMesh.new(),
		PrismMesh.new(),
	]

	# Scale meshes down for dense packing
	for m in mesh_types:
		if m is BoxMesh:
			m.size = Vector3(0.5, 0.5, 0.5)
		elif m is SphereMesh:
			m.radius = 0.25
			m.height = 0.5
		elif m is CylinderMesh:
			m.top_radius = 0.2
			m.bottom_radius = 0.2
			m.height = 0.5
		elif m is CapsuleMesh:
			m.radius = 0.15
			m.height = 0.5
		elif m is PrismMesh:
			m.size = Vector3(0.5, 0.5, 0.5)

	for i in range(ENTITY_COUNT):
		var col := i % GRID_COLS
		var row := i / GRID_COLS

		var mi := MeshInstance3D.new()
		mi.name = "Interactive_%03d" % i
		mi.mesh = mesh_types[i % mesh_types.size()]

		# Position in grid
		var x := (col - GRID_COLS / 2.0 + 0.5) * SPACING
		var z := (row - GRID_ROWS / 2.0 + 0.5) * SPACING
		mi.position = Vector3(x, 0.5, z)

		# Color based on position
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(
			float(i) / ENTITY_COUNT,
			0.7,
			0.85
		)
		mi.material_override = mat

		# Add collision shape for input
		var body := StaticBody3D.new()
		body.name = "Body_%03d" % i

		var col_shape := CollisionShape3D.new()
		# Use box collision for all — simpler and consistent
		var box_shape := BoxShape3D.new()
		box_shape.size = Vector3(0.5, 0.5, 0.5)
		col_shape.shape = box_shape
		body.add_child(col_shape)
		mi.add_child(body)

		add_child(mi)
