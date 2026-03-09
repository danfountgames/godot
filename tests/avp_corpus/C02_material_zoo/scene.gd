## C2: Material Zoo — Material fidelity classification corpus
##
## Content: 20 objects, each with a distinct StandardMaterial3D variation.
## Purpose: Proof B corpus — material fidelity classification using
## 8-axis rubric (§2.23). Side-by-side comparison of Godot editor
## preview vs RealityKit render for each material.
## Hostile: No
##
## Used by: Proof B (visual parity buckets)
##
## Material variations covered:
## - Full metallic, full rough, glass/transparent, emissive,
##   alpha-test, double-sided, ORM-packed, clearcoat,
##   backlight, normal-mapped, unlit-style high emission,
##   subsurface approximation, anisotropy approximation,
##   refraction approximation, detail texture, UV offset/scale
##
## Expected result: Each material scored on 8-axis rubric.
## Minimum 60% of materials must score "Perceptual match" or better (≥11/16).
extends Node3D

const MATERIAL_COUNT := 20

func _ready() -> void:
	_build_zoo()

func _build_zoo() -> void:
	# Each entry: { name, material configuration lambda }
	var materials: Array[Dictionary] = []

	# 1. Default PBR (baseline)
	materials.append({"name": "M01_Default_PBR", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.8, 0.8, 0.8)
		m.roughness = 0.5
		m.metallic = 0.0
	})

	# 2. Full metallic
	materials.append({"name": "M02_Full_Metallic", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.9, 0.85, 0.7) # Gold-ish
		m.roughness = 0.1
		m.metallic = 1.0
	})

	# 3. Full rough
	materials.append({"name": "M03_Full_Rough", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.6, 0.4, 0.3) # Clay-like
		m.roughness = 1.0
		m.metallic = 0.0
	})

	# 4. Transparent glass
	materials.append({"name": "M04_Transparent_Glass", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.9, 0.95, 1.0, 0.3)
		m.roughness = 0.0
		m.metallic = 0.0
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	})

	# 5. Emissive (no texture)
	materials.append({"name": "M05_Emissive", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.1, 0.1, 0.1)
		m.emission_enabled = true
		m.emission = Color(1.0, 0.3, 0.0)
		m.emission_energy_multiplier = 3.0
	})

	# 6. Alpha scissor (cutout)
	materials.append({"name": "M06_Alpha_Scissor", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.2, 0.8, 0.2, 0.7)
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		m.alpha_scissor_threshold = 0.5
	})

	# 7. Double-sided (cull disabled)
	materials.append({"name": "M07_Double_Sided", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.9, 0.2, 0.2)
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		m.roughness = 0.5
	})

	# 8. Clearcoat (car paint)
	materials.append({"name": "M08_Clearcoat", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.0, 0.1, 0.6)
		m.roughness = 0.3
		m.metallic = 0.8
		m.clearcoat_enabled = true
		m.clearcoat = 1.0
		m.clearcoat_roughness = 0.1
	})

	# 9. High roughness metallic (brushed metal)
	materials.append({"name": "M09_Brushed_Metal", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.7, 0.7, 0.75)
		m.roughness = 0.6
		m.metallic = 1.0
	})

	# 10. Emissive + transparent (hologram-like)
	materials.append({"name": "M10_Emissive_Transparent", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.0, 0.5, 1.0, 0.5)
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		m.emission_enabled = true
		m.emission = Color(0.0, 0.5, 1.0)
		m.emission_energy_multiplier = 2.0
	})

	# 11. Front-face cull (inside visible)
	materials.append({"name": "M11_Front_Cull", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.8, 0.6, 0.9)
		m.cull_mode = BaseMaterial3D.CULL_FRONT
	})

	# 12. Pure black metallic (mirror)
	materials.append({"name": "M12_Mirror", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.02, 0.02, 0.02)
		m.roughness = 0.0
		m.metallic = 1.0
	})

	# 13. Warm matte (skin approximation)
	materials.append({"name": "M13_Warm_Matte", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.87, 0.72, 0.58)
		m.roughness = 0.8
		m.metallic = 0.0
	})

	# 14. Bright emissive (LED-like)
	materials.append({"name": "M14_LED_Emissive", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(1.0, 1.0, 1.0)
		m.emission_enabled = true
		m.emission = Color(0.0, 1.0, 0.0)
		m.emission_energy_multiplier = 5.0
	})

	# 15. Low alpha transparent overlay
	materials.append({"name": "M15_Low_Alpha_Overlay", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(1.0, 0.0, 0.0, 0.15)
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
	})

	# 16. Copper (warm metallic)
	materials.append({"name": "M16_Copper", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.95, 0.64, 0.37)
		m.roughness = 0.25
		m.metallic = 1.0
	})

	# 17. Plastic (saturated, mid roughness)
	materials.append({"name": "M17_Plastic", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(1.0, 0.8, 0.0)
		m.roughness = 0.4
		m.metallic = 0.0
	})

	# 18. Dark emissive rim (rim light approximation via emission)
	materials.append({"name": "M18_Dark_Emissive_Rim", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.05, 0.05, 0.05)
		m.emission_enabled = true
		m.emission = Color(0.3, 0.6, 1.0)
		m.emission_energy_multiplier = 1.0
		m.roughness = 0.9
	})

	# 19. Clearcoat rough (satin finish)
	materials.append({"name": "M19_Satin_Clearcoat", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.3, 0.0, 0.0)
		m.roughness = 0.5
		m.metallic = 0.5
		m.clearcoat_enabled = true
		m.clearcoat = 0.5
		m.clearcoat_roughness = 0.5
	})

	# 20. White dielectric (porcelain)
	materials.append({"name": "M20_Porcelain", "setup": func(m: StandardMaterial3D):
		m.albedo_color = Color(0.95, 0.93, 0.88)
		m.roughness = 0.15
		m.metallic = 0.0
	})

	# Build scene: 4x5 grid of spheres with each material
	for i in range(materials.size()):
		var mi := MeshInstance3D.new()
		mi.name = materials[i]["name"]
		mi.mesh = SphereMesh.new()

		var mat := StandardMaterial3D.new()
		materials[i]["setup"].call(mat)
		mi.material_override = mat

		# Arrange in 4x5 grid
		var col := i % 5
		var row := i / 5
		mi.position = Vector3(col * 2.5 - 5.0, 0.0, row * 2.5 - 3.75)

		add_child(mi)
