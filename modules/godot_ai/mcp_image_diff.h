/**************************************************************************/
/*  mcp_image_diff.h                                                      */
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

#ifndef MCP_IMAGE_DIFF_H
#define MCP_IMAGE_DIFF_H

#include "core/io/image.h"
#include "core/math/rect2i.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// Comparing two captures, so evidence about a visual medium can be visual.
//
// Screenshots already existed and were treated as attachments: taken, saved, and
// described in prose. Nothing could answer the question a change actually raises -
// "did this alter what the player sees, and where?" - without a person opening two
// files and looking between them.
//
// Everything here is arithmetic on two images, so it is testable on synthetic ones
// with no display, which matters because the thing it exists to support is the part of
// the stack a display is otherwise required for.
class MCPImageDiff {
public:
	// Per-channel difference below this is treated as equal. Not zero: a capture of the
	// same unchanged scene twice is not bit-identical - dithering, text antialiasing and
	// float rounding in the renderer all move a channel by one or two - and a comparison
	// that called that a change would report a difference on every single call.
	static constexpr int DEFAULT_TOLERANCE = 8;

	// Below this fraction of changed pixels the result reads as "minor". Above it,
	// "substantial". A threshold has to exist for the verdict to mean anything, and
	// naming it here beats each caller inventing one.
	static constexpr double SUBSTANTIAL_FRACTION = 0.02;

	struct Result {
		bool comparable = false; // False when the two differ in size.
		int width = 0;
		int height = 0;
		int64_t pixels = 0;
		int64_t changed = 0;
		double fraction = 0.0; // changed / pixels.
		int max_channel_delta = 0; // The largest single-channel difference found.
		Rect2i bounds; // Tightest box containing every changed pixel. Empty when none.
		String verdict; // "identical" | "minor" | "substantial"

		Dictionary to_dictionary() const;
	};

	// Compares two images pixel by pixel. Both are read at their own format; the caller
	// does not have to convert. Images of different sizes are not compared at all
	// rather than compared over their overlap: a resized window is a different question
	// from a changed one, and answering the wrong one quietly is worse than refusing.
	static Result compare(const Ref<Image> &p_before, const Ref<Image> &p_after, int p_tolerance = DEFAULT_TOLERANCE);

	// A picture of the difference: the "after" image dimmed, with changed pixels marked.
	// Returns an invalid ref when the two are not comparable.
	//
	// Dimmed rather than blanked, because a bare mask of changed pixels tells you the
	// shape of the change and not what changed - and the whole point of producing an
	// image instead of a number is that somebody is going to look at it.
	static Ref<Image> render(const Ref<Image> &p_before, const Ref<Image> &p_after, int p_tolerance = DEFAULT_TOLERANCE);

	// One line a person can read: what changed, how much of the frame, and where.
	static String describe(const Result &p_result);
};

#endif // MCP_IMAGE_DIFF_H
