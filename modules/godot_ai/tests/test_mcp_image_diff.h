/**************************************************************************/
/*  test_mcp_image_diff.h                                                 */
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

#ifndef TEST_MCP_IMAGE_DIFF_H
#define TEST_MCP_IMAGE_DIFF_H

#include "modules/godot_ai/mcp_image_diff.h"

#include "tests/test_macros.h"

namespace TestMCPImageDiff {

static Ref<Image> filled(int p_width, int p_height, const Color &p_colour) {
	Ref<Image> image = Image::create_empty(p_width, p_height, false, Image::FORMAT_RGBA8);
	image->fill(p_colour);
	return image;
}

TEST_CASE("[godot_ai] Two identical captures report no change") {
	const Ref<Image> before = filled(32, 32, Color(0.2, 0.4, 0.6));
	const Ref<Image> after = filled(32, 32, Color(0.2, 0.4, 0.6));

	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK(result.comparable);
	CHECK(result.verdict == "identical");
	CHECK(result.changed == 0);
	CHECK(result.pixels == 32 * 32);
	CHECK(result.fraction == doctest::Approx(0.0));
}

TEST_CASE("[godot_ai] Renderer noise below the tolerance is not a change") {
	// The reason the tolerance is not zero: two captures of the same unchanged scene
	// are not bit-identical - dithering, text antialiasing and float rounding all move
	// a channel by one or two - and zero would report a difference on every call.
	const Ref<Image> before = filled(16, 16, Color(100 / 255.0, 100 / 255.0, 100 / 255.0));
	const Ref<Image> after = filled(16, 16, Color(103 / 255.0, 97 / 255.0, 100 / 255.0));

	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK(result.verdict == "identical");
	CHECK(result.changed == 0);
	// Still reported, so "identical" can be seen to have been close.
	CHECK(result.max_channel_delta == 3);

	// With no tolerance at all, the same pair is a wholesale change.
	const MCPImageDiff::Result strict = MCPImageDiff::compare(before, after, 0);
	CHECK(strict.verdict == "substantial");
	CHECK(strict.changed == 16 * 16);
}

TEST_CASE("[godot_ai] A changed region is located, not just counted") {
	Ref<Image> before = filled(100, 100, Color(0, 0, 0));
	Ref<Image> after = filled(100, 100, Color(0, 0, 0));
	for (int y = 20; y < 30; y++) {
		for (int x = 40; x < 50; x++) {
			after->set_pixel(x, y, Color(1, 1, 1));
		}
	}

	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK(result.changed == 100);
	// The box is what makes the answer actionable: where to point a person, or where
	// to crop the next capture.
	CHECK(result.bounds == Rect2i(40, 20, 10, 10));
	CHECK(result.max_channel_delta == 255);
	// 100 of 10000 is one percent, under the substantial threshold.
	CHECK(result.verdict == "minor");
	CHECK(MCPImageDiff::describe(result).contains("10x10 at (40, 20)"));
}

TEST_CASE("[godot_ai] A large change reads as substantial") {
	Ref<Image> before = filled(50, 50, Color(0, 0, 0));
	Ref<Image> after = filled(50, 50, Color(0, 0, 0));
	for (int y = 0; y < 25; y++) {
		for (int x = 0; x < 50; x++) {
			after->set_pixel(x, y, Color(1, 0, 0));
		}
	}

	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK(result.fraction == doctest::Approx(0.5));
	CHECK(result.verdict == "substantial");
	CHECK(MCPImageDiff::describe(result).begins_with("Substantial"));
}

TEST_CASE("[godot_ai] A transparency-only change still counts") {
	// Alpha is compared for exactly this: a change that only makes something
	// transparent is hard to see by eye and easy to ship.
	Ref<Image> before = filled(8, 8, Color(1, 1, 1, 1));
	Ref<Image> after = filled(8, 8, Color(1, 1, 1, 0));

	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK(result.changed == 64);
	CHECK(result.max_channel_delta == 255);
}

TEST_CASE("[godot_ai] Different sizes are refused, not compared over the overlap") {
	const Ref<Image> before = filled(32, 32, Color(1, 1, 1));
	const Ref<Image> after = filled(64, 32, Color(1, 1, 1));

	// A resized window is a different question from a changed one, and answering the
	// wrong one quietly is worse than refusing.
	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK_FALSE(result.comparable);
	CHECK(result.verdict == "incomparable");
	CHECK(result.changed == 0);
	CHECK(MCPImageDiff::describe(result).contains("different sizes"));

	CHECK(MCPImageDiff::render(before, after).is_null());
}

TEST_CASE("[godot_ai] A null image is incomparable rather than a crash") {
	const Ref<Image> real = filled(4, 4, Color(1, 1, 1));
	CHECK_FALSE(MCPImageDiff::compare(Ref<Image>(), real).comparable);
	CHECK_FALSE(MCPImageDiff::compare(real, Ref<Image>()).comparable);
	CHECK(MCPImageDiff::render(Ref<Image>(), real).is_null());
}

TEST_CASE("[godot_ai] The difference image marks what changed and keeps the rest visible") {
	Ref<Image> before = filled(10, 10, Color(0.5, 0.5, 0.5));
	Ref<Image> after = filled(10, 10, Color(0.5, 0.5, 0.5));
	after->set_pixel(3, 3, Color(1, 1, 0));

	const Ref<Image> rendered = MCPImageDiff::render(before, after);
	REQUIRE(rendered.is_valid());
	CHECK(rendered->get_width() == 10);
	CHECK(rendered->get_height() == 10);

	// Changed pixels are marked in magenta.
	const Color marked = rendered->get_pixel(3, 3);
	CHECK(marked.r > 0.3f);
	CHECK(marked.g == doctest::Approx(0.0f));
	CHECK(marked.b > 0.2f);

	// Unchanged ones are dimmed rather than blanked, so the picture still shows what
	// the frame was - a bare mask gives the shape of a change and not its context.
	const Color context = rendered->get_pixel(0, 0);
	CHECK(context.r > 0.0f);
	CHECK(context.r < 0.5f);
	CHECK(context.a == doctest::Approx(1.0f));
}

TEST_CASE("[godot_ai] An empty image compares without dividing by zero") {
	const Ref<Image> before = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	const Ref<Image> after = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	const MCPImageDiff::Result result = MCPImageDiff::compare(before, after);
	CHECK(result.comparable);
	CHECK(result.pixels == 1);
}

} // namespace TestMCPImageDiff

#endif // TEST_MCP_IMAGE_DIFF_H
