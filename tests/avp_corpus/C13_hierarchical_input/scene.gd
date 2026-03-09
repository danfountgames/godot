## C13: Hierarchical Input Routing
##
## Content: Parent entity with InputTargetComponent, 5 child entities
## each with CollisionComponent and distinct meshes at different
## positions/scales.
## Purpose: 3-part input routing proof (§2.30):
##   (1) Which RealityKit entity was actually targeted
##   (2) Which Godot node becomes the receiver
##   (3) Whether bridged hit position is in correct coordinate space
##
## All three must be verified per-tap in BOTH modes:
##   - Flat model (default): each child's node receives tap
##   - Grouped model (opt-in): parent node receives tap
extends Node3D

## Emitted when any child receives a spatial tap
signal child_tapped(child_name: String, hit_position: Vector3)

func _ready() -> void:
	_build_scene()

func _build_scene() -> void:
	# Parent node (for grouped input testing)
	var parent := Node3D.new()
	parent.name = "InputParent"
	parent.position = Vector3(0, 1, 0)
	add_child(parent)

	# 5 child entities with distinct meshes, positions, and scales
	var child_configs := [
		{"name": "Child_Red_Box", "mesh": BoxMesh.new(), "color": Color.RED,
		 "pos": Vector3(-2, 0, 0), "scale": Vector3(0.5, 0.5, 0.5)},
		{"name": "Child_Green_Sphere", "mesh": SphereMesh.new(), "color": Color.GREEN,
		 "pos": Vector3(-1, 0, 0), "scale": Vector3(0.8, 0.8, 0.8)},
		{"name": "Child_Blue_Cylinder", "mesh": CylinderMesh.new(), "color": Color.BLUE,
		 "pos": Vector3(0, 0, 0), "scale": Vector3(1.0, 1.0, 1.0)},
		{"name": "Child_Yellow_Capsule", "mesh": CapsuleMesh.new(), "color": Color.YELLOW,
		 "pos": Vector3(1, 0, 0), "scale": Vector3(0.6, 0.6, 0.6)},
		{"name": "Child_Purple_Prism", "mesh": PrismMesh.new(), "color": Color.PURPLE,
		 "pos": Vector3(2, 0, 0), "scale": Vector3(1.2, 1.2, 1.2)},
	]

	for config in child_configs:
		var mi := MeshInstance3D.new()
		mi.name = config["name"]
		mi.mesh = config["mesh"]
		mi.position = config["pos"]
		mi.scale = config["scale"]

		var mat := StandardMaterial3D.new()
		mat.albedo_color = config["color"]
		mi.material_override = mat

		# Add collision shape for input
		var body := StaticBody3D.new()
		body.name = "Body_%s" % config["name"]
		var col := CollisionShape3D.new()
		col.shape = BoxShape3D.new()
		body.add_child(col)
		mi.add_child(body)

		parent.add_child(mi)
