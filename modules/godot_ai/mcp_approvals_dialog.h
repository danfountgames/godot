/**************************************************************************/
/*  mcp_approvals_dialog.h                                                */
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

#ifndef MCP_APPROVALS_DIALOG_H
#define MCP_APPROVALS_DIALOG_H

#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/tree.h"

class MCPService;

// The one place a user can see and change what the AI service is allowed to do.
//
// Approvals are stored in editor settings, which the Editor Settings window can
// already display as raw arrays. That is not a decision surface: it shows names with
// no context, and a client that is waiting to connect does not appear there at all.
// This dialog lists what is actually pending, with the reason it is pending.
class MCPApprovalsDialog : public AcceptDialog {
	GDCLASS(MCPApprovalsDialog, AcceptDialog);

	enum Column {
		COLUMN_NAME,
		COLUMN_KIND,
		COLUMN_STATUS,
		COLUMN_ACTION,
		COLUMN_MAX,
	};

	enum ButtonId {
		BUTTON_TOGGLE,
	};

	MCPService *service = nullptr;
	Tree *tree = nullptr;
	Label *summary = nullptr;

	void _button_pressed(TreeItem *p_item, int p_column, int p_id, MouseButton p_button);

protected:
	void _notification(int p_what);

public:
	explicit MCPApprovalsDialog(MCPService *p_service);

	// Rebuilds from the current service and skill state. Called whenever the dialog
	// is shown, because both can change while it is closed.
	void refresh();
};

#endif // MCP_APPROVALS_DIALOG_H
