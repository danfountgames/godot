## C1: Static Showroom — Baseline test scene
##
## Content: 50 static meshes, 20 materials, 5 textures.
## Purpose: Baseline — does the bridge work at all?
## Hostile: No
##
## Used by: Proof A (extraction truthfulness), Proof C (frame budget)
##
## Expected result: All 50 meshes render correctly in RealityKit with
## correct transforms, materials, and visibility states.
extends Node3D

const MESH_COUNT := 50
const MATERIAL_COUNT := 20

func _ready() -> void:
	_build_showroom()

func _build_showroom() -> void:
	var materials: Array[StandardMaterial3D] = []

	# Create 20 distinct materials
	for i in range(MATERIAL_COUNT):
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(float(i) / MATERIAL_COUNT, 0.7, 0.9)
		mat.roughness = randf_range(0.1, 0.9)
		mat.metallic = randf_range(0.0, 1.0)
		if i % 4 == 0:
			mat.emission_enabled = true
			mat.emission = Color.from_hsv(float(i) / MATERIAL_COUNT, 1.0, 1.0)
			mat.emission_energy_multiplier = 2.0
		materials.append(mat)

	# Create 50 static meshes in a grid
	var mesh_types: Array[Mesh] = [
		BoxMesh.new(),
		SphereMesh.new(),
		CylinderMesh.new(),
		CapsuleMesh.new(),
		PrismMesh.new(),
	]

	for i in range(MESH_COUNT):
		var mi := MeshInstance3D.new()
		mi.name = "StaticMesh_%03d" % i
		mi.mesh = mesh_types[i % mesh_types.size()]
		mi.material_override = materials[i % materials.size()]

		# Arrange in a 10x5 grid
		var col := i % 10
		var row := i / 10
		mi.position = Vector3(col * 2.0 - 9.0, 0.0, row * 2.0 - 4.0)
		mi.rotation_degrees = Vector3(0, randf_range(0, 360), 0)
		mi.scale = Vector3.ONE * randf_range(0.5, 1.5)

		add_child(mi)
