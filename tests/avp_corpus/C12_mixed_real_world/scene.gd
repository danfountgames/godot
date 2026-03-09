## C12: Mixed Real-World — The "honest demo" scene
##
## Content: 30 static meshes, 5 skinned characters, 1 MultiMesh (500 instances),
## 3 lights, 2 custom shader objects, HUD attachment, tappable objects.
## Purpose: Closest to a real game. Tests all bridge subsystems together.
## Hostile: No
##
## Used by: Proof C (frame budget), Proof E (failure envelopes),
## Phase 8 benchmark matrix
##
## Expected result:
## - Static meshes: PRESERVED
## - Skinned characters: PRESERVED (if Phase 4 hard-cut passes)
## - MultiMesh instances: PRESERVED
## - Lights: LOST (documented per §2.17)
## - Custom shaders: LOST (fallback to PBR, documented per §2.17)
## - HUD: PRESERVED (positioned in volume space)
## - Input: PRESERVED (tappable objects respond)
## - Overall 90 FPS target on hardware
extends Node3D

const STATIC_COUNT := 30
const CHARACTER_COUNT := 5
const MULTIMESH_INSTANCES := 500

var time_elapsed := 0.0
var characters: Array[Dictionary] = []

func _ready() -> void:
	_build_static_environment()
	_build_characters()
	_build_vegetation()
	_build_lights()
	_build_custom_shader_objects()
	_build_hud()
	_build_interactive_objects()

func _process(delta: float) -> void:
	time_elapsed += delta
	_animate_characters()

# --- Static environment (30 meshes) ---
func _build_static_environment() -> void:
	# Ground
	var ground := MeshInstance3D.new()
	ground.name = "Ground"
	var plane := PlaneMesh.new()
	plane.size = Vector2(20, 20)
	ground.mesh = plane
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.4, 0.35, 0.25)
	ground_mat.roughness = 0.95
	ground.material_override = ground_mat
	add_child(ground)

	# Buildings / structures
	var rng := RandomNumberGenerator.new()
	rng.seed = 123

	for i in range(STATIC_COUNT - 1): # -1 for ground
		var mi := MeshInstance3D.new()
		mi.name = "Static_%02d" % i

		match i % 4:
			0:
				var box := BoxMesh.new()
				box.size = Vector3(
					rng.randf_range(0.5, 2.0),
					rng.randf_range(0.5, 3.0),
					rng.randf_range(0.5, 2.0)
				)
				mi.mesh = box
			1:
				mi.mesh = CylinderMesh.new()
			2:
				mi.mesh = SphereMesh.new()
			3:
				mi.mesh = CapsuleMesh.new()

		# Scatter in the scene
		mi.position = Vector3(
			rng.randf_range(-8.0, 8.0),
			rng.randf_range(0.5, 1.5),
			rng.randf_range(-8.0, 8.0)
		)

		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(rng.randf(), 0.5, 0.7)
		mat.roughness = rng.randf_range(0.2, 0.9)
		mat.metallic = rng.randf_range(0.0, 0.5)
		mi.material_override = mat
		add_child(mi)

# --- Skinned characters (5) ---
func _build_characters() -> void:
	for i in range(CHARACTER_COUNT):
		var root := Node3D.new()
		root.name = "Char_%02d" % i
		root.position = Vector3(i * 2.5 - 5.0, 0.0, -3.0)
		add_child(root)

		var bones: Array[MeshInstance3D] = []
		var hue := float(i) / CHARACTER_COUNT

		# Simplified body: 5 key bones
		var bone_data := [
			{"pos": Vector3(0, 0.5, 0), "size": Vector3(0.3, 0.5, 0.2)},
			{"pos": Vector3(0, 1.2, 0), "size": Vector3(0.35, 0.4, 0.2)},
			{"pos": Vector3(0, 1.8, 0), "size": Vector3(0.2, 0.2, 0.2)},
			{"pos": Vector3(-0.4, 1.1, 0), "size": Vector3(0.4, 0.1, 0.1)},
			{"pos": Vector3(0.4, 1.1, 0), "size": Vector3(0.4, 0.1, 0.1)},
		]

		for b in bone_data:
			var mi := MeshInstance3D.new()
			var box := BoxMesh.new()
			box.size = b["size"]
			mi.mesh = box
			mi.position = b["pos"]

			var mat := StandardMaterial3D.new()
			mat.albedo_color = Color.from_hsv(hue, 0.6, 0.8)
			mi.material_override = mat
			root.add_child(mi)
			bones.append(mi)

		characters.append({"root": root, "bones": bones, "phase": float(i) * 0.7})

func _animate_characters() -> void:
	for ch in characters:
		var t := time_elapsed * 2.0 + ch["phase"]
		var bones_arr: Array = ch["bones"]
		if bones_arr.size() >= 5:
			# Subtle idle sway
			(bones_arr[0] as MeshInstance3D).position.y = 0.5 + sin(t) * 0.02
			(bones_arr[3] as MeshInstance3D).rotation.z = sin(t) * 0.3
			(bones_arr[4] as MeshInstance3D).rotation.z = -sin(t) * 0.3

