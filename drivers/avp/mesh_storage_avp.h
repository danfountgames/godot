/**************************************************************************/
/*  mesh_storage_avp.h                                                    */
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

#ifndef MESH_STORAGE_AVP_H
#define MESH_STORAGE_AVP_H

#include "core/templates/rid_owner.h"
#include "servers/rendering/storage/mesh_storage.h"

/**
 * AVP mesh storage.
 *
 * Stores mesh data (vertices, normals, UVs, indices) and on first use
 * forwards mesh data to the bridge via godot_avp_mesh_create().
 * Manages RID -> mesh_id mapping.
 */
class MeshStorageAVP : public RendererMeshStorage {
private:
	static MeshStorageAVP *singleton;

	struct AVPMesh {
		Vector<RS::SurfaceData> surfaces;
		int blend_shape_count = 0;
		RS::BlendShapeMode blend_shape_mode = RS::BLEND_SHAPE_MODE_NORMALIZED;
		PackedFloat32Array blend_shape_values;
		Dependency dependency;
		bool uploaded_to_bridge = false; // Track whether mesh data has been sent to RealityKit.
		String path;

		// LOD support: number of LOD levels available (from surface.lods).
		int lod_count = 0;
		// LOD edge_length thresholds (sorted ascending — lower edge_length = lower detail).
		Vector<float> lod_thresholds;
	};

	mutable RID_Owner<AVPMesh> mesh_owner;

	struct AVPMultiMesh {
		PackedFloat32Array buffer;
		RID mesh;
		int instance_count = 0;
		RS::MultimeshTransformFormat transform_format = RS::MULTIMESH_TRANSFORM_3D;
		bool use_colors = false;
		bool use_custom_data = false;
	};

	mutable RID_Owner<AVPMultiMesh> multimesh_owner;

	// Upload mesh surface data to the bridge on first use.
	void _upload_mesh_to_bridge(RID p_mesh, AVPMesh *p_avp_mesh);

public:
	static MeshStorageAVP *get_singleton() { return singleton; }

	MeshStorageAVP();
	~MeshStorageAVP();

	/* MESH API */
	AVPMesh *get_mesh(RID p_rid) { return mesh_owner.get_or_null(p_rid); }
	bool owns_mesh(RID p_rid) { return mesh_owner.owns(p_rid); }

	virtual RID mesh_allocate() override;
	virtual void mesh_initialize(RID p_rid) override;
	virtual void mesh_free(RID p_rid) override;

	virtual void mesh_set_blend_shape_count(RID p_mesh, int p_blend_shape_count) override;
	virtual bool mesh_needs_instance(RID p_mesh, bool p_has_skeleton) override { return false; }

	virtual void mesh_add_surface(RID p_mesh, const RS::SurfaceData &p_surface) override;

	virtual int mesh_get_blend_shape_count(RID p_mesh) const override;

	virtual void mesh_set_blend_shape_mode(RID p_mesh, RS::BlendShapeMode p_mode) override;
	virtual RS::BlendShapeMode mesh_get_blend_shape_mode(RID p_mesh) const override;

