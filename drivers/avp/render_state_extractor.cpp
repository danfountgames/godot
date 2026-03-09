/**************************************************************************/
/*  render_state_extractor.cpp                                            */
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

#include "render_state_extractor.h"

#include "core/math/math_defs.h"
#include "core/string/print_string.h"
#include "input_bridge_avp.h"
#include "material_storage_avp.h"
#include "mesh_storage_avp.h"
#include "rasterizer_scene_avp.h"

RenderStateExtractor::RenderStateExtractor() {
}

RenderStateExtractor::~RenderStateExtractor() {
	clear();
}

void RenderStateExtractor::transform_to_float16(const Transform3D &p_transform, float *r_dst) {
	// Godot Transform3D is a 3x4 matrix (basis + origin).
	// We output a column-major 4x4 matrix for RealityKit.
	const Basis &b = p_transform.basis;
	const Vector3 &o = p_transform.origin;

	// Column 0
	r_dst[0] = b[0][0];
	r_dst[1] = b[1][0];
	r_dst[2] = b[2][0];
	r_dst[3] = 0.0f;

	// Column 1
	r_dst[4] = b[0][1];
	r_dst[5] = b[1][1];
	r_dst[6] = b[2][1];
	r_dst[7] = 0.0f;

	// Column 2
	r_dst[8] = b[0][2];
	r_dst[9] = b[1][2];
	r_dst[10] = b[2][2];
	r_dst[11] = 0.0f;

	// Column 3 (translation)
	r_dst[12] = o.x;
	r_dst[13] = o.y;
	r_dst[14] = o.z;
	r_dst[15] = 1.0f;
}

bool RenderStateExtractor::transforms_equal(const float *p_a, const float *p_b) {
	for (int i = 0; i < 16; i++) {
		if (!Math::is_equal_approx(p_a[i], p_b[i])) {
			return false;
		}
	}
	return true;
}

uint64_t RenderStateExtractor::entity_id_from_instance(const RenderGeometryInstance *p_instance) {
	// Use the pointer address as a stable per-frame identifier.
	// This is safe because geometry instances are allocated via PagedAllocator
	// and their addresses are stable across frames while they exist.
	return reinterpret_cast<uint64_t>(p_instance);
}

void RenderStateExtractor::emit_batched_transforms(const LocalVector<uint64_t> &p_ids, const LocalVector<float> &p_transforms) {
	if (p_ids.size() == 0) {
		return;
	}
	godot_avp_entities_update_transforms(
			p_ids.ptr(),
			p_transforms.ptr(),
			p_ids.size());
}

void RenderStateExtractor::process_frame(const PagedArray<RenderGeometryInstance *> &p_instances) {
	current_state.clear();

	// Phase 1: Build current frame state from geometry instances.
	for (uint32_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstance *gi = p_instances[i];
		if (!gi) {
			continue;
		}

		RenderEntity entity;
		entity.id = entity_id_from_instance(gi);
		entity.visible = true;
		entity.layer_mask = 1; // Default; will be set by GeometryInstanceAVP.

		// Extract transform.
		Transform3D xform = gi->get_transform();
		transform_to_float16(xform, entity.world_transform);

		// Extract mesh_id and material_id from the GeometryInstanceAVP subclass.
		RasterizerSceneAVP::GeometryInstanceAVP *avp_gi =
				static_cast<RasterizerSceneAVP::GeometryInstanceAVP *>(gi);

		entity.mesh_id = avp_gi->mesh_id;

		// Resolve material: override > surface[0] material > 0.
		if (avp_gi->material_override.is_valid()) {
			entity.material_id = avp_gi->material_override.get_id();
		} else if (avp_gi->surface_materials.size() > 0 && avp_gi->surface_materials[0].is_valid()) {
			entity.material_id = avp_gi->surface_materials[0].get_id();
		} else {
			// Try to get the material from the mesh surface itself.
			RID base_rid = avp_gi->base;
			if (MeshStorageAVP::get_singleton()) {
				RID surface_mat = MeshStorageAVP::get_singleton()->mesh_surface_get_material(base_rid, 0);
				entity.material_id = surface_mat.is_valid() ? surface_mat.get_id() : 0;
			}
		}

		entity.layer_mask = avp_gi->layer_mask;
		entity.visible = avp_gi->visible;

		// Cache AABB size for collision shape generation (Phase 7.5).
		AABB entity_aabb = avp_gi->get_aabb();
		entity.aabb_size = entity_aabb.size;

		// Volume camera culling (Phase 3.2): in bounded mode, hide entities
		// whose AABB falls entirely outside the volume bounds.
		if (entity.visible && volume_culling_enabled && volume_mode == 0) {
			// Transform the local AABB to world space using the entity's transform.
			Transform3D xform_cull = avp_gi->get_transform();
			AABB world_aabb = xform_cull.xform(entity_aabb);
			if (!_is_in_volume(world_aabb)) {
				entity.visible = false;
			}
		}

		current_state.insert(entity.id, entity);
	}

	// Phase 2: Diff against previous frame.

	// Collect batched transform updates.
	LocalVector<uint64_t> batch_ids;
	LocalVector<float> batch_transforms;

	// Check for new and modified entities.
	for (const KeyValue<uint64_t, RenderEntity> &E : current_state) {
		const RenderEntity &cur = E.value;

		const RenderEntity *prev = previous_state.getptr(cur.id);
		if (!prev) {
			// New entity -- ensure resources are uploaded before creation.
			_ensure_resources_uploaded(cur.mesh_id, cur.material_id);

			godot_avp_entity_create(cur.id, cur.mesh_id, cur.material_id, cur.world_transform);
			if (!cur.visible) {
				godot_avp_entity_set_visible(cur.id, 0);
			}

			// Phase 7.5: Auto-generate collision shape and mark as interactive.
			// All visible entities with meshes get a bounding-box collision shape
			// so they can receive spatial input events on visionOS.
			// Priority: explicit CollisionShape3D > AABB fallback > small default box.
			if (cur.visible && cur.mesh_id != 0) {
				InputBridgeAVP *input_bridge = InputBridgeAVP::get_singleton();
				if (input_bridge) {
					// Use the cached AABB size from Phase 1 for box collision.
					input_bridge->auto_collision_from_aabb(cur.id, cur.aabb_size);
					input_bridge->set_entity_interactive(cur.id, true);
				}
			}
		} else {
			// Existing entity -- check what changed.
			uint32_t dirty = DIRTY_NONE;

			if (!transforms_equal(cur.world_transform, prev->world_transform)) {
				dirty |= DIRTY_TRANSFORM;
			}
			if (cur.mesh_id != prev->mesh_id) {
				dirty |= DIRTY_MESH;
			}
			if (cur.material_id != prev->material_id) {
				dirty |= DIRTY_MATERIAL;
			}
			if (cur.visible != prev->visible) {
				dirty |= DIRTY_VISIBILITY;
			}

			// Emit individual property updates for non-transform changes.
			if (dirty & DIRTY_MESH) {
				_ensure_mesh_uploaded(cur.mesh_id);
				godot_avp_entity_set_mesh(cur.id, cur.mesh_id);
			}
			if (dirty & DIRTY_MATERIAL) {
				_ensure_material_uploaded(cur.material_id);
				godot_avp_entity_set_material(cur.id, cur.material_id);
			}
			if (dirty & DIRTY_VISIBILITY) {
				godot_avp_entity_set_visible(cur.id, cur.visible ? 1 : 0);
			}

			// Batch transform updates for efficiency.
			if (dirty & DIRTY_TRANSFORM) {
				batch_ids.push_back(cur.id);
				for (int j = 0; j < 16; j++) {
					batch_transforms.push_back(cur.world_transform[j]);
				}
			}
		}
	}

	// Emit batched transform updates.
	emit_batched_transforms(batch_ids, batch_transforms);

	// Phase 3: Check for destroyed entities (present in previous but not current).
	for (const KeyValue<uint64_t, RenderEntity> &E : previous_state) {
		if (!current_state.has(E.key)) {
			// Clean up input state for the destroyed entity.
			InputBridgeAVP *input_bridge = InputBridgeAVP::get_singleton();
			if (input_bridge) {
				input_bridge->remove_entity(E.key);
			}
			godot_avp_entity_destroy(E.key);
		}
	}

	// Swap state: current becomes previous for next frame.
	previous_state = current_state;
}

