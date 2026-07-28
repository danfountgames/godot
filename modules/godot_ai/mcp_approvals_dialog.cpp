/**************************************************************************/
/*  mcp_approvals_dialog.cpp                                              */
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

#include "mcp_approvals_dialog.h"

#include "mcp_service.h"
#include "mcp_skills.h"

#include "scene/gui/box_container.h"

MCPApprovalsDialog::MCPApprovalsDialog(MCPService *p_service) :
		service(p_service) {
	set_title(TTR("Godot AI: Clients and Skills"));
	set_min_size(Size2(560, 360));
	set_ok_button_text(TTR("Close"));

	VBoxContainer *layout = memnew(VBoxContainer);
	add_child(layout);

	summary = memnew(Label);
	summary->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	layout->add_child(summary);

	tree = memnew(Tree);
	tree->set_columns(COLUMN_MAX);
	tree->set_column_titles_visible(true);
	tree->set_column_title(COLUMN_NAME, TTR("Name"));
	tree->set_column_title(COLUMN_KIND, TTR("Kind"));
	tree->set_column_title(COLUMN_STATUS, TTR("Status"));
	tree->set_column_title(COLUMN_ACTION, TTR("Action"));
	tree->set_column_expand(COLUMN_KIND, false);
	tree->set_column_expand(COLUMN_ACTION, false);
	tree->set_column_custom_minimum_width(COLUMN_KIND, 90);
	tree->set_column_custom_minimum_width(COLUMN_STATUS, 200);
	tree->set_column_custom_minimum_width(COLUMN_ACTION, 110);
	tree->set_hide_root(true);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->connect("button_clicked", callable_mp(this, &MCPApprovalsDialog::_button_pressed));
	layout->add_child(tree);
}

void MCPApprovalsDialog::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED && is_visible()) {
		refresh();
	}
}

void MCPApprovalsDialog::refresh() {
	tree->clear();
	TreeItem *root = tree->create_item();

	int pending_count = 0;

	if (service) {
		for (const String &client : service->get_pending_clients()) {
			TreeItem *item = tree->create_item(root);
			item->set_text(COLUMN_NAME, client);
			item->set_text(COLUMN_KIND, TTR("Client"));
			item->set_text(COLUMN_STATUS, TTR("Waiting for approval"));
			item->set_meta("kind", "client");
			item->set_meta("name", client);
			item->add_button(COLUMN_ACTION, Ref<Texture2D>(), BUTTON_TOGGLE, false, TTR("Allow"));
			pending_count++;
		}
	}
	if (MCPService::is_client_approved(String())) {
		// Approval is currently blanket-enabled; say so instead of listing nothing.
		TreeItem *item = tree->create_item(root);
		item->set_text(COLUMN_NAME, TTR("(all clients)"));
		item->set_text(COLUMN_KIND, TTR("Client"));
		item->set_text(COLUMN_STATUS, TTR("Approval is disabled - every client is accepted"));
	}

	for (const MCPSkill &skill : MCPSkills::discover()) {
		TreeItem *item = tree->create_item(root);
		item->set_text(COLUMN_NAME, skill.name);
		item->set_text(COLUMN_KIND, TTR("Skill"));

		String status;
		if (!skill.problem.is_empty()) {
			status = skill.problem;
		} else if (!skill.enabled) {
			status = TTR("Disabled by the skill itself");
		} else if (!skill.version_supported) {
			status = vformat(TTR("Needs editor version %s"), skill.required_editor_version);
		} else if (skill.allowed) {
			status = vformat(TTR("Allowed (%s)"), skill.root_kind);
		} else {
			status = vformat(TTR("Not allowed (%s)"), skill.root_kind);
			pending_count++;
		}
		item->set_text(COLUMN_STATUS, status);
		item->set_tooltip_text(COLUMN_NAME, skill.description.is_empty() ? skill.path : skill.description);
		item->set_meta("kind", "skill");
		item->set_meta("name", skill.name);

		// A skill that cannot run is not a decision the user should be offered.
		if (skill.problem.is_empty() && skill.enabled && skill.version_supported) {
			item->add_button(COLUMN_ACTION, Ref<Texture2D>(), BUTTON_TOGGLE, false,
					skill.allowed ? TTR("Revoke") : TTR("Allow"));
		}
	}

	if (pending_count > 0) {
		summary->set_text(vformat(TTR("%d item(s) are waiting for your decision. Nothing an AI client "
									  "asks for runs until you allow it here."),
				pending_count));
	} else {
		summary->set_text(TTR("Nothing is waiting for a decision."));
	}
}

void MCPApprovalsDialog::_button_pressed(TreeItem *p_item, int p_column, int p_id, MouseButton p_button) {
	if (!p_item || p_id != BUTTON_TOGGLE) {
		return;
	}
	const String kind = p_item->get_meta("kind", String());
	const String name = p_item->get_meta("name", String());
	if (name.is_empty()) {
		return;
	}

	if (kind == "client" && service) {
		service->approve_client_name(name);
	} else if (kind == "skill") {
		MCPSkills::set_allowed(name, !MCPSkills::is_allowed(name));
	}
	refresh();
}
