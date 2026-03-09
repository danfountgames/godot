/**************************************************************************/
/*  avp_bridge.h                                                          */
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

#ifndef AVP_BRIDGE_H
#define AVP_BRIDGE_H

/**
 * Godot → Apple Vision Pro RealityKit Bridge
 *
 * extern "C" interface between Godot's C++ rendering backend (drivers/avp/)
 * and the Swift RealityKit presentation layer (swift/Sources/GodotAVP/).
 *
 * Design: Godot is authoritative for simulation (transforms, physics,
 * animation, scripting). RealityKit is a pure presentation layer.
 * The bridge sends final render state, not scene hierarchy.
 *
 * See RK_PLAN.md §2.9 for architecture rationale.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Entity lifecycle
// ---------------------------------------------------------------------------

/**
 * Create a new RealityKit entity.
 *
 * @param id             Stable identifier derived from Godot instance RID.
 * @param mesh_id        RID of the mesh resource.
 * @param material_id    RID of the primary material.
 * @param transform_16   Column-major 4×4 transform matrix (16 floats).
 */
void godot_avp_entity_create(uint64_t id, uint64_t mesh_id, uint64_t material_id,
		const float *transform_16);

/**
 * Destroy a RealityKit entity. Removes from scene and frees resources
 * if no other entity references them.
 */
void godot_avp_entity_destroy(uint64_t id);

/**
 * Set the world transform of an entity.
 *
 * @param id             Entity identifier.
 * @param transform_16   Column-major 4×4 transform matrix (16 floats).
 */
void godot_avp_entity_set_transform(uint64_t id, const float *transform_16);

/**
 * Set the visibility of an entity.
 *
 * @param id       Entity identifier.
 * @param visible  1 = visible, 0 = hidden.
 */
void godot_avp_entity_set_visible(uint64_t id, int visible);

/**
 * Change the mesh assigned to an entity.
 */
void godot_avp_entity_set_mesh(uint64_t id, uint64_t mesh_id);

/**
 * Change the material assigned to an entity.
 */
void godot_avp_entity_set_material(uint64_t id, uint64_t material_id);

// ---------------------------------------------------------------------------
// Batched transform update (preferred — one call per frame)
// ---------------------------------------------------------------------------

/**
 * Update transforms for multiple entities in a single call.
 *
 * @param ids          Array of entity IDs.
 * @param transforms   Packed array of column-major 4×4 matrices (16 floats each).
 * @param count        Number of entities to update.
 */
void godot_avp_entities_update_transforms(const uint64_t *ids,
		const float *transforms,
		uint32_t count);

// ---------------------------------------------------------------------------
// Mesh data upload
// ---------------------------------------------------------------------------

/**
 * Upload mesh data for a given mesh ID. Creates a LowLevelMesh on the
 * Swift side.
 *
 * @param mesh_id       Unique mesh identifier (from Godot RID).
 * @param positions     Vertex positions (3 floats per vertex: x, y, z).
 * @param normals       Vertex normals (3 floats per vertex, may be NULL).
 * @param uvs           Texture coordinates (2 floats per vertex, may be NULL).
 * @param tangents      Tangent vectors (4 floats per vertex: x, y, z, w; may be NULL).
 * @param colors        Vertex colors (4 floats per vertex: r, g, b, a; may be NULL).
 * @param indices       Triangle indices (3 per triangle).
 * @param vertex_count  Number of vertices.
 * @param index_count   Number of indices.
 */
void godot_avp_mesh_create(uint64_t mesh_id,
		const float *positions, const float *normals,
		const float *uvs, const float *tangents,
		const float *colors,
		const uint32_t *indices,
		uint32_t vertex_count, uint32_t index_count,
		uint32_t surface_count);

/**
 * Destroy a cached mesh resource.
 */
void godot_avp_mesh_destroy(uint64_t mesh_id);

/**
 * Update vertex data for an existing mesh (e.g., skinned mesh streaming).
 *
 * @param mesh_id       Mesh to update.
 * @param positions     New vertex positions.
 * @param normals       New vertex normals (may be NULL to keep existing).
 * @param vertex_count  Number of vertices.
 */
