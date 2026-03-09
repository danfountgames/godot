/**************************************************************************/
/*  avp_bridge.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                 */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

/**
 * Bridge implementation for the Godot -> Apple Vision Pro RealityKit renderer.
 *
 * This file implements all extern "C" functions declared in avp_bridge.h.
 * Each function routes to a Swift-side callback that is registered during
 * initialization. When running without a Swift backend (e.g., during
 * development/testing on non-visionOS platforms), calls are no-ops.
 *
 * The Swift side registers its callbacks by calling the registration
 * functions (not yet defined here -- will be added when the Swift
 * layer is implemented).
 */

#include "avp_bridge.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Swift-side function pointers.
// These are set by the Swift layer during initialization.
// When null, calls are no-ops (safe for testing on non-visionOS).
// ---------------------------------------------------------------------------

// Entity lifecycle.
static void (*s_entity_create)(uint64_t, uint64_t, uint64_t, const float *) = nullptr;
static void (*s_entity_destroy)(uint64_t) = nullptr;
static void (*s_entity_set_transform)(uint64_t, const float *) = nullptr;
static void (*s_entity_set_visible)(uint64_t, int) = nullptr;
static void (*s_entity_set_mesh)(uint64_t, uint64_t) = nullptr;
static void (*s_entity_set_material)(uint64_t, uint64_t) = nullptr;

// Batched transforms.
static void (*s_entities_update_transforms)(const uint64_t *, const float *, uint32_t) = nullptr;

// Mesh.
static void (*s_mesh_create)(uint64_t, const float *, const float *, const float *, const float *, const float *, const uint32_t *, uint32_t, uint32_t, uint32_t) = nullptr;
static void (*s_mesh_destroy)(uint64_t) = nullptr;
static void (*s_mesh_update_vertices)(uint64_t, const float *, const float *, uint32_t) = nullptr;

// Texture.
static void (*s_texture_create)(uint64_t, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, int) = nullptr;
static void (*s_texture_destroy)(uint64_t) = nullptr;

// Material.
static void (*s_material_create_pbr)(uint64_t, uint64_t, float, float, float, float, float, uint64_t, float, uint64_t, uint64_t, float, uint64_t, float, float, float, float, uint64_t, float, float, int, float, int) = nullptr;
static void (*s_material_create_unlit)(uint64_t, uint64_t, float, float, float, float, int, float, int) = nullptr;
static void (*s_material_create_custom)(uint64_t, const char *, const char *) = nullptr;
static void (*s_material_destroy)(uint64_t) = nullptr;

// Frame sync.
static void (*s_frame_begin)(void) = nullptr;
static void (*s_frame_end)(void) = nullptr;

// Volume camera.
static void (*s_volume_camera_set_bounds)(float, float, float) = nullptr;
static void (*s_volume_camera_set_transform)(const float *) = nullptr;
static void (*s_volume_camera_set_mode)(int) = nullptr;

// Instancing.
static void (*s_instancing_update)(uint64_t, uint64_t, uint64_t, const float *, uint32_t) = nullptr;
static void (*s_instancing_destroy)(uint64_t) = nullptr;

// Collision.
static void (*s_entity_set_collision)(uint64_t, int, const float *, uint32_t) = nullptr;
static void (*s_entity_set_interactive)(uint64_t, int) = nullptr;

// Diagnostics.
static uint32_t (*s_get_entity_count)(void) = nullptr;
static uint64_t (*s_get_memory_estimate)(void) = nullptr;

// Input callbacks (Swift -> C++).
static godot_avp_tap_callback s_tap_callback = nullptr;
static godot_avp_drag_callback s_drag_callback = nullptr;

// Entity count tracked on the C++ side as a fallback.
static uint32_t s_local_entity_count = 0;

// ---------------------------------------------------------------------------
// extern "C" implementations
// ---------------------------------------------------------------------------

