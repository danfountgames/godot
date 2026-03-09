## C8: Deep Hierarchy — Transform hierarchy validation
##
## Content: 10 levels of parent-child nesting, animation on root.
## Purpose: Validates flat model doesn't double-apply transforms.
## Hostile: No
##
## Used by: Proof A (extraction truthfulness)
##
## CRITICAL NEGATIVE TEST: Parent transform must not double-apply.
## 3-level hierarchy: root rotated 45 deg, child offset (1,0,0),
## grandchild offset (0,1,0). Grandchild world position must match
## Godot's computed world transform exactly.
extends Node3D

const DEPTH := 10

func _ready() -> void:
	_build_hierarchy(self, 0)

	# Animate the root with rotation
	var tween := create_tween().set_loops()
	tween.tween_property(self, "rotation:y", TAU, 4.0).from(0.0)

func _build_hierarchy(parent: Node3D, level: int) -> void:
	if level >= DEPTH:
		return

	var node := Node3D.new()
	node.name = "Level_%d" % level
	node.position = Vector3(1.0, 0.0, 0.0)
	node.rotation_degrees = Vector3(0, 36.0, 0)  # 36 deg per level

	# Add a visible mesh at each level
	var mi := MeshInstance3D.new()
	mi.name = "Mesh_%d" % level
	var mesh := BoxMesh.new()
	mesh.size = Vector3.ONE * (1.0 - level * 0.08)
	mi.mesh = mesh

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color.from_hsv(float(level) / DEPTH, 0.8, 0.9)
	mi.material_override = mat

	node.add_child(mi)
	parent.add_child(node)

	_build_hierarchy(node, level + 1)
