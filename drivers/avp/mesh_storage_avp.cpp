/**************************************************************************/
/*  mesh_storage_avp.cpp                                                  */
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

#include "mesh_storage_avp.h"

#include "avp_bridge.h"
#include "core/math/vector3.h"
#include "core/string/print_string.h"

MeshStorageAVP *MeshStorageAVP::singleton = nullptr;

MeshStorageAVP::MeshStorageAVP() {
	singleton = this;
}

MeshStorageAVP::~MeshStorageAVP() {
	singleton = nullptr;
}

RID MeshStorageAVP::mesh_allocate() {
	return mesh_owner.allocate_rid();
}

void MeshStorageAVP::mesh_initialize(RID p_rid) {
	mesh_owner.initialize_rid(p_rid, AVPMesh());
}

void MeshStorageAVP::mesh_free(RID p_rid) {
	AVPMesh *mesh = mesh_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(mesh);

	// Destroy the mesh on the bridge side if it was uploaded.
	if (mesh->uploaded_to_bridge) {
		godot_avp_mesh_destroy(p_rid.get_id());
	}

	mesh->dependency.deleted_notify(p_rid);
	mesh_owner.free(p_rid);
}

void MeshStorageAVP::mesh_set_blend_shape_count(RID p_mesh, int p_blend_shape_count) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	m->blend_shape_count = p_blend_shape_count;
}

void MeshStorageAVP::mesh_add_surface(RID p_mesh, const RS::SurfaceData &p_surface) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);

	m->surfaces.push_back(RS::SurfaceData());
	RS::SurfaceData *s = &m->surfaces.write[m->surfaces.size() - 1];
	s->format = p_surface.format;
	s->primitive = p_surface.primitive;
	s->vertex_data = p_surface.vertex_data;
	s->attribute_data = p_surface.attribute_data;
	s->vertex_count = p_surface.vertex_count;
	s->index_data = p_surface.index_data;
	s->index_count = p_surface.index_count;
	s->aabb = p_surface.aabb;
	s->skin_data = p_surface.skin_data;
	s->lods = p_surface.lods;
	s->bone_aabbs = p_surface.bone_aabbs;
	s->mesh_to_skeleton_xform = p_surface.mesh_to_skeleton_xform;
	s->blend_shape_data = p_surface.blend_shape_data;
	s->uv_scale = p_surface.uv_scale;
	s->material = p_surface.material;

	// Track LOD levels.
	m->lod_count = p_surface.lods.size();
	m->lod_thresholds.clear();
	for (int l = 0; l < p_surface.lods.size(); l++) {
		m->lod_thresholds.push_back(p_surface.lods[l].edge_length);
	}

	// Mark as needing re-upload since surface data changed.
	m->uploaded_to_bridge = false;

	m->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
}

int MeshStorageAVP::mesh_get_blend_shape_count(RID p_mesh) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, 0);
	return m->blend_shape_count;
}

void MeshStorageAVP::mesh_set_blend_shape_mode(RID p_mesh, RS::BlendShapeMode p_mode) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	m->blend_shape_mode = p_mode;
}

RS::BlendShapeMode MeshStorageAVP::mesh_get_blend_shape_mode(RID p_mesh) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, RS::BLEND_SHAPE_MODE_NORMALIZED);
	return m->blend_shape_mode;
}

void MeshStorageAVP::mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	ERR_FAIL_INDEX(p_surface, m->surfaces.size());
	m->surfaces.write[p_surface].material = p_material;
}

RID MeshStorageAVP::mesh_surface_get_material(RID p_mesh, int p_surface) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, RID());
	ERR_FAIL_INDEX_V(p_surface, m->surfaces.size(), RID());
	return m->surfaces[p_surface].material;
}

RS::SurfaceData MeshStorageAVP::mesh_get_surface(RID p_mesh, int p_surface) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, RS::SurfaceData());
	ERR_FAIL_INDEX_V(p_surface, m->surfaces.size(), RS::SurfaceData());
	return m->surfaces[p_surface];
}

int MeshStorageAVP::mesh_get_surface_count(RID p_mesh) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, 0);
	return m->surfaces.size();
}

