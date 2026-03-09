/**************************************************************************/
/*  rasterizer_avp.h                                                      */
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

#ifndef RASTERIZER_AVP_H
#define RASTERIZER_AVP_H

#include "core/templates/rid_owner.h"
#include "core/templates/self_list.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/dummy/environment/fog.h"
#include "servers/rendering/dummy/environment/gi.h"
#include "servers/rendering/dummy/rasterizer_canvas_dummy.h"
#include "servers/rendering/dummy/storage/light_storage.h"
#include "servers/rendering/dummy/storage/particles_storage.h"
#include "servers/rendering/dummy/storage/utilities.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/rendering_server.h"

#include "material_storage_avp.h"
#include "mesh_storage_avp.h"
#include "rasterizer_scene_avp.h"
#include "texture_storage_avp.h"

/**
 * RasterizerAVP -- RendererCompositor for Apple Vision Pro.
 *
 * Acts as a no-op GPU renderer where render_scene() is the extraction
 * hook. Uses AVP-specific implementations for mesh, material, texture,
 * and scene rendering. Uses dummy implementations for everything else
 * (lights, particles, fog, GI, canvas, utilities) since those subsystems
 * are either handled by RealityKit or out of scope for AVP.
 *
 * Follows the make_current() pattern from RasterizerDummy.
 */
class RasterizerAVP : public RendererCompositor {
private:
	uint64_t frame = 1;
	double delta = 0;
	double time = 0.0;

protected:
	// Canvas: dummy (2D is out of scope for AVP).
	RasterizerCanvasDummy canvas;

	// Reuse dummy implementations for subsystems handled by RealityKit.
	RendererDummy::Utilities utilities;
	RendererDummy::LightStorage light_storage;
	RendererDummy::ParticlesStorage particles_storage;
	RendererDummy::GI gi;
	RendererDummy::Fog fog;

	// AVP-specific implementations.
	MaterialStorageAVP material_storage;
	MeshStorageAVP mesh_storage;
	TextureStorageAVP texture_storage;
	RasterizerSceneAVP scene;

public:
	RendererUtilities *get_utilities() override { return &utilities; }
	RendererLightStorage *get_light_storage() override { return &light_storage; }
	RendererMaterialStorage *get_material_storage() override { return &material_storage; }
	RendererMeshStorage *get_mesh_storage() override { return &mesh_storage; }
	RendererParticlesStorage *get_particles_storage() override { return &particles_storage; }
	RendererTextureStorage *get_texture_storage() override { return &texture_storage; }
	RendererGI *get_gi() override { return &gi; }
	RendererFog *get_fog() override { return &fog; }
	RendererCanvasRender *get_canvas() override { return &canvas; }
	RendererSceneRender *get_scene() override { return &scene; }

	void set_boot_image_with_stretch(const Ref<Image> &p_image, const Color &p_color, RenderingServer::SplashStretchMode p_stretch_mode, bool p_use_filter = true) override {}
	void set_boot_image(const Ref<Image> &p_image, const Color &p_color, bool p_scale, bool p_use_filter = true) override {}

	void initialize() override {}
	void begin_frame(double frame_step) override {
		frame++;
		delta = frame_step;
		time += frame_step;
	}

	void blit_render_targets_to_screen(int p_screen, const BlitToScreen *p_render_targets, int p_amount) override {}

	bool is_opengl() override { return false; }
	void gl_end_frame(bool p_swap_buffers) override {}

	void end_frame(bool p_present) override {
		// No swap buffers needed -- RealityKit handles presentation.
	}

	void finalize() override {}

	static RendererCompositor *_create_current() {
		return memnew(RasterizerAVP);
	}

	static void make_current() {
		_create_func = _create_current;
		low_end = false;
	}

	uint64_t get_frame_number() const override { return frame; }
	double get_frame_delta_time() const override { return delta; }
	double get_total_time() const override { return time; }
	bool can_create_resources_async() const override { return false; }

	RasterizerAVP() {}
	~RasterizerAVP() {}
};

#endif // RASTERIZER_AVP_H
