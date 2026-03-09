## C3: Skinned Character Parade — Skeletal animation stress test
##
## Content: 10 skinned characters with skeletal animation playing.
## Purpose: Red-risk proof for §2.18 — skinned mesh streaming viability.
## Tests CPU-side skinning overhead and mesh re-upload cost.
## Hostile: No
##
## Used by: Proof C (frame budget), Phase 4 hard-cut rule (§2.24)
##
## Expected result: Characters animate at 90 FPS on hardware.
## Target: 20 skinned characters. Minimum viable: 5 characters.
## If skinned mesh re-upload cost is too high, this scene documents
## the ceiling and the hard-cut decision.
##
## NOTE: This test scene uses procedural "skeleton-like" animation
## since full skinned meshes require imported assets. The scene
## approximates the per-frame cost by driving multiple MeshInstance3D
## transforms at high frequency, simulating what the bridge would do
## for each bone-influenced vertex group.
extends Node3D

const CHARACTER_COUNT := 10
const BONES_PER_CHARACTER := 15
const ANIMATION_SPEED := 2.0

var characters: Array[Dictionary] = []
var time_elapsed := 0.0

func _ready() -> void:
	_build_parade()

func _process(delta: float) -> void:
	time_elapsed += delta
	_animate_characters()

func _build_parade() -> void:
	for i in range(CHARACTER_COUNT):
		var character := _create_procedural_character(i)
		characters.append(character)

func _create_procedural_character(idx: int) -> Dictionary:
	var root := Node3D.new()
	root.name = "Character_%02d" % idx
	# Arrange in a line
	root.position = Vector3(idx * 2.0 - (CHARACTER_COUNT - 1), 0.0, 0.0)
	add_child(root)

	var hue := float(idx) / CHARACTER_COUNT
	var bones: Array[MeshInstance3D] = []

	# Build a simple humanoid-ish skeleton: chain of capsules
	# Simulating: hips, spine, chest, neck, head, L/R upper arm,
	# L/R forearm, L/R upper leg, L/R lower leg, L/R foot, L/R hand
	var bone_configs := [
		{"name": "Hips", "pos": Vector3(0, 1.0, 0), "size": Vector3(0.4, 0.2, 0.2)},
		{"name": "Spine", "pos": Vector3(0, 1.3, 0), "size": Vector3(0.35, 0.25, 0.2)},
		{"name": "Chest", "pos": Vector3(0, 1.6, 0), "size": Vector3(0.4, 0.25, 0.2)},
		{"name": "Neck", "pos": Vector3(0, 1.85, 0), "size": Vector3(0.1, 0.1, 0.1)},
		{"name": "Head", "pos": Vector3(0, 2.05, 0), "size": Vector3(0.2, 0.25, 0.2)},
		{"name": "UpperArm_L", "pos": Vector3(-0.5, 1.55, 0), "size": Vector3(0.3, 0.1, 0.1)},
		{"name": "ForeArm_L", "pos": Vector3(-0.85, 1.55, 0), "size": Vector3(0.25, 0.08, 0.08)},
		{"name": "UpperArm_R", "pos": Vector3(0.5, 1.55, 0), "size": Vector3(0.3, 0.1, 0.1)},
		{"name": "ForeArm_R", "pos": Vector3(0.85, 1.55, 0), "size": Vector3(0.25, 0.08, 0.08)},
		{"name": "UpperLeg_L", "pos": Vector3(-0.15, 0.6, 0), "size": Vector3(0.12, 0.35, 0.12)},
		{"name": "LowerLeg_L", "pos": Vector3(-0.15, 0.2, 0), "size": Vector3(0.1, 0.3, 0.1)},
		{"name": "UpperLeg_R", "pos": Vector3(0.15, 0.6, 0), "size": Vector3(0.12, 0.35, 0.12)},
		{"name": "LowerLeg_R", "pos": Vector3(0.15, 0.2, 0), "size": Vector3(0.1, 0.3, 0.1)},
		{"name": "Foot_L", "pos": Vector3(-0.15, 0.05, 0.08), "size": Vector3(0.1, 0.05, 0.15)},
		{"name": "Foot_R", "pos": Vector3(0.15, 0.05, 0.08), "size": Vector3(0.1, 0.05, 0.15)},
	]

	for b in range(min(BONES_PER_CHARACTER, bone_configs.size())):
		var cfg = bone_configs[b]
		var mi := MeshInstance3D.new()
		mi.name = "%s_%s" % [root.name, cfg["name"]]

		var box := BoxMesh.new()
		box.size = cfg["size"]
		mi.mesh = box
		mi.position = cfg["pos"]

		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(hue, 0.6, 0.85)
		mi.material_override = mat

		root.add_child(mi)
		bones.append(mi)

	return {"root": root, "bones": bones, "phase": float(idx) * 0.5}

func _animate_characters() -> void:
	for char_data in characters:
		var phase: float = char_data["phase"]
		var bones_arr: Array = char_data["bones"]
		var t := time_elapsed * ANIMATION_SPEED + phase

		# Simple idle/walk cycle approximation
		for b in range(bones_arr.size()):
			var mi: MeshInstance3D = bones_arr[b]
			var bone_name: String = mi.name

			# Apply subtle per-bone animation based on name
			if "Hips" in bone_name:
				mi.position.y = 1.0 + sin(t * 2.0) * 0.02
			elif "Spine" in bone_name:
				mi.rotation.z = sin(t * 2.0) * 0.03
			elif "UpperArm_L" in bone_name:
				mi.rotation.z = sin(t * 2.0) * 0.3
			elif "ForeArm_L" in bone_name:
				mi.rotation.z = sin(t * 2.0 + 0.5) * 0.4 - 0.3
			elif "UpperArm_R" in bone_name:
				mi.rotation.z = -sin(t * 2.0) * 0.3
			elif "ForeArm_R" in bone_name:
				mi.rotation.z = -sin(t * 2.0 + 0.5) * 0.4 + 0.3
			elif "UpperLeg_L" in bone_name:
				mi.rotation.x = sin(t * 2.0) * 0.4
			elif "LowerLeg_L" in bone_name:
				mi.rotation.x = sin(t * 2.0 + 1.0) * 0.3 - 0.2
			elif "UpperLeg_R" in bone_name:
				mi.rotation.x = -sin(t * 2.0) * 0.4
			elif "LowerLeg_R" in bone_name:
				mi.rotation.x = -sin(t * 2.0 + 1.0) * 0.3 + 0.2
			elif "Head" in bone_name:
				mi.rotation.y = sin(t * 0.5) * 0.15
