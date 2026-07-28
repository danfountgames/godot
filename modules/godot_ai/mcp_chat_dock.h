/**************************************************************************/
/*  mcp_chat_dock.h                                                       */
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

// The chat panel. It renders MCPChat and nothing else decides anything here - what a
// turn is, when it may be sent, what happens to a cancelled one, all of that lives in
// mcp_chat.h where it can be tested without a screen.

#ifndef MCP_CHAT_DOCK_H
#define MCP_CHAT_DOCK_H

#include "mcp_chat.h"

#include "scene/gui/box_container.h"

class Button;
class Label;
class LineEdit;
class RichTextLabel;
class MCPService;

class MCPChatDock : public VBoxContainer {
	GDCLASS(MCPChatDock, VBoxContainer);

	Ref<MCPChat> chat;
	MCPService *service = nullptr;

	RichTextLabel *transcript = nullptr;
	LineEdit *input = nullptr;
	Button *send_button = nullptr;
	Button *cancel_button = nullptr;
	Button *attach_button = nullptr;
	Button *clear_button = nullptr;
	Label *status = nullptr;

	void _send();
	void _cancel();
	void _clear();
	void _attach_edited_scene();
	void _on_chat_changed();

protected:
	void _notification(int p_what);

public:
	explicit MCPChatDock(MCPService *p_service);

	// Refreshes the transcript and the state of every control.
	void refresh();

	// Shown from the command palette: makes the panel visible and puts the caret in
	// the input, so the palette entry is a way to start typing rather than a way to
	// look at a panel.
	void focus_input();

	Ref<MCPChat> get_chat() const { return chat; }
};

#endif // MCP_CHAT_DOCK_H
