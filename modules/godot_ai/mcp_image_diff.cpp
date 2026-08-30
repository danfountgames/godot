/**************************************************************************/
/*  mcp_image_diff.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
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

#include "mcp_image_diff.h"

namespace {

// Largest per-channel difference between two colours, in 0-255 terms. Alpha is
// included: a change that only makes something transparent is still a change, and
// leaving alpha out would hide exactly the kind of bug that is hard to see by eye.
int channel_delta(const Color &p_a, const Color &p_b) {
	const double scale = 255.0;
	int worst = 0;
	worst = MAX(worst, (int)Math::round(Math::abs(p_a.r - p_b.r) * scale));
	worst = MAX(worst, (int)Math::round(Math::abs(p_a.g - p_b.g) * scale));
	worst = MAX(worst, (int)Math::round(Math::abs(p_a.b - p_b.b) * scale));
	worst = MAX(worst, (int)Math::round(Math::abs(p_a.a - p_b.a) * scale));
	return worst;
}

} // namespace

Dictionary MCPImageDiff::Result::to_dictionary() const {
	Dictionary out;
	out["comparable"] = comparable;
	out["verdict"] = verdict;
	out["width"] = width;
	out["height"] = height;
	out["pixels"] = pixels;
	out["changed_pixels"] = changed;
	out["changed_fraction"] = fraction;
	out["max_channel_delta"] = max_channel_delta;
	if (changed > 0) {
		Dictionary box;
		box["x"] = bounds.position.x;
		box["y"] = bounds.position.y;
		box["width"] = bounds.size.x;
		box["height"] = bounds.size.y;
		// The box is what makes this actionable rather than a number: it is where to
		// point a person, or where to crop the next capture.
		out["changed_bounds"] = box;
	}
	return out;
}

MCPImageDiff::Result MCPImageDiff::compare(const Ref<Image> &p_before, const Ref<Image> &p_after, int p_tolerance) {
	Result result;
	if (p_before.is_null() || p_after.is_null()) {
		result.verdict = "incomparable";
		return result;
	}
	if (p_before->get_width() != p_after->get_width() || p_before->get_height() != p_after->get_height()) {
		// Not compared over the overlap. A resized window is a different question from
		// a changed one, and answering the wrong question quietly is worse than saying
		// the two cannot be compared.
		result.verdict = "incomparable";
		return result;
	}

	result.comparable = true;
	result.width = p_after->get_width();
	result.height = p_after->get_height();
	result.pixels = (int64_t)result.width * (int64_t)result.height;
	if (result.pixels == 0) {
		result.verdict = "identical";
		return result;
	}

	const int tolerance = MAX(0, p_tolerance);
	int min_x = result.width;
	int min_y = result.height;
	int max_x = -1;
	int max_y = -1;

	for (int y = 0; y < result.height; y++) {
		for (int x = 0; x < result.width; x++) {
			const int delta = channel_delta(p_before->get_pixel(x, y), p_after->get_pixel(x, y));
			// Tracked even below tolerance, so a comparison that says "identical" can
			// still show it was nearly not.
			result.max_channel_delta = MAX(result.max_channel_delta, delta);
			if (delta <= tolerance) {
				continue;
			}
			result.changed++;
			min_x = MIN(min_x, x);
			min_y = MIN(min_y, y);
			max_x = MAX(max_x, x);
			max_y = MAX(max_y, y);
		}
	}

	result.fraction = (double)result.changed / (double)result.pixels;
	if (result.changed == 0) {
		result.verdict = "identical";
	} else {
		result.bounds = Rect2i(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
		result.verdict = result.fraction >= SUBSTANTIAL_FRACTION ? "substantial" : "minor";
	}
	return result;
}

Ref<Image> MCPImageDiff::render(const Ref<Image> &p_before, const Ref<Image> &p_after, int p_tolerance) {
	if (p_before.is_null() || p_after.is_null() ||
			p_before->get_width() != p_after->get_width() ||
			p_before->get_height() != p_after->get_height()) {
		return Ref<Image>();
	}

	const int width = p_after->get_width();
	const int height = p_after->get_height();
	Ref<Image> out = Image::create_empty(width, height, false, Image::FORMAT_RGBA8);
	if (out.is_null()) {
		return out;
	}

	const int tolerance = MAX(0, p_tolerance);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const Color after = p_after->get_pixel(x, y);
			const int delta = channel_delta(p_before->get_pixel(x, y), after);
			if (delta <= tolerance) {
				// Dimmed, not blanked. A bare mask shows the shape of the change and
				// not what changed, and somebody is going to look at this.
				const float grey = (after.r + after.g + after.b) / 3.0f;
				out->set_pixel(x, y, Color(grey * 0.25f, grey * 0.25f, grey * 0.3f, 1.0f));
			} else {
				// Marked in magenta, scaled by how big the change was, so a subtle
				// shift and a wholesale repaint do not look the same.
				const float strength = CLAMP((float)delta / 255.0f, 0.35f, 1.0f);
				out->set_pixel(x, y, Color(strength, 0.0f, strength * 0.85f, 1.0f));
			}
		}
	}
	return out;
}

String MCPImageDiff::describe(const Result &p_result) {
	if (!p_result.comparable) {
		return "The two captures are different sizes, so nothing was compared. Take both at "
			   "the same resolution - a resized window is a different question from a changed one.";
	}
	if (p_result.changed == 0) {
		return vformat("Nothing changed. The largest single-channel difference was %d, within the "
					   "tolerance, which is what an unchanged scene captured twice looks like.",
				p_result.max_channel_delta);
	}
	return vformat("%s change: %d of %d pixels (%.2f%%) differ, the largest by %d of 255, all "
				   "within %dx%d at (%d, %d).",
			p_result.verdict == "substantial" ? "Substantial" : "Minor",
			p_result.changed, p_result.pixels, p_result.fraction * 100.0, p_result.max_channel_delta,
			p_result.bounds.size.x, p_result.bounds.size.y,
			p_result.bounds.position.x, p_result.bounds.position.y);
}
