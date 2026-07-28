/**************************************************************************/
/*  mcp_capture_metadata.h                                                */
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

#ifndef MCP_CAPTURE_METADATA_H
#define MCP_CAPTURE_METADATA_H

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// What a screenshot is a picture of.
//
// An image on its own is not evidence. Three tools here return one, of three different
// things - the editor's viewport, the whole screen, and the running game - and once a
// capture has been saved to a file and quoted in a report there is nothing left to say
// which it was, when it was taken, or whether the game was running at five times speed
// at the time. Every one of those has been mistaken for another in practice.
//
// So every capture carries the same block: what it shows, precisely what was
// photographed, and when. Tools add their own facts on top - a frame number, a time
// scale - and anything that undermines the image as evidence goes in `note`.
class MCPCaptureMetadata {
public:
	// The three things this product can photograph. Deliberately a short closed set: a
	// caller has to be able to branch on it.
	static const char *SOURCE_EDITOR_VIEWPORT;
	static const char *SOURCE_EDITOR_SCREEN;
	static const char *SOURCE_GAME_WINDOW;

	// Stamps `r_result` with source, subject and the time of capture.
	static void stamp(Dictionary &r_result, const String &p_source, const String &p_subject);

	// Appends a caveat, keeping any already there. Notes accumulate because a capture
	// can be doubtful for more than one reason at once.
	static void add_note(Dictionary &r_result, const String &p_note);

	// The properties every capture result declares, so discovery and execution agree.
	static void declare(Dictionary &r_properties);
};

#endif // MCP_CAPTURE_METADATA_H
