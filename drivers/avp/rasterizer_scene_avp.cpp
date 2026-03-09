/**************************************************************************/
/*  rasterizer_scene_avp.cpp                                              */
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

#include "rasterizer_scene_avp.h"

#include "avp_bridge.h"
#include "servers/rendering/rendering_server_globals.h"

RasterizerSceneAVP::RasterizerSceneAVP() {
	// Initialize the spatial input bridge — registers tap/drag
	// C callbacks with the Swift side.
	input_bridge.initialize();
}

RasterizerSceneAVP::~RasterizerSceneAVP() {
	state_extractor.clear();
}

RenderGeometryInstance *RasterizerSceneAVP::geometry_instance_create(RID p_base) {
	// Validate that this is a geometry type we care about.
	RS::InstanceType type = RSG::utilities->get_base_type(p_base);
	ERR_FAIL_COND_V(!((1 << type) & RS::INSTANCE_GEOMETRY_MASK), nullptr);

	GeometryInstanceAVP *ginstance = geometry_instance_alloc.alloc();
	ginstance->base = p_base;

	// Derive mesh_id from the base RID.
	ginstance->mesh_id = p_base.get_id();

	return ginstance;
}

void RasterizerSceneAVP::geometry_instance_free(RenderGeometryInstance *p_geometry_instance) {
	GeometryInstanceAVP *ginstance = static_cast<GeometryInstanceAVP *>(p_geometry_instance);
	ERR_FAIL_NULL(ginstance);

	geometry_instance_alloc.free(ginstance);
}

void RasterizerSceneAVP::render_scene(const Ref<RenderSceneBuffers> &p_render_buffers, const CameraData *p_camera_data, const CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const PagedArray<RID> &p_reflection_probes, const PagedArray<RID> &p_voxel_gi_instances, const PagedArray<RID> &p_decals, const PagedArray<RID> &p_lightmaps, const PagedArray<RID> &p_fog_volumes, RID p_environment, RID p_camera_attributes, RID p_compositor, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, const RenderShadowData *p_render_shadows, int p_render_shadow_count, const RenderSDFGIData *p_render_sdfgi_regions, int p_render_sdfgi_region_count, const RenderSDFGIUpdateData *p_sdfgi_update_data, RenderingMethod::RenderInfo *r_info) {
	// This is the critical hook: instead of rendering to a framebuffer,
	// we extract render state and forward it to the RealityKit bridge.

	// Signal frame begin to the Swift side.
	godot_avp_frame_begin();

	// Delegate to the state extractor which performs the diff
	// and emits create/update/destroy calls.
	state_extractor.process_frame(p_instances);

	// Signal frame end.
	godot_avp_frame_end();
}

bool RasterizerSceneAVP::free(RID p_rid) {
	if (is_environment(p_rid)) {
		environment_free(p_rid);
		return true;
	} else if (is_compositor(p_rid)) {
		compositor_free(p_rid);
		return true;
	} else if (is_compositor_effect(p_rid)) {
		compositor_effect_free(p_rid);
		return true;
	} else if (RSG::camera_attributes->owns_camera_attributes(p_rid)) {
		RSG::camera_attributes->camera_attributes_free(p_rid);
		return true;
	} else {
		return false;
	}
}
