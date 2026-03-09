## C5: Create/Destroy Storm — Entity churn stress test
##
## Content: Script spawns 50 entities/sec and destroys 50 entities/sec.
## Purpose: Churn, entity pool, ID stability testing.
## Hostile: No
##
## Used by: Proof C (frame budget), Proof E (failure envelopes)
##
## Expected result: entityRegistry.count oscillates around a stable mean (±10%).
## No memory growth trend. No stale entity resurrection.
extends Node3D

const SPAWN_RATE := 50  # entities per second
const MAX_LIFETIME := 2.0  # seconds before destruction

var _entity_count := 0
var _spawn_accumulator := 0.0
var _mesh: Mesh

func _ready() -> void:
	_mesh = SphereMesh.new()

func _process(delta: float) -> void:
	_spawn_accumulator += delta * SPAWN_RATE

	while _spawn_accumulator >= 1.0:
		_spawn_entity()
		_spawn_accumulator -= 1.0

func _spawn_entity() -> void:
	var mi := MeshInstance3D.new()
	mi.name = "ChurnEntity_%d" % _entity_count
	mi.mesh = _mesh

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color.from_hsv(randf(), 0.8, 0.9)
	mi.material_override = mat

	mi.position = Vector3(
		randf_range(-5, 5),
		randf_range(0, 3),
		randf_range(-5, 5)
	)

	add_child(mi)
	_entity_count += 1

	# Schedule destruction
	var timer := get_tree().create_timer(MAX_LIFETIME)
	timer.timeout.connect(func(): _destroy_entity(mi))

func _destroy_entity(node: Node) -> void:
	if is_instance_valid(node):
		node.queue_free()