void godot_avp_mesh_update_vertices(uint64_t mesh_id,
		const float *positions, const float *normals,
		uint32_t vertex_count);

// ---------------------------------------------------------------------------
// Texture data upload
// ---------------------------------------------------------------------------

/**
 * Upload texture data.
 *
 * @param texture_id  Unique texture identifier.
 * @param pixels      Raw pixel data.
 * @param width       Texture width in pixels.
 * @param height      Texture height in pixels.
 * @param format      Pixel format (maps to MTLPixelFormat):
 *                    0 = RGBA8 Unorm
 *                    1 = RGBA8 sRGB
 *                    2 = RGBA16 Float
 *                    3 = R8 Unorm
 *                    4 = RG8 Unorm
 *                    5 = BC7 Unorm (compressed)
 * @param mipmaps     Number of mipmap levels (1 = no mipmaps).
 * @param is_srgb     Whether the texture is in sRGB color space.
 */
void godot_avp_texture_create(uint64_t texture_id,
		const uint8_t *pixels,
		uint32_t width, uint32_t height,
		uint32_t format, uint32_t mipmaps,
		int is_srgb);

/**
 * Destroy a cached texture resource.
 */
void godot_avp_texture_destroy(uint64_t texture_id);

// ---------------------------------------------------------------------------
// Material descriptors
// ---------------------------------------------------------------------------

/**
 * Create a PBR material mapped from Godot's StandardMaterial3D.
 * See RK_PLAN.md §2.13 for the property mapping table.
 *
 * @param material_id        Unique material identifier.
 * @param albedo_tex         Albedo texture ID (0 = no texture).
 * @param albedo_r/g/b/a     Albedo tint color.
 * @param roughness          Roughness scalar [0..1].
 * @param roughness_tex      Roughness texture ID (0 = no texture).
 * @param metallic           Metallic scalar [0..1].
 * @param metallic_tex       Metallic texture ID (0 = no texture).
 * @param normal_tex         Normal map texture ID (0 = no normal map).
 * @param normal_scale       Normal map intensity.
 * @param emission_tex       Emission texture ID (0 = no emission).
 * @param emission_r/g/b     Emission color.
 * @param emission_energy    Emission intensity multiplier.
 * @param ao_tex             Ambient occlusion texture ID.
 * @param clearcoat          Clearcoat intensity [0..1].
 * @param clearcoat_roughness Clearcoat roughness [0..1].
 * @param alpha_mode         0=opaque, 1=transparent, 2=alpha_scissor.
 * @param alpha_scissor_threshold  Alpha cutoff for scissor mode.
 * @param cull_mode          0=back, 1=front, 2=disabled (double-sided).
 */
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
		int cull_mode);

/**
 * Create an unlit material.
 */
void godot_avp_material_create_unlit(uint64_t material_id,
		uint64_t albedo_tex,
		float albedo_r, float albedo_g, float albedo_b, float albedo_a,
		int alpha_mode, float alpha_scissor_threshold,
		int cull_mode);

/**
 * Create a material using a registered replacement shader.
 * See RK_PLAN.md §2.13 Tier 3 for the @rk_shader annotation system.
 *
 * @param material_id   Unique material identifier.
 * @param shader_name   Name of the registered Swift shader (e.g., "ToonMaterial").
 * @param params_json   JSON-encoded shader parameters (may be NULL).
 */
void godot_avp_material_create_custom(uint64_t material_id,
		const char *shader_name,
		const char *params_json);

/**
 * Destroy a cached material.
 */
void godot_avp_material_destroy(uint64_t material_id);

// ---------------------------------------------------------------------------
// Frame synchronization
// ---------------------------------------------------------------------------

/** Signal the beginning of a new frame. */
void godot_avp_frame_begin(void);

/** Signal the end of a frame — all entity updates for this frame are complete. */
void godot_avp_frame_end(void);

// ---------------------------------------------------------------------------
// VolumeCamera
// ---------------------------------------------------------------------------

/**
 * Set the bounding dimensions of the spatial volume.
 * In bounded mode, content is clipped to this box.
 *
 * @param size_x/y/z  Volume dimensions in meters.
 */
