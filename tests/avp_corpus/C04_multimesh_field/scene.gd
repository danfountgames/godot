## C4: MultiMesh Field — Instancing path validation
##
## Content: 1 MultiMesh with 2000 instances (grass/vegetation).
## Purpose: Validates MultiMesh → MeshInstancesComponent conversion.
## Tests two-stage validation (§2.26): Stage 1 (API works) then
## Stage 2 (cost reduction vs individual entities measured).
## Hostile: No
##
## Used by: Phase 4 task 4.3 (instancing validation)
##
## Expected result: 2000 instances render correctly at 90 FPS.
## Stage 2 must show ≥30% improvement in at least TWO of four
## metrics (CPU time, GPU time, memory, draw cost) compared
## to 2000 individual ModelEntities.
extends Node3D

const INSTANCE_COUNT := 2000
const FIELD_SIZE := 30.0

func _ready() -> void:
	_build_field()

func _build_field() -> void:
	# Create the base mesh: a simple grass blade (thin box)
	var grass_mesh := BoxMesh.new()
	grass_mesh.size = Vector3(0.05, 0.3, 0.02)

	var grass_mat := StandardMaterial3D.new()
	grass_mat.albedo_color = Color(0.2, 0.6, 0.15)
	grass_mat.roughness = 0.9
	grass_mat.cull_mode = BaseMaterial3D.CULL_DISABLED

	# Create MultiMesh
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = true
	multimesh.instance_count = INSTANCE_COUNT
	multimesh.mesh = grass_mesh

	# Populate instances with random positions in a field
	var rng := RandomNumberGenerator.new()
	rng.seed = 42 # Deterministic for reproducibility

	for i in range(INSTANCE_COUNT):
		var x := rng.randf_range(-FIELD_SIZE / 2.0, FIELD_SIZE / 2.0)
		var z := rng.randf_range(-FIELD_SIZE / 2.0, FIELD_SIZE / 2.0)
		var y := 0.15 # Half height of grass blade

		# Random rotation around Y axis
		var angle := rng.randf_range(0, TAU)
		var scale_y := rng.randf_range(0.5, 1.5)

		var xform := Transform3D()
		xform = xform.scaled(Vector3(1.0, scale_y, 1.0))
		xform = xform.rotated(Vector3.UP, angle)
		xform.origin = Vector3(x, y * scale_y, z)

		multimesh.set_instance_transform(i, xform)

		# Vary the green tint slightly per instance
		var green_var := rng.randf_range(0.4, 0.8)
		multimesh.set_instance_color(i, Color(0.15, green_var, 0.1))

	# Create the MultiMeshInstance3D
	var mmi := MultiMeshInstance3D.new()
	mmi.name = "GrassField"
	mmi.multimesh = multimesh
	mmi.material_override = grass_mat
	add_child(mmi)

	# Add a ground plane for context
	var ground := MeshInstance3D.new()
	ground.name = "GroundPlane"
	var plane_mesh := PlaneMesh.new()
	plane_mesh.size = Vector2(FIELD_SIZE + 2, FIELD_SIZE + 2)
	ground.mesh = plane_mesh

	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.35, 0.25, 0.15)
	ground_mat.roughness = 1.0
	ground.material_override = ground_mat
	add_child(ground)
