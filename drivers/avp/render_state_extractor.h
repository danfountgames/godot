/**************************************************************************/
/*  render_state_extractor.h                                              */
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

#ifndef RENDER_STATE_EXTRACTOR_H
#define RENDER_STATE_EXTRACTOR_H

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/paged_array.h"
#include "servers/rendering/renderer_geometry_instance.h"

#include "avp_bridge.h"

/**
 * Diff engine for the Godot -> RealityKit bridge.
 *
 * Maintains a HashMap of previous frame state per entity.
 * Compares current frame vs previous frame and emits
 * create/update/destroy calls to the bridge via extern "C" functions.
 */
class RenderStateExtractor {
public:
	// Dirty flag bitmask.
	enum DirtyFlags : uint32_t {
		DIRTY_NONE = 0,
		DIRTY_TRANSFORM = (1 << 0),
		DIRTY_MESH = (1 << 1),
		DIRTY_MATERIAL = (1 << 2),
		DIRTY_VISIBILITY = (1 << 3),
		DIRTY_LAYER_MASK = (1 << 4),
		DIRTY_ALL = 0xFFFFFFFF,
	};

	struct RenderEntity {
		uint64_t id = 0;
		uint64_t mesh_id = 0;
		uint64_t material_id = 0;
		float world_transform[16] = {};
		bool visible = true;
		uint32_t layer_mask = 1;
		uint32_t flags = DIRTY_NONE; // dirty tracking bitmask
		Vector3 aabb_size; // Cached AABB size for collision shape generation.
	};

	RenderStateExtractor();
	~RenderStateExtractor();

	/**
	 * Process a frame's worth of geometry instances.
	 * Compares against previous frame state and emits bridge calls
	 * for creates, updates, and destroys.
	 */
	void process_frame(const PagedArray<RenderGeometryInstance *> &p_instances);

	/**
	 * Destroy all tracked entities. Called during shutdown.
	 */
	void clear();

	/**
	 * Get the number of currently tracked entities.
	 */
	uint32_t get_entity_count() const;

	// ---- Volume Camera Culling (Phase 3.2) ----

	/**
	 * Set the volume camera bounds. In bounded mode, entities whose
	 * transformed AABB does not intersect this box are culled (hidden).
	 */
	void set_volume_bounds(const Vector3 &p_size);

	/**
	 * Set the volume camera world transform.
	 */
	void set_volume_transform(const Transform3D &p_transform);

	/**
	 * Set the volume mode. 0 = bounded (cull outside), 1 = immersive (no cull).
	 */
	void set_volume_mode(int p_mode);

	/**
	 * Enable or disable volume culling.
	 */
	void set_volume_culling_enabled(bool p_enabled);

private:
	// Previous frame state, keyed by entity id (pointer-derived).
	HashMap<uint64_t, RenderEntity> previous_state;

	// Current frame state, rebuilt each frame.
	HashMap<uint64_t, RenderEntity> current_state;

	// Convert a Transform3D to a column-major float[16] array.
	static void transform_to_float16(const Transform3D &p_transform, float *r_dst);

	// Compare two float[16] arrays for approximate equality.
	static bool transforms_equal(const float *p_a, const float *p_b);

	// Derive a stable entity id from a geometry instance pointer.
	static uint64_t entity_id_from_instance(const RenderGeometryInstance *p_instance);

	// Emit batched transform updates for entities that only changed transform.
	void emit_batched_transforms(const LocalVector<uint64_t> &p_ids, const LocalVector<float> &p_transforms);

	// Ensure mesh/material resources are uploaded to the Swift bridge before use.
	void _ensure_mesh_uploaded(uint64_t p_mesh_id);
	void _ensure_material_uploaded(uint64_t p_material_id);
	void _ensure_resources_uploaded(uint64_t p_mesh_id, uint64_t p_material_id);

	// Volume camera state for bounded-mode culling.
	bool volume_culling_enabled = false;
	int volume_mode = 0; // 0 = bounded, 1 = immersive
	Vector3 volume_size = Vector3(1, 1, 1);
	Transform3D volume_transform;
	AABB volume_aabb; // World-space AABB computed from size + transform.

	void _update_volume_aabb();
	bool _is_in_volume(const AABB &p_aabb) const;
};

#endif // RENDER_STATE_EXTRACTOR_H
