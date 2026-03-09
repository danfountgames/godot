## C7: Volume Transitions — Volume camera + SwiftUI lifecycle
##
## Content: Scene with bounded volume, script toggles bounded↔immersive
## every 5 seconds.
## Purpose: Tests VolumeCamera mode transitions, content clipping,
## and SwiftUI lifecycle during view changes.
## Hostile: No
##
## Used by: Phase 3 (Volume Camera), Proof E (lifecycle abuse)
##
## Expected result:
## - Mode transitions are smooth (no crash, no entity leak)
## - Content correctly clips in bounded mode
## - Content renders without clipping in immersive mode
## - Entity count remains stable across transitions
## - 90 FPS maintained during and after transitions
extends Node3D

const TRANSITION_INTERVAL := 5.0
const OBJECT_COUNT := 30

var transition_timer := 0.0
var is_immersive := false
var transition_count := 0

func _ready() -> void:
	_build_scene()

func _process(delta: float) -> void:
	transition_timer += delta
	if transition_timer >= TRANSITION_INTERVAL:
		transition_timer = 0.0
		_toggle_mode()

func _build_scene() -> void:
	# Create objects at varying distances from origin
	# Some inside default volume bounds, some outside
	for i in range(OBJECT_COUNT):
		var mi := MeshInstance3D.new()
		mi.name = "VolumeObj_%02d" % i

		# Alternate mesh types
		match i % 3:
			0: mi.mesh = BoxMesh.new()
			1: mi.mesh = SphereMesh.new()
			2: mi.mesh = CylinderMesh.new()

		# Place objects in a sphere pattern at varying radii
		var angle := float(i) / OBJECT_COUNT * TAU
		var radius := 0.5 + float(i) / OBJECT_COUNT * 3.0
		var height := sin(float(i) * 0.5) * 1.5

		mi.position = Vector3(
			cos(angle) * radius,
			height,
			sin(angle) * radius
		)
		mi.scale = Vector3.ONE * (0.3 + float(i) / OBJECT_COUNT * 0.5)

		var mat := StandardMaterial3D.new()
		# Objects inside volume bounds are blue, outside are red
		if mi.position.length() < 1.5:
			mat.albedo_color = Color(0.2, 0.4, 0.9)
		else:
			mat.albedo_color = Color(0.9, 0.3, 0.2)
		mat.roughness = 0.5
		mi.material_override = mat

		add_child(mi)

	# Add a reference marker at volume center
	var center := MeshInstance3D.new()
	center.name = "VolumeCenter"
	center.mesh = SphereMesh.new()
	(center.mesh as SphereMesh).radius = 0.1
	var center_mat := StandardMaterial3D.new()
	center_mat.albedo_color = Color.WHITE
	center_mat.emission_enabled = true
	center_mat.emission = Color.WHITE
	center_mat.emission_energy_multiplier = 2.0
	center.material_override = center_mat
	add_child(center)

func _toggle_mode() -> void:
	is_immersive = !is_immersive
	transition_count += 1

	# In a real integration, this would call:
	# godot_avp_volume_camera_set_mode(1 if is_immersive else 0)
	# For the test script, we log the transition.
	print("Volume transition #%d → %s" % [
		transition_count,
		"IMMERSIVE" if is_immersive else "BOUNDED"
	])
