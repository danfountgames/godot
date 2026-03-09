## C9: [HOSTILE] Light-Dependent Scene
##
## Content: Scene where visual correctness depends entirely on Godot
## point/spot lights, baked lightmaps, and shadow casting.
## Purpose: Tests lighting gap honestly — EXPECTED to look wrong.
## Hostile: YES
##
## Expected result: DOCUMENTED DEGRADATION.
## - Geometry: PRESERVED
## - PBR materials: PRESERVED
## - Transforms: PRESERVED
## - Point lights: LOST (not forwarded, §2.17)
## - Spot lights: LOST (not forwarded)
## - Shadows: LOST (not forwarded)
## - Environment sky: LOST (not forwarded)
## - The scene will look visually different because Godot lights are not
##   forwarded. RealityKit IBL may or may not approximate the intent.
extends Node3D

func _ready() -> void:
	_build_scene()

func _build_scene() -> void:
	# Dark room — relies entirely on Godot lights
	var env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.02, 0.05)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.environment = environment
	add_child(env)

	# Floor
	var floor_mi := MeshInstance3D.new()
	floor_mi.mesh = PlaneMesh.new()
	(floor_mi.mesh as PlaneMesh).size = Vector2(20, 20)
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.3, 0.3, 0.35)
	floor_mat.roughness = 0.8
	floor_mi.material_override = floor_mat
	add_child(floor_mi)

	# Central object
	var sphere := MeshInstance3D.new()
	sphere.mesh = SphereMesh.new()
	sphere.position = Vector3(0, 1, 0)
	var sphere_mat := StandardMaterial3D.new()
	sphere_mat.albedo_color = Color.WHITE
	sphere_mat.metallic = 0.8
	sphere_mat.roughness = 0.2
	sphere.material_override = sphere_mat
	add_child(sphere)

	# Red point light (left)
	var light_red := OmniLight3D.new()
	light_red.light_color = Color.RED
	light_red.light_energy = 4.0
	light_red.omni_range = 5.0
	light_red.position = Vector3(-3, 2, 0)
	light_red.shadow_enabled = true
	add_child(light_red)

	# Blue point light (right)
	var light_blue := OmniLight3D.new()
	light_blue.light_color = Color.BLUE
	light_blue.light_energy = 4.0
	light_blue.omni_range = 5.0
	light_blue.position = Vector3(3, 2, 0)
	light_blue.shadow_enabled = true
	add_child(light_blue)

	# Spot light (above, casting dramatic shadow)
	var spotlight := SpotLight3D.new()
	spotlight.light_color = Color(1.0, 0.9, 0.7)
	spotlight.light_energy = 8.0
	spotlight.spot_range = 10.0
	spotlight.spot_angle = 30.0
	spotlight.position = Vector3(0, 5, 2)
	spotlight.rotation_degrees = Vector3(-60, 0, 0)
	spotlight.shadow_enabled = true
	add_child(spotlight)

	# Shadow-casting pillars
	for i in range(4):
		var pillar := MeshInstance3D.new()
		var cyl := CylinderMesh.new()
		cyl.top_radius = 0.15
		cyl.bottom_radius = 0.15
		cyl.height = 3.0
		pillar.mesh = cyl
		var angle := float(i) * TAU / 4.0
		pillar.position = Vector3(cos(angle) * 2.0, 1.5, sin(angle) * 2.0)
		var pillar_mat := StandardMaterial3D.new()
		pillar_mat.albedo_color = Color(0.6, 0.5, 0.4)
		pillar.material_override = pillar_mat
		add_child(pillar)