void godot_avp_volume_camera_set_bounds(float size_x, float size_y, float size_z);

/**
 * Set the world transform of the volume camera.
 */
void godot_avp_volume_camera_set_transform(const float *transform_16);

/**
 * Set the volume mode.
 *
 * @param mode  0 = bounded (volume window), 1 = immersive (immersive space).
 */
void godot_avp_volume_camera_set_mode(int mode);

// ---------------------------------------------------------------------------
// Spatial input callbacks (Swift → C++)
//
// RealityKit is the SOLE authority for spatial input.
// Eye gaze ray is system-private — Godot cannot perform spatial picking.
// See RK_PLAN.md §2.30.
// ---------------------------------------------------------------------------

/**
 * Callback for spatial tap gestures.
 *
 * @param entity_id     RealityKit entity that was tapped (maps to Godot RID).
 * @param hit_pos_x/y/z Hit position in entity-local coordinates.
 * @param hit_normal_x/y/z Surface normal at hit point.
 *        ⚠️ UNPROVEN — public docs confirm hit_position but not surface
 *        normal. If unavailable at prototype time, these will be zero.
 */
typedef void (*godot_avp_tap_callback)(uint64_t entity_id,
		float hit_pos_x, float hit_pos_y, float hit_pos_z,
		float hit_normal_x, float hit_normal_y, float hit_normal_z);

/**
 * Callback for spatial drag gestures.
 *
 * @param entity_id     Entity being dragged.
 * @param phase         0=began, 1=changed, 2=ended, 3=cancelled.
 * @param hit_pos_x/y/z Initial hit position (entity-local).
 * @param translation_x/y/z Cumulative drag translation (world-space).
 */
typedef void (*godot_avp_drag_callback)(uint64_t entity_id, int phase,
		float hit_pos_x, float hit_pos_y, float hit_pos_z,
		float translation_x, float translation_y, float translation_z);

/** Register the tap gesture callback. */
void godot_avp_set_tap_callback(godot_avp_tap_callback callback);

/** Register the drag gesture callback. */
void godot_avp_set_drag_callback(godot_avp_drag_callback callback);

// ---------------------------------------------------------------------------
// Instancing (MultiMesh → MeshInstancesComponent)
// ---------------------------------------------------------------------------

/**
 * Create or update an instanced mesh group.
 *
 * @param group_id       Unique group identifier (from Godot MultiMesh RID).
 * @param mesh_id        The mesh to instance.
 * @param material_id    The material to apply.
 * @param transforms     Packed array of column-major 4×4 matrices.
 * @param instance_count Number of instances.
 */
void godot_avp_instancing_update(uint64_t group_id, uint64_t mesh_id,
		uint64_t material_id,
		const float *transforms,
		uint32_t instance_count);

/**
 * Destroy an instanced mesh group.
 */
void godot_avp_instancing_destroy(uint64_t group_id);

// ---------------------------------------------------------------------------
// Collision shape forwarding (for spatial input)
// See RK_PLAN.md §2.30 — collision shape strategy.
// ---------------------------------------------------------------------------

/**
 * Set collision shape for an entity from Godot CollisionShape3D data.
 *
 * @param entity_id    Entity to set collision on.
 * @param shape_type   0=box, 1=sphere, 2=capsule, 3=convex_hull, 4=static_mesh.
 * @param data         Shape-specific data (size/radius/vertices depending on type).
 * @param data_count   Number of floats in data array.
 */
void godot_avp_entity_set_collision(uint64_t entity_id, int shape_type,
		const float *data, uint32_t data_count);

/**
 * Mark an entity as interactive (adds InputTargetComponent).
 *
 * @param entity_id    Entity to make interactive.
 * @param interactive  1 = interactive, 0 = not interactive.
 */
void godot_avp_entity_set_interactive(uint64_t entity_id, int interactive);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

/** Get the current count of active entities in the RealityKit registry. */
uint32_t godot_avp_get_entity_count(void);

/** Get the current resident memory estimate (bytes) of the Swift stores. */
uint64_t godot_avp_get_memory_estimate(void);

#ifdef __cplusplus
}
#endif

#endif // AVP_BRIDGE_H