AABB MeshStorageAVP::mesh_get_aabb(RID p_mesh, RID p_skeleton) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, AABB());

	AABB aabb;
	for (int i = 0; i < m->surfaces.size(); i++) {
		if (i == 0) {
			aabb = m->surfaces[i].aabb;
		} else {
			aabb.merge_with(m->surfaces[i].aabb);
		}
	}
	return aabb;
}

void MeshStorageAVP::mesh_set_path(RID p_mesh, const String &p_path) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	m->path = p_path;
}

String MeshStorageAVP::mesh_get_path(RID p_mesh) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, String());
	return m->path;
}

void MeshStorageAVP::mesh_surface_remove(RID p_mesh, int p_surface) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	m->dependency.changed_notify(Dependency::DEPENDENCY_CHANGED_MESH);
	m->surfaces.remove_at(p_surface);
	m->uploaded_to_bridge = false;
}

void MeshStorageAVP::mesh_clear(RID p_mesh) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL(m);
	m->surfaces.clear();
	m->uploaded_to_bridge = false;
}

void MeshStorageAVP::_upload_mesh_to_bridge(RID p_mesh, AVPMesh *p_avp_mesh) {
	if (p_avp_mesh->uploaded_to_bridge) {
		return;
	}

	if (p_avp_mesh->surfaces.is_empty()) {
		return;
	}

	// Upload the first surface. Multi-surface meshes create additional
	// bridge entities in a later integration phase.
	const RS::SurfaceData &surface = p_avp_mesh->surfaces[0];

	uint32_t vertex_count = surface.vertex_count;
	uint32_t index_count = surface.index_count;

	if (vertex_count == 0) {
		return;
	}

	uint64_t format = surface.format;
	bool is_compressed = format & RS::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	bool is_2d = format & RS::ARRAY_FLAG_USE_2D_VERTICES;
	bool has_normals = format & RS::ARRAY_FORMAT_NORMAL;
	bool has_tangents = format & RS::ARRAY_FORMAT_TANGENT;
	bool has_color = format & RS::ARRAY_FORMAT_COLOR;
	bool has_uv = format & RS::ARRAY_FORMAT_TEX_UV;

	const uint8_t *vertex_data = surface.vertex_data.ptr();
	uint32_t vertex_data_size = surface.vertex_data.size();

	// ---- Calculate vertex_data layout (position, normal, tangent) ----
	uint32_t vertex_elem_size = 0;
	uint32_t normal_elem_size = 0;
	uint32_t tangent_elem_size = 0;

	if (is_compressed) {
		// Compressed: position = 8 bytes (4 uint16s), tangent implicit.
		vertex_elem_size = 8;
		normal_elem_size = has_normals ? 4 : 0;
		tangent_elem_size = 0; // Tangent encoded in vertex angle.
	} else {
		vertex_elem_size = is_2d ? 8 : 12;
		normal_elem_size = has_normals ? 4 : 0;
		tangent_elem_size = has_tangents ? 4 : 0;
	}

	uint32_t vertex_stride = vertex_elem_size + normal_elem_size + tangent_elem_size;

	// Sanity check stride against actual data.
	if (vertex_data_size < vertex_count * vertex_stride) {
		vertex_stride = vertex_data_size / vertex_count;
		if (vertex_stride < (is_2d ? 8 : 12)) {
			WARN_PRINT("AVP: vertex data too small, skipping mesh upload.");
			return;
		}
	}

	// ---- Extract positions ----
	Vector<float> positions;
	positions.resize(vertex_count * 3);
	float *pos_ptr = positions.ptrw();

	if (is_compressed) {
		// Compressed: positions stored as 4 uint16s, normalized to AABB.
		const AABB &aabb = surface.aabb;
		Vector3 aabb_pos = aabb.position;
		Vector3 aabb_size = aabb.size;

		for (uint32_t i = 0; i < vertex_count; i++) {
			const uint16_t *src = reinterpret_cast<const uint16_t *>(vertex_data + i * vertex_stride);
			pos_ptr[i * 3 + 0] = (float(src[0]) / 65535.0f) * aabb_size.x + aabb_pos.x;
			pos_ptr[i * 3 + 1] = (float(src[1]) / 65535.0f) * aabb_size.y + aabb_pos.y;
			pos_ptr[i * 3 + 2] = (float(src[2]) / 65535.0f) * aabb_size.z + aabb_pos.z;
		}
	} else {
		for (uint32_t i = 0; i < vertex_count; i++) {
			const float *src = reinterpret_cast<const float *>(vertex_data + i * vertex_stride);
			pos_ptr[i * 3 + 0] = src[0];
			pos_ptr[i * 3 + 1] = src[1];
			pos_ptr[i * 3 + 2] = is_2d ? 0.0f : src[2];
		}
	}

	// ---- Extract normals (octahedron-packed) ----
	Vector<float> normals;
	float *normals_ptr = nullptr;
	if (has_normals && normal_elem_size > 0) {
		normals.resize(vertex_count * 3);
		normals_ptr = normals.ptrw();

		uint32_t normal_data_offset = vertex_elem_size;
		for (uint32_t i = 0; i < vertex_count; i++) {
			const uint32_t *packed = reinterpret_cast<const uint32_t *>(
					vertex_data + i * vertex_stride + normal_data_offset);
			uint32_t n = packed[0];
			// Octahedron decode: two uint16 values → Vector3.
			Vector2 oct(
					float(n & 0xFFFF) / 65535.0f,
					float((n >> 16) & 0xFFFF) / 65535.0f);
			Vector3 normal = Vector3::octahedron_decode(oct);
			normals_ptr[i * 3 + 0] = normal.x;
			normals_ptr[i * 3 + 1] = normal.y;
			normals_ptr[i * 3 + 2] = normal.z;
		}
	}

	// ---- Extract tangents (octahedron-packed, non-compressed only) ----
	Vector<float> tangents;
	float *tangents_ptr = nullptr;
	if (has_tangents && !is_compressed && tangent_elem_size > 0) {
		tangents.resize(vertex_count * 4);
		tangents_ptr = tangents.ptrw();

		uint32_t tangent_data_offset = vertex_elem_size + normal_elem_size;
		for (uint32_t i = 0; i < vertex_count; i++) {
			const uint32_t *packed = reinterpret_cast<const uint32_t *>(
					vertex_data + i * vertex_stride + tangent_data_offset);
			uint32_t t = packed[0];
			Vector2 oct(
					float(t & 0xFFFF) / 65535.0f,
					float((t >> 16) & 0xFFFF) / 65535.0f);
			float sign = 1.0f;
			Vector3 tangent = Vector3::octahedron_tangent_decode(oct, &sign);
			tangents_ptr[i * 4 + 0] = tangent.x;
			tangents_ptr[i * 4 + 1] = tangent.y;
			tangents_ptr[i * 4 + 2] = tangent.z;
			tangents_ptr[i * 4 + 3] = sign;
		}
	}

	// ---- Extract attributes from attribute_data (colors, UVs) ----
	const uint8_t *attrib_data = surface.attribute_data.ptr();
	uint32_t attrib_data_size = surface.attribute_data.size();

	// Calculate attribute stride and offsets.
	uint32_t attrib_stride = 0;
	uint32_t color_offset = 0;
	uint32_t uv_offset = 0;

	if (attrib_data && attrib_data_size > 0) {
		// Color: 4 bytes (RGBA8) if present.
		if (has_color) {
			color_offset = attrib_stride;
			attrib_stride += 4;
		}
		// UV: 4 bytes (compressed, 2 uint16s) or 8 bytes (2 floats).
		if (has_uv) {
			uv_offset = attrib_stride;
			attrib_stride += is_compressed ? 4 : 8;
		}
		// UV2 and custom channels follow but are not forwarded to bridge.
		// Adjust stride for remaining attributes so offset calculation is correct.
		bool has_uv2 = format & RS::ARRAY_FORMAT_TEX_UV2;
		if (has_uv2) {
			attrib_stride += is_compressed ? 4 : 8;
		}
		// Custom channels.
		for (int ci = 0; ci < RS::ARRAY_CUSTOM_COUNT; ci++) {
			if (format & (RS::ARRAY_FORMAT_CUSTOM0 << ci)) {
				static const uint32_t custom_sizes[] = { 4, 4, 4, 8, 4, 8, 12, 16 };
				uint32_t custom_fmt = (format >> (RS::ARRAY_FORMAT_CUSTOM_BASE + ci * RS::ARRAY_FORMAT_CUSTOM_BITS)) & RS::ARRAY_FORMAT_CUSTOM_MASK;
				attrib_stride += custom_sizes[custom_fmt];
			}
		}

		// Fallback sanity check.
		if (attrib_stride == 0 || attrib_data_size < vertex_count * attrib_stride) {
			if (attrib_stride > 0) {
				attrib_stride = attrib_data_size / vertex_count;
			}
		}
	}

	// Extract colors.
	Vector<float> colors;
	float *colors_ptr = nullptr;
	if (has_color && attrib_data && attrib_stride > 0) {
		colors.resize(vertex_count * 4);
		colors_ptr = colors.ptrw();

		for (uint32_t i = 0; i < vertex_count; i++) {
			const uint8_t *src = attrib_data + i * attrib_stride + color_offset;
			colors_ptr[i * 4 + 0] = src[0] / 255.0f;
			colors_ptr[i * 4 + 1] = src[1] / 255.0f;
			colors_ptr[i * 4 + 2] = src[2] / 255.0f;
			colors_ptr[i * 4 + 3] = src[3] / 255.0f;
		}
	}

	// Extract UVs.
	Vector<float> uvs;
	float *uvs_ptr = nullptr;
	if (has_uv && attrib_data && attrib_stride > 0) {
		uvs.resize(vertex_count * 2);
		uvs_ptr = uvs.ptrw();

		if (is_compressed) {
			Vector4 uv_scale = surface.uv_scale;
			for (uint32_t i = 0; i < vertex_count; i++) {
				const uint16_t *src = reinterpret_cast<const uint16_t *>(
						attrib_data + i * attrib_stride + uv_offset);
				float u = float(src[0]) / 65535.0f;
				float v = float(src[1]) / 65535.0f;
				if (uv_scale.x != 0.0f || uv_scale.y != 0.0f) {
					u = (u - 0.5f) * uv_scale.x;
					v = (v - 0.5f) * uv_scale.y;
				}
				uvs_ptr[i * 2 + 0] = u;
				uvs_ptr[i * 2 + 1] = v;
			}
		} else {
			for (uint32_t i = 0; i < vertex_count; i++) {
				const float *src = reinterpret_cast<const float *>(
						attrib_data + i * attrib_stride + uv_offset);
				uvs_ptr[i * 2 + 0] = src[0];
				uvs_ptr[i * 2 + 1] = src[1];
			}
		}
	}

	// ---- Extract indices ----
	const uint32_t *indices_ptr = nullptr;
	Vector<uint32_t> indices_32;
	if (index_count > 0 && surface.index_data.size() > 0) {
		if (vertex_count <= 65536) {
			// 16-bit indices — widen to 32-bit for bridge.
			const uint16_t *idx16 = reinterpret_cast<const uint16_t *>(surface.index_data.ptr());
			indices_32.resize(index_count);
			uint32_t *idx32_w = indices_32.ptrw();
			for (uint32_t i = 0; i < index_count; i++) {
				idx32_w[i] = idx16[i];
			}
			indices_ptr = indices_32.ptr();
		} else {
			// 32-bit indices — use directly.
			indices_ptr = reinterpret_cast<const uint32_t *>(surface.index_data.ptr());
		}
	}

	// ---- Upload to bridge ----
	uint64_t mesh_id = p_mesh.get_id();
	uint32_t surface_count = p_avp_mesh->surfaces.size();

	godot_avp_mesh_create(
			mesh_id,
			pos_ptr,
			normals_ptr,
			uvs_ptr,
			tangents_ptr,
			colors_ptr,
			indices_ptr,
			vertex_count,
			index_count,
			surface_count);

	p_avp_mesh->uploaded_to_bridge = true;
}