	virtual void mesh_surface_update_vertex_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) override {}
	virtual void mesh_surface_update_attribute_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) override {}
	virtual void mesh_surface_update_skin_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) override {}
	virtual void mesh_surface_update_index_region(RID p_mesh, int p_surface, int p_offset, const Vector<uint8_t> &p_data) override {}

	virtual void mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material) override;
	virtual RID mesh_surface_get_material(RID p_mesh, int p_surface) const override;

	virtual RS::SurfaceData mesh_get_surface(RID p_mesh, int p_surface) const override;
	virtual int mesh_get_surface_count(RID p_mesh) const override;

	virtual void mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) override {}
	virtual AABB mesh_get_custom_aabb(RID p_mesh) const override { return AABB(); }
	virtual AABB mesh_get_aabb(RID p_mesh, RID p_skeleton = RID()) override;

	virtual void mesh_set_path(RID p_mesh, const String &p_path) override;
	virtual String mesh_get_path(RID p_mesh) const override;

	virtual void mesh_set_shadow_mesh(RID p_mesh, RID p_shadow_mesh) override {}

	virtual void mesh_surface_remove(RID p_mesh, int p_surface) override;
	virtual void mesh_clear(RID p_mesh) override;
	virtual void mesh_debug_usage(List<RS::MeshInfo> *r_info) override {}

	/* MESH INSTANCE */

	virtual RID mesh_instance_create(RID p_base) override { return RID(); }
	virtual void mesh_instance_free(RID p_rid) override {}

	virtual void mesh_instance_set_skeleton(RID p_mesh_instance, RID p_skeleton) override {}
	virtual void mesh_instance_set_blend_shape_weight(RID p_mesh_instance, int p_shape, float p_weight) override {}
	virtual void mesh_instance_check_for_update(RID p_mesh_instance) override {}
	virtual void mesh_instance_set_canvas_item_transform(RID p_mesh_instance, const Transform2D &p_transform) override {}
	virtual void update_mesh_instances() override {}

	/* MULTIMESH API */

	bool owns_multimesh(RID p_rid) { return multimesh_owner.owns(p_rid); }

	virtual RID _multimesh_allocate() override;
	virtual void _multimesh_initialize(RID p_rid) override;
	virtual void _multimesh_free(RID p_rid) override;

	virtual void _multimesh_allocate_data(RID p_multimesh, int p_instances, RS::MultimeshTransformFormat p_transform_format, bool p_use_colors = false, bool p_use_custom_data = false, bool p_use_indirect = false) override;
	virtual int _multimesh_get_instance_count(RID p_multimesh) const override;

	virtual void _multimesh_set_mesh(RID p_multimesh, RID p_mesh) override;
	virtual void _multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform3D &p_transform) override {}
	virtual void _multimesh_instance_set_transform_2d(RID p_multimesh, int p_index, const Transform2D &p_transform) override {}
	virtual void _multimesh_instance_set_color(RID p_multimesh, int p_index, const Color &p_color) override {}
	virtual void _multimesh_instance_set_custom_data(RID p_multimesh, int p_index, const Color &p_color) override {}

	virtual void _multimesh_set_custom_aabb(RID p_multimesh, const AABB &p_aabb) override {}
	virtual AABB _multimesh_get_custom_aabb(RID p_multimesh) const override { return AABB(); }

	virtual RID _multimesh_get_mesh(RID p_multimesh) const override;
	virtual AABB _multimesh_get_aabb(RID p_multimesh) override { return AABB(); }

	virtual Transform3D _multimesh_instance_get_transform(RID p_multimesh, int p_index) const override { return Transform3D(); }
	virtual Transform2D _multimesh_instance_get_transform_2d(RID p_multimesh, int p_index) const override { return Transform2D(); }
	virtual Color _multimesh_instance_get_color(RID p_multimesh, int p_index) const override { return Color(); }
	virtual Color _multimesh_instance_get_custom_data(RID p_multimesh, int p_index) const override { return Color(); }
	virtual void _multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) override;
	virtual RID _multimesh_get_command_buffer_rd_rid(RID p_multimesh) const override { return RID(); }
	virtual RID _multimesh_get_buffer_rd_rid(RID p_multimesh) const override { return RID(); }
	virtual Vector<float> _multimesh_get_buffer(RID p_multimesh) const override;

	virtual void _multimesh_set_visible_instances(RID p_multimesh, int p_visible) override {}
	virtual int _multimesh_get_visible_instances(RID p_multimesh) const override { return 0; }

	MultiMeshInterpolator *_multimesh_get_interpolator(RID p_multimesh) const override { return nullptr; }

	/* SKELETON API */

	virtual RID skeleton_allocate() override { return RID(); }
	virtual void skeleton_initialize(RID p_rid) override {}
	virtual void skeleton_free(RID p_rid) override {}
	virtual void skeleton_allocate_data(RID p_skeleton, int p_bones, bool p_2d_skeleton = false) override {}
	virtual void skeleton_set_base_transform_2d(RID p_skeleton, const Transform2D &p_base_transform) override {}
	virtual int skeleton_get_bone_count(RID p_skeleton) const override { return 0; }
	virtual void skeleton_bone_set_transform(RID p_skeleton, int p_bone, const Transform3D &p_transform) override {}
	virtual Transform3D skeleton_bone_get_transform(RID p_skeleton, int p_bone) const override { return Transform3D(); }
	virtual void skeleton_bone_set_transform_2d(RID p_skeleton, int p_bone, const Transform2D &p_transform) override {}
	virtual Transform2D skeleton_bone_get_transform_2d(RID p_skeleton, int p_bone) const override { return Transform2D(); }

	virtual void skeleton_update_dependency(RID p_base, DependencyTracker *p_instance) override {}

	/* OCCLUDER */

	void occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) {}

	/**
	 * Ensure the mesh data for the given RID has been uploaded to
	 * the RealityKit bridge. Called lazily when a mesh is first
	 * referenced during render_scene().
	 */
	void ensure_mesh_uploaded(RID p_mesh);

	/**
	 * Get the number of LOD levels available for a mesh.
	 * Returns 0 if no LODs, or the count of alternate index buffers.
	 */
	int mesh_get_lod_count(RID p_mesh) const;

	/**
	 * Select a LOD level for the mesh and re-upload with the alternate
	 * index buffer. level 0 = highest quality (original), 1+ = lower detail.
	 * Returns true if the LOD was changed.
	 */
	bool mesh_select_lod(RID p_mesh, int p_lod_level);
};

#endif // MESH_STORAGE_AVP_H
