## C16: Recycled RID Torture Test
##
## Content: Script rapidly creates/destroys instances to force RID reuse.
## Purpose: Validates no stale entity resurrection (§2.27).
## Hostile: No (but deliberately adversarial)
##
## Used by: Proof A (extraction truthfulness), Proof E (failure envelopes)
##
## CRITICAL NEGATIVE TEST (from §2.20):
## Create entity A (RID X). Destroy A. Wait 5 frames. Create entity B
## (Godot may reuse RID X). B must have its OWN mesh/material/position.
## B does NOT inherit A's appearance or transform.
extends Node3D

const BURST_SIZE := 20
const BURST_INTERVAL := 0.1  # seconds
const DESTROY_DELAY_FRAMES := 5

var _frame_count := 0
var _burst_accumulator := 0.0
var _entity_counter := 0
var _pending_destroys: Array[Dictionary] = []

func _ready() -> void:
	pass

func _process(delta: float) -> void:
	_frame_count += 1
	_burst_accumulator += delta

	# Process pending destroys (after frame delay)
	var still_pending: Array[Dictionary] = []
	for entry in _pending_destroys:
		if _frame_count >= entry["destroy_at_frame"]:
			var node: Node = entry["node"]
			if is_instance_valid(node):
				node.queue_free()
		else:
			still_pending.append(entry)
	_pending_destroys = still_pending

	# Spawn bursts
	if _burst_accumulator >= BURST_INTERVAL:
		_burst_accumulator -= BURST_INTERVAL
		_spawn_burst()

func _spawn_burst() -> void:
	for i in range(BURST_SIZE):
		var mi := MeshInstance3D.new()
		mi.name = "RIDTorture_%d" % _entity_counter

		# Alternate mesh types to verify no mesh bleed
		var mesh: Mesh
		if _entity_counter % 3 == 0:
			mesh = BoxMesh.new()
		elif _entity_counter % 3 == 1:
			mesh = SphereMesh.new()
		else:
			mesh = CylinderMesh.new()
		mi.mesh = mesh

		# Distinct material per entity to verify no material bleed
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color.from_hsv(
			fmod(float(_entity_counter) * 0.1, 1.0), 0.9, 0.9
		)
		mi.material_override = mat

		# Distinct position per entity
		mi.position = Vector3(
			fmod(float(_entity_counter) * 0.3, 10.0) - 5.0,
			fmod(float(_entity_counter) * 0.2, 3.0),
			fmod(float(_entity_counter) * 0.4, 10.0) - 5.0
		)

		add_child(mi)

		# Schedule destruction after DESTROY_DELAY_FRAMES
		_pending_destroys.append({
			"node": mi,
			"destroy_at_frame": _frame_count + DESTROY_DELAY_FRAMES,
		})

		_entity_counter += 1
