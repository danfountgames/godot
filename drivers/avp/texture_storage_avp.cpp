/**************************************************************************/
/*  texture_storage_avp.cpp                                               */
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

#include "texture_storage_avp.h"

#include "avp_bridge.h"
#include "core/string/print_string.h"

TextureStorageAVP::TextureStorageAVP() {
	singleton = this;
}

TextureStorageAVP::~TextureStorageAVP() {
	singleton = nullptr;
}

uint32_t TextureStorageAVP::_image_format_to_bridge_format(Image::Format p_format, bool p_is_srgb) {
	// Bridge format codes (from avp_bridge.h):
	// 0 = RGBA8 Unorm
	// 1 = RGBA8 sRGB
	// 2 = RGBA16 Float
	// 3 = R8 Unorm
	// 4 = RG8 Unorm
	// 5 = BC7 Unorm (compressed)

	switch (p_format) {
		case Image::FORMAT_RGBA8:
			return p_is_srgb ? 1 : 0;
		case Image::FORMAT_RGBAF:
			return 2;
		case Image::FORMAT_R8:
			return 3;
		case Image::FORMAT_RG8:
			return 4;
		default:
			// Default to RGBA8 sRGB for anything else.
			return p_is_srgb ? 1 : 0;
	}
}

void TextureStorageAVP::_upload_texture_to_bridge(RID p_texture, AVPTexture *p_avp_texture) {
	if (p_avp_texture->uploaded_to_bridge) {
		return;
	}

	if (p_avp_texture->image.is_null() || p_avp_texture->image->is_empty()) {
		return;
	}

	Ref<Image> img = p_avp_texture->image;

	// Ensure the image is in RGBA8 format for the bridge.
	if (img->get_format() != Image::FORMAT_RGBA8) {
		img = img->duplicate();
		img->convert(Image::FORMAT_RGBA8);
	}

	Vector<uint8_t> data = img->get_data();
	uint32_t width = img->get_width();
	uint32_t height = img->get_height();
	uint32_t bridge_format = _image_format_to_bridge_format(img->get_format(), p_avp_texture->is_srgb);
	uint32_t mipmaps = img->has_mipmaps() ? img->get_mipmap_count() + 1 : 1;

	godot_avp_texture_create(
			p_texture.get_id(),
			data.ptr(),
			width,
			height,
			bridge_format,
			mipmaps,
			p_avp_texture->is_srgb ? 1 : 0);

	p_avp_texture->uploaded_to_bridge = true;
}

void TextureStorageAVP::ensure_texture_uploaded(RID p_texture) {
	AVPTexture *t = texture_owner.get_or_null(p_texture);
	if (t && !t->uploaded_to_bridge) {
		_upload_texture_to_bridge(p_texture, t);
	}
}

/* Texture API */

RID TextureStorageAVP::texture_allocate() {
	AVPTexture *texture = memnew(AVPTexture);
	ERR_FAIL_NULL_V(texture, RID());
	return texture_owner.make_rid(texture);
}

void TextureStorageAVP::texture_free(RID p_rid) {
	AVPTexture *texture = texture_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(texture);

	if (texture->uploaded_to_bridge) {
		godot_avp_texture_destroy(p_rid.get_id());
	}

	texture_owner.free(p_rid);
	memdelete(texture);
}

void TextureStorageAVP::texture_2d_initialize(RID p_texture, const Ref<Image> &p_image) {
	AVPTexture *t = texture_owner.get_or_null(p_texture);
	ERR_FAIL_NULL(t);

	t->image = p_image->duplicate();
	t->width = p_image->get_width();
	t->height = p_image->get_height();
	t->format = p_image->get_format();
	t->uploaded_to_bridge = false;
}

void TextureStorageAVP::texture_2d_update(RID p_texture, const Ref<Image> &p_image, int p_layer) {
	AVPTexture *t = texture_owner.get_or_null(p_texture);
	ERR_FAIL_NULL(t);

	t->image = p_image->duplicate();
	t->width = p_image->get_width();
	t->height = p_image->get_height();
	t->format = p_image->get_format();
	t->uploaded_to_bridge = false; // Needs re-upload.
}

Ref<Image> TextureStorageAVP::texture_2d_get(RID p_texture) const {
	AVPTexture *t = texture_owner.get_or_null(p_texture);
	ERR_FAIL_NULL_V(t, Ref<Image>());
	return t->image;
}

void TextureStorageAVP::texture_replace(RID p_texture, RID p_by_texture) {
	AVPTexture *t = texture_owner.get_or_null(p_texture);
	ERR_FAIL_NULL(t);
	AVPTexture *t_by = texture_owner.get_or_null(p_by_texture);
	ERR_FAIL_NULL(t_by);

	t->image = t_by->image;
	t->width = t_by->width;
	t->height = t_by->height;
	t->format = t_by->format;
	t->is_srgb = t_by->is_srgb;
	t->uploaded_to_bridge = false; // Needs re-upload.

	texture_free(p_by_texture);
}

Image::Format TextureStorageAVP::texture_get_format(RID p_texture) const {
	AVPTexture *t = texture_owner.get_or_null(p_texture);
	ERR_FAIL_NULL_V(t, Image::FORMAT_MAX);
	return t->format;
}