void MeshStorageAVP::ensure_mesh_uploaded(RID p_mesh) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	if (m && !m->uploaded_to_bridge) {
		_upload_mesh_to_bridge(p_mesh, m);
	}
}

/* MULTIMESH API */

RID MeshStorageAVP::_multimesh_allocate() {
	return multimesh_owner.allocate_rid();
}

void MeshStorageAVP::_multimesh_initialize(RID p_rid) {
	multimesh_owner.initialize_rid(p_rid, AVPMultiMesh());
}

void MeshStorageAVP::_multimesh_free(RID p_rid) {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(multimesh);

	// Destroy on bridge side.
	godot_avp_instancing_destroy(p_rid.get_id());

	multimesh_owner.free(p_rid);
}

void MeshStorageAVP::_multimesh_allocate_data(RID p_multimesh, int p_instances, RS::MultimeshTransformFormat p_transform_format, bool p_use_colors, bool p_use_custom_data, bool p_use_indirect) {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL(multimesh);

	multimesh->instance_count = p_instances;
	multimesh->transform_format = p_transform_format;
	multimesh->use_colors = p_use_colors;
	multimesh->use_custom_data = p_use_custom_data;
}

int MeshStorageAVP::_multimesh_get_instance_count(RID p_multimesh) const {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL_V(multimesh, 0);
	return multimesh->instance_count;
}