extern "C" {

// Entity lifecycle.

void godot_avp_entity_create(uint64_t id, uint64_t mesh_id, uint64_t material_id,
		const float *transform_16) {
	s_local_entity_count++;
	if (s_entity_create) {
		s_entity_create(id, mesh_id, material_id, transform_16);
	}
}

void godot_avp_entity_destroy(uint64_t id) {
	if (s_local_entity_count > 0) {
		s_local_entity_count--;
	}
	if (s_entity_destroy) {
		s_entity_destroy(id);
	}
}

void godot_avp_entity_set_transform(uint64_t id, const float *transform_16) {
	if (s_entity_set_transform) {
		s_entity_set_transform(id, transform_16);
	}
}

void godot_avp_entity_set_visible(uint64_t id, int visible) {
	if (s_entity_set_visible) {
		s_entity_set_visible(id, visible);
	}
}

void godot_avp_entity_set_mesh(uint64_t id, uint64_t mesh_id) {
	if (s_entity_set_mesh) {
		s_entity_set_mesh(id, mesh_id);
	}
}

void godot_avp_entity_set_material(uint64_t id, uint64_t material_id) {
	if (s_entity_set_material) {
		s_entity_set_material(id, material_id);
	}
}

// Batched transform update.

void godot_avp_entities_update_transforms(const uint64_t *ids,
		const float *transforms,
		uint32_t count) {
	if (s_entities_update_transforms) {
		s_entities_update_transforms(ids, transforms, count);
	}
}

// Mesh data upload.

void godot_avp_mesh_create(uint64_t mesh_id,
		const float *positions, const float *normals,
		const float *uvs, const float *tangents,
		const float *colors,
		const uint32_t *indices,
		uint32_t vertex_count, uint32_t index_count,
		uint32_t surface_count) {
	if (s_mesh_create) {
		s_mesh_create(mesh_id, positions, normals, uvs, tangents, colors,
				indices, vertex_count, index_count, surface_count);
	}
}

void godot_avp_mesh_destroy(uint64_t mesh_id) {
	if (s_mesh_destroy) {
		s_mesh_destroy(mesh_id);
	}
}

void godot_avp_mesh_update_vertices(uint64_t mesh_id,
		const float *positions, const float *normals,
		uint32_t vertex_count) {
	if (s_mesh_update_vertices) {
		s_mesh_update_vertices(mesh_id, positions, normals, vertex_count);
	}
}

// Texture data upload.

void godot_avp_texture_create(uint64_t texture_id,
		const uint8_t *pixels,
		uint32_t width, uint32_t height,
		uint32_t format, uint32_t mipmaps,
		int is_srgb) {
	if (s_texture_create) {
		s_texture_create(texture_id, pixels, width, height, format, mipmaps, is_srgb);
	}
}

void godot_avp_texture_destroy(uint64_t texture_id) {
	if (s_texture_destroy) {
		s_texture_destroy(texture_id);
	}
}

// Material descriptors.

void godot_avp_material_create_pbr(uint64_t material_id,
		uint64_t albedo_tex, float albedo_r, float albedo_g,
		float albedo_b, float albedo_a,
		float roughness, uint64_t roughness_tex,
		float metallic, uint64_t metallic_tex,
		uint64_t normal_tex, float normal_scale,
		uint64_t emission_tex,
		float emission_r, float emission_g, float emission_b,
		float emission_energy,
		uint64_t ao_tex,
		float clearcoat, float clearcoat_roughness,
		int alpha_mode, float alpha_scissor_threshold,
		int cull_mode) {
	if (s_material_create_pbr) {
		s_material_create_pbr(material_id, albedo_tex, albedo_r, albedo_g,
				albedo_b, albedo_a, roughness, roughness_tex,
				metallic, metallic_tex, normal_tex, normal_scale,
				emission_tex, emission_r, emission_g, emission_b,
				emission_energy, ao_tex, clearcoat, clearcoat_roughness,
				alpha_mode, alpha_scissor_threshold, cull_mode);
	}
}

void godot_avp_material_create_unlit(uint64_t material_id,
		uint64_t albedo_tex,
		float albedo_r, float albedo_g, float albedo_b, float albedo_a,
		int alpha_mode, float alpha_scissor_threshold,
		int cull_mode) {
	if (s_material_create_unlit) {
		s_material_create_unlit(material_id, albedo_tex, albedo_r, albedo_g,
				albedo_b, albedo_a, alpha_mode, alpha_scissor_threshold,
				cull_mode);
	}
}

void godot_avp_material_create_custom(uint64_t material_id,
		const char *shader_name,
		const char *params_json) {
	if (s_material_create_custom) {
		s_material_create_custom(material_id, shader_name, params_json);
	}
}

void godot_avp_material_destroy(uint64_t material_id) {
	if (s_material_destroy) {
		s_material_destroy(material_id);
	}
}

// Frame synchronization.

void godot_avp_frame_begin(void) {
	if (s_frame_begin) {
		s_frame_begin();
	}
}

void godot_avp_frame_end(void) {
	if (s_frame_end) {
		s_frame_end();
	}
}

// VolumeCamera.

void godot_avp_volume_camera_set_bounds(float size_x, float size_y, float size_z) {
	if (s_volume_camera_set_bounds) {
		s_volume_camera_set_bounds(size_x, size_y, size_z);
	}
}

void godot_avp_volume_camera_set_transform(const float *transform_16) {
	if (s_volume_camera_set_transform) {
		s_volume_camera_set_transform(transform_16);
	}
}

void godot_avp_volume_camera_set_mode(int mode) {
	if (s_volume_camera_set_mode) {
		s_volume_camera_set_mode(mode);
	}
}

// Spatial input callbacks.

void godot_avp_set_tap_callback(godot_avp_tap_callback callback) {
	s_tap_callback = callback;
}

void godot_avp_set_drag_callback(godot_avp_drag_callback callback) {
	s_drag_callback = callback;
}

// Instancing.

void godot_avp_instancing_update(uint64_t group_id, uint64_t mesh_id,
		uint64_t material_id,
		const float *transforms,
		uint32_t instance_count) {
	if (s_instancing_update) {
		s_instancing_update(group_id, mesh_id, material_id, transforms, instance_count);
	}
}

void godot_avp_instancing_destroy(uint64_t group_id) {
	if (s_instancing_destroy) {
		s_instancing_destroy(group_id);
	}
}

// Collision shape forwarding.

void godot_avp_entity_set_collision(uint64_t entity_id, int shape_type,
		const float *data, uint32_t data_count) {
	if (s_entity_set_collision) {
		s_entity_set_collision(entity_id, shape_type, data, data_count);
	}
}

void godot_avp_entity_set_interactive(uint64_t entity_id, int interactive) {
	if (s_entity_set_interactive) {
		s_entity_set_interactive(entity_id, interactive);
	}
}

// Diagnostics.

uint32_t godot_avp_get_entity_count(void) {
	if (s_get_entity_count) {
		return s_get_entity_count();
	}
	return s_local_entity_count;
}

uint64_t godot_avp_get_memory_estimate(void) {
	if (s_get_memory_estimate) {
		return s_get_memory_estimate();
	}
	return 0;
}

} // extern "C"