void RenderStateExtractor::clear() {
	for (const KeyValue<uint64_t, RenderEntity> &E : previous_state) {
		godot_avp_entity_destroy(E.key);
	}
	previous_state.clear();
	current_state.clear();
}

void RenderStateExtractor::_ensure_mesh_uploaded(uint64_t p_mesh_id) {
	if (p_mesh_id == 0) {
		return;
	}
	MeshStorageAVP *mesh_storage = MeshStorageAVP::get_singleton();
	if (mesh_storage) {
		// Reconstruct RID from the stored ID.
		RID mesh_rid = RID::from_uint64(p_mesh_id);
		mesh_storage->ensure_mesh_uploaded(mesh_rid);
	}
}

void RenderStateExtractor::_ensure_material_uploaded(uint64_t p_material_id) {
	if (p_material_id == 0) {
		return;
	}
	MaterialStorageAVP *mat_storage = MaterialStorageAVP::get_singleton();
	if (mat_storage) {
		RID mat_rid = RID::from_uint64(p_material_id);
		mat_storage->ensure_material_uploaded(mat_rid);
	}
}

void RenderStateExtractor::_ensure_resources_uploaded(uint64_t p_mesh_id, uint64_t p_material_id) {
	_ensure_mesh_uploaded(p_mesh_id);
	_ensure_material_uploaded(p_material_id);
}

// ---- Volume Camera Culling ----

void RenderStateExtractor::set_volume_bounds(const Vector3 &p_size) {
	volume_size = p_size;
	_update_volume_aabb();
	// Forward to Swift side.
	godot_avp_volume_camera_set_bounds(p_size.x, p_size.y, p_size.z);
}

void RenderStateExtractor::set_volume_transform(const Transform3D &p_transform) {
	volume_transform = p_transform;
	_update_volume_aabb();
	// Forward to Swift side.
	float mat[16];
	transform_to_float16(p_transform, mat);
	godot_avp_volume_camera_set_transform(mat);
}

void RenderStateExtractor::set_volume_mode(int p_mode) {
	volume_mode = p_mode;
	godot_avp_volume_camera_set_mode(p_mode);
}

void RenderStateExtractor::set_volume_culling_enabled(bool p_enabled) {
	volume_culling_enabled = p_enabled;
}

void RenderStateExtractor::_update_volume_aabb() {
	// Compute a world-space AABB centered at the volume transform origin.
	Vector3 half = volume_size * 0.5f;
	AABB local_aabb = AABB(-half, volume_size);
	// Transform the local AABB corners into world space for an axis-aligned
	// bounding box test. This is an approximation; for rotated volumes the
	// AABB will be larger than needed, but it's conservative (no false culls).
	volume_aabb = volume_transform.xform(local_aabb);
}

bool RenderStateExtractor::_is_in_volume(const AABB &p_aabb) const {
	return volume_aabb.intersects(p_aabb);
}

uint32_t RenderStateExtractor::get_entity_count() const {
	return previous_state.size();
}