void MeshStorageAVP::_multimesh_set_mesh(RID p_multimesh, RID p_mesh) {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL(multimesh);
	multimesh->mesh = p_mesh;
}

RID MeshStorageAVP::_multimesh_get_mesh(RID p_multimesh) const {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL_V(multimesh, RID());
	return multimesh->mesh;
}

void MeshStorageAVP::_multimesh_set_buffer(RID p_multimesh, const Vector<float> &p_buffer) {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL(multimesh);
	multimesh->buffer.resize(p_buffer.size());
	float *cache_data = multimesh->buffer.ptrw();
	memcpy(cache_data, p_buffer.ptr(), p_buffer.size() * sizeof(float));
}

Vector<float> MeshStorageAVP::_multimesh_get_buffer(RID p_multimesh) const {
	AVPMultiMesh *multimesh = multimesh_owner.get_or_null(p_multimesh);
	ERR_FAIL_NULL_V(multimesh, Vector<float>());
	return multimesh->buffer;
}

/* LOD API */

int MeshStorageAVP::mesh_get_lod_count(RID p_mesh) const {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, 0);
	return m->lod_count;
}

bool MeshStorageAVP::mesh_select_lod(RID p_mesh, int p_lod_level) {
	AVPMesh *m = mesh_owner.get_or_null(p_mesh);
	ERR_FAIL_NULL_V(m, false);

	if (m->surfaces.is_empty()) {
		return false;
	}

	const RS::SurfaceData &surface = m->surfaces[0];

	if (p_lod_level <= 0 || p_lod_level > m->lod_count) {
		// LOD 0 or invalid → use original index buffer.
		// Re-upload with the original indices if currently showing a different LOD.
		m->uploaded_to_bridge = false;
		_upload_mesh_to_bridge(p_mesh, m);
		return true;
	}

	// LOD level 1..N maps to surface.lods[0..N-1].
	int lod_idx = p_lod_level - 1;
	ERR_FAIL_INDEX_V(lod_idx, surface.lods.size(), false);

	const RS::SurfaceData::LOD &lod = surface.lods[lod_idx];

	if (lod.index_data.is_empty()) {
		return false;
	}

	// Destroy the existing mesh on the bridge and re-upload with LOD indices.
	uint64_t mesh_id = p_mesh.get_id();
	godot_avp_mesh_destroy(mesh_id);

	// Re-extract positions (same vertex data, different index buffer).
	// Mark as not uploaded so _upload_mesh_to_bridge rebuilds it.
	// For LOD we override the index data temporarily.
	// NOTE: This is a simplified approach. A full implementation would
	// upload all LOD levels as separate mesh resources and switch between them.

	// For now, swap the index data, upload, then swap back.
	// This avoids duplicating the full vertex extraction code.
	RS::SurfaceData &mutable_surface = m->surfaces.write[0];
	Vector<uint8_t> original_index_data = mutable_surface.index_data;
	uint32_t original_index_count = mutable_surface.index_count;

	mutable_surface.index_data = lod.index_data;
	// LOD index count: data_size / sizeof(uint16_t or uint32_t).
	if (surface.vertex_count <= 65536) {
		mutable_surface.index_count = lod.index_data.size() / sizeof(uint16_t);
	} else {
		mutable_surface.index_count = lod.index_data.size() / sizeof(uint32_t);
	}

	m->uploaded_to_bridge = false;
	_upload_mesh_to_bridge(p_mesh, m);

	// Restore original index data.
	mutable_surface.index_data = original_index_data;
	mutable_surface.index_count = original_index_count;

	return true;
}
