## C11: [HOSTILE] Particle + Billboard Scene
##
## Content: GPUParticles3D, CPUParticles3D, billboard sprites,
## camera-facing quads.
## Purpose: Tests features NOT in the bridge — expected to be
## missing or degraded.
## Hostile: YES
##
## Expected result: DOCUMENTED DEGRADATION.
## - Geometry (non-particle meshes): PRESERVED
## - Transforms: PRESERVED
## - GPUParticles3D: LOST (particle positions not extracted)
## - CPUParticles3D: LOST (not forwarded)
## - Billboard sprites: LOST (camera-relative, no forwarding)
## - Camera-facing quads: LOST (no camera forwarding)
extends Node3D

func _ready() -> void:
	_build_scene()

func _build_scene() -> void:
	# Static reference mesh (should render correctly)
	var reference := MeshInstance3D.new()
	reference.name = "ReferenceMesh"
	reference.mesh = BoxMesh.new()
	reference.position = Vector3(0, 0.5, 0)
	var ref_mat := StandardMaterial3D.new()
	ref_mat.albedo_color = Color.WHITE
	reference.material_override = ref_mat
	add_child(reference)

	# GPU Particles — fire effect (EXPECTED: LOST)
	var gpu_particles := GPUParticles3D.new()
	gpu_particles.name = "FireParticles_GPU"
	gpu_particles.amount = 200
	gpu_particles.lifetime = 1.5
	gpu_particles.position = Vector3(-3, 0, 0)
	var particle_mesh := SphereMesh.new()
	particle_mesh.radius = 0.05
	particle_mesh.height = 0.1
	gpu_particles.draw_pass_1 = particle_mesh
	var particle_mat := ParticleProcessMaterial.new()
	particle_mat.direction = Vector3(0, 1, 0)
	particle_mat.initial_velocity_min = 1.0
	particle_mat.initial_velocity_max = 2.0
	particle_mat.gravity = Vector3(0, -2, 0)
	gpu_particles.process_material = particle_mat
	add_child(gpu_particles)

	# CPU Particles — sparks (EXPECTED: LOST)
	var cpu_particles := CPUParticles3D.new()
	cpu_particles.name = "SparkParticles_CPU"
	cpu_particles.amount = 100
	cpu_particles.lifetime = 0.8
	cpu_particles.position = Vector3(3, 0, 0)
	cpu_particles.mesh = particle_mesh
	cpu_particles.direction = Vector3(0, 1, 0)
	cpu_particles.initial_velocity_min = 3.0
	cpu_particles.initial_velocity_max = 5.0
	cpu_particles.gravity = Vector3(0, -9.8, 0)
	add_child(cpu_particles)

	# Billboard sprite (EXPECTED: LOST — camera-relative)
	var billboard := MeshInstance3D.new()
	billboard.name = "BillboardSprite"
	var quad := QuadMesh.new()
	quad.size = Vector2(1.5, 1.5)
	billboard.mesh = quad
	billboard.position = Vector3(0, 2, -2)
	var bb_mat := StandardMaterial3D.new()
	bb_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	bb_mat.albedo_color = Color(1, 1, 0, 0.8)
	bb_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	billboard.material_override = bb_mat
	add_child(billboard)

	# Fixed-Y billboard (EXPECTED: LOST)
	var fixed_bb := MeshInstance3D.new()
	fixed_bb.name = "FixedYBillboard"
	fixed_bb.mesh = quad.duplicate()
	fixed_bb.position = Vector3(2, 2, -2)
	var fixed_mat := StandardMaterial3D.new()
	fixed_mat.billboard_mode = BaseMaterial3D.BILLBOARD_FIXED_Y
	fixed_mat.albedo_color = Color(0, 1, 1, 0.8)
	fixed_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	fixed_bb.material_override = fixed_mat
	add_child(fixed_bb)
