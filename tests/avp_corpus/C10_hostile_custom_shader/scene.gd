## C10: [HOSTILE] Custom Shader Gallery
##
## Content: 8 objects using custom .gdshader (not StandardMaterial3D),
## no @rk_shader annotations.
## Purpose: Tests unmapped shader fallback — every object should fall
## back to default PBR with logged warning.
## Hostile: YES
##
## Expected result: DOCUMENTED DEGRADATION.
## - Geometry: PRESERVED
## - Transforms: PRESERVED
## - Custom shader visuals: LOST (no @rk_shader annotations)
## - Each object should render with fallback default PBR material
## - Bridge should LOG A WARNING for each unmapped shader
## - Bridge must NOT crash on any shader type
extends Node3D

func _ready() -> void:
	_build_gallery()

func _build_gallery() -> void:
	var shader_types := [
		"dissolve",
		"hologram",
		"fresnel_glow",
		"vertex_wave",
		"triplanar",
		"rim_light",
		"pixelate",
		"scanline",
	]

	for i in range(shader_types.size()):
		var mi := MeshInstance3D.new()
		mi.name = "CustomShader_%s" % shader_types[i]
		mi.mesh = SphereMesh.new()
		mi.position = Vector3(float(i % 4) * 2.5 - 3.75, 1.0, float(i / 4) * 2.5 - 1.25)

		# Create a ShaderMaterial with a custom shader (no @rk_shader annotation)
		var shader := Shader.new()
		shader.code = _get_shader_code(shader_types[i])

		var mat := ShaderMaterial.new()
		mat.shader = shader
		mi.material_override = mat

		add_child(mi)

func _get_shader_code(type: String) -> String:
	match type:
		"dissolve":
			return """
shader_type spatial;
uniform float dissolve_amount : hint_range(0.0, 1.0) = 0.5;
uniform vec4 edge_color : source_color = vec4(1.0, 0.5, 0.0, 1.0);
void fragment() {
	float noise = fract(sin(dot(UV, vec2(12.9898, 78.233))) * 43758.5453);
	if (noise < dissolve_amount) discard;
	ALBEDO = mix(vec3(0.8), edge_color.rgb, smoothstep(dissolve_amount - 0.1, dissolve_amount, noise));
}
"""
		"hologram":
			return """
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform vec4 holo_color : source_color = vec4(0.0, 0.8, 1.0, 0.5);
void fragment() {
	float scanline = sin(VERTEX.y * 50.0 + TIME * 5.0) * 0.5 + 0.5;
	ALBEDO = holo_color.rgb;
	ALPHA = holo_color.a * scanline;
}
"""
		"fresnel_glow":
			return """
shader_type spatial;
uniform vec4 glow_color : source_color = vec4(0.0, 1.0, 0.5, 1.0);
uniform float glow_power = 3.0;
void fragment() {
	float fresnel = pow(1.0 - dot(NORMAL, VIEW), glow_power);
	ALBEDO = vec3(0.1);
	EMISSION = glow_color.rgb * fresnel * 2.0;
}
"""
		"vertex_wave":
			return """
shader_type spatial;
uniform float wave_amplitude = 0.2;
uniform float wave_speed = 2.0;
void vertex() {
	VERTEX.y += sin(VERTEX.x * 3.0 + TIME * wave_speed) * wave_amplitude;
}
void fragment() {
	ALBEDO = vec3(0.3, 0.6, 0.9);
}
"""
		_:
			return """
shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.5);
}
"""