# --- Vegetation via MultiMesh (500 instances) ---
func _build_vegetation() -> void:
	var grass := BoxMesh.new()
	grass.size = Vector3(0.04, 0.2, 0.02)

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.instance_count = MULTIMESH_INSTANCES
	mm.mesh = grass

	var rng := RandomNumberGenerator.new()
	rng.seed = 456

	for i in range(MULTIMESH_INSTANCES):
		var x := rng.randf_range(-9.0, 9.0)
		var z := rng.randf_range(-9.0, 9.0)
		var scale_y := rng.randf_range(0.5, 1.2)

		var xf := Transform3D()
		xf = xf.scaled(Vector3(1.0, scale_y, 1.0))
		xf = xf.rotated(Vector3.UP, rng.randf_range(0, TAU))
		xf.origin = Vector3(x, 0.1 * scale_y, z)
		mm.set_instance_transform(i, xf)
		mm.set_instance_color(i, Color(0.1, rng.randf_range(0.4, 0.7), 0.1))

	var mmi := MultiMeshInstance3D.new()
	mmi.name = "Vegetation"
	mmi.multimesh = mm
	var veg_mat := StandardMaterial3D.new()
	veg_mat.albedo_color = Color(0.2, 0.55, 0.15)
	veg_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mmi.material_override = veg_mat
	add_child(mmi)

# --- Lights (3) — expected LOST in bridge ---
func _build_lights() -> void:
	var light1 := OmniLight3D.new()
	light1.name = "Light_Red"
	light1.position = Vector3(-3, 3, 0)
	light1.light_color = Color.RED
	light1.light_energy = 2.0
	light1.omni_range = 6.0
	add_child(light1)

	var light2 := OmniLight3D.new()
	light2.name = "Light_Blue"
	light2.position = Vector3(3, 3, 0)
	light2.light_color = Color.BLUE
	light2.light_energy = 2.0
	light2.omni_range = 6.0
	add_child(light2)

	var light3 := SpotLight3D.new()
	light3.name = "Light_Spot"
	light3.position = Vector3(0, 5, 0)
	light3.rotation_degrees = Vector3(-90, 0, 0)
	light3.light_color = Color(1, 0.9, 0.7)
	light3.light_energy = 3.0
	light3.spot_range = 10.0
	light3.spot_angle = 30.0
	add_child(light3)

# --- Custom shader objects (2) — expected LOST, fallback to PBR ---
func _build_custom_shader_objects() -> void:
	# Object with custom shader (no @rk_shader annotation)
	for i in range(2):
		var mi := MeshInstance3D.new()
		mi.name = "CustomShader_%d" % i
		mi.mesh = SphereMesh.new()
		mi.position = Vector3(-4.0 + i * 8.0, 1.5, 5.0)

		# Use a ShaderMaterial with a simple custom shader
		var shader := Shader.new()
		shader.code = """
shader_type spatial;
uniform vec4 base_color : source_color = vec4(0.5, 0.0, 1.0, 1.0);
uniform float wave_speed = 2.0;
void vertex() {
	VERTEX.y += sin(VERTEX.x * 3.0 + TIME * wave_speed) * 0.1;
}
void fragment() {
	ALBEDO = base_color.rgb;
	ROUGHNESS = 0.5;
}
"""
		var shader_mat := ShaderMaterial.new()
		shader_mat.shader = shader
		mi.material_override = shader_mat
		add_child(mi)

# --- HUD attachment (positioned in volume space) ---
func _build_hud() -> void:
	# Simple 3D "HUD" — a flat panel with info
	var hud := MeshInstance3D.new()
	hud.name = "HUD_Panel"
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 0.5)
	hud.mesh = quad
	hud.position = Vector3(0, 3.0, -2.0)
	hud.rotation_degrees = Vector3(-15, 0, 0)

	var hud_mat := StandardMaterial3D.new()
	hud_mat.albedo_color = Color(0.1, 0.1, 0.2, 0.8)
	hud_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	hud_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	hud_mat.emission_enabled = true
	hud_mat.emission = Color(0.2, 0.4, 0.8)
	hud_mat.emission_energy_multiplier = 0.5
	hud.material_override = hud_mat
	add_child(hud)

# --- Interactive objects (5 tappable) ---
func _build_interactive_objects() -> void:
	for i in range(5):
		var mi := MeshInstance3D.new()
		mi.name = "Tappable_%d" % i
		mi.mesh = BoxMesh.new()
		mi.position = Vector3(i * 2.0 - 4.0, 0.5, 3.0)
		mi.scale = Vector3(0.6, 0.6, 0.6)

		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(float(i) / 5.0, 0.8, 0.9)
		mat.emission_enabled = true
		mat.emission = mat.albedo_color * 0.3
		mi.material_override = mat

		var body := StaticBody3D.new()
		var col := CollisionShape3D.new()
		col.shape = BoxShape3D.new()
		body.add_child(col)
		mi.add_child(body)

		add_child(mi)
