/**************************************************************************/
/*  mcp_capture_metadata.cpp                                              */
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

#include "mcp_capture_metadata.h"

#include "mcp_schema.h"

#include "core/os/time.h"

const char *MCPCaptureMetadata::SOURCE_EDITOR_VIEWPORT = "editor_viewport";
const char *MCPCaptureMetadata::SOURCE_EDITOR_SCREEN = "editor_screen";
const char *MCPCaptureMetadata::SOURCE_GAME_WINDOW = "game_window";

void MCPCaptureMetadata::stamp(Dictionary &r_result, const String &p_source, const String &p_subject) {
	r_result["source"] = p_source;
	r_result["subject"] = p_subject;
	// The editor's wall clock, in the local zone with an offset, so a capture can be
	// lined up against an output log or a session note without arithmetic.
	const Time *time = Time::get_singleton();
	r_result["captured_at"] = time->get_datetime_string_from_system(false, false) +
			time->get_offset_string_from_offset_minutes(
					(int64_t)time->get_time_zone_from_system().get("bias", 0));
}

void MCPCaptureMetadata::add_note(Dictionary &r_result, const String &p_note) {
	const String existing = r_result.get("note", String());
	r_result["note"] = existing.is_empty() ? p_note : existing + " " + p_note;
}

void MCPCaptureMetadata::declare(Dictionary &r_properties) {
	r_properties["source"] = MCPSchema::string_property(
			"What was photographed: 'editor_viewport', 'editor_screen' or 'game_window'.");
	r_properties["subject"] = MCPSchema::string_property(
			"Precisely what that was - a window title, or the viewport's description.");
	r_properties["captured_at"] = MCPSchema::string_property(
			"When the image was taken, by the editor's clock, with a UTC offset.");
	r_properties["note"] = MCPSchema::string_property(
			"Anything that limits this image as evidence. Absent when there is nothing to say.");
}
