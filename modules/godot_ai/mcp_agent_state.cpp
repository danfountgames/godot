/**************************************************************************/
/*  mcp_agent_state.cpp                                                   */
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

#include "mcp_agent_state.h"

#include "core/string/print_string.h"
#include "core/variant/variant.h"

namespace {

// Function-local statics: engine string containers at namespace scope are constructed
// before the memory subsystem exists. That cost this tranche a day once already.
String &goal_text() {
	static String goal;
	return goal;
}

String &activity_text() {
	static String activity;
	return activity;
}

MCPAgentControl::State &control_state() {
	static MCPAgentControl::State state = MCPAgentControl::STATE_RUNNING;
	return state;
}

String &control_reason() {
	static String reason;
	return reason;
}

// Tools that stay available while the agent is held.
//
// A user who stops an agent mid-change still wants to ask what it did, and the dock
// reads the stream through a tool like anything else - refusing these would remove its
// data source at the moment it matters most. Every one of them is read-only.
bool is_always_allowed(const String &p_tool_name) {
	return p_tool_name == "Godot_GetActivity" ||
			p_tool_name == "Godot_GetEditorStatus" ||
			p_tool_name == "Godot_ListInstances" ||
			p_tool_name == "Godot_ListCheckpoints" ||
			p_tool_name == "Godot_DiffCheckpoint";
}

} // namespace

void MCPAgentIntent::set_goal(const String &p_goal) {
	goal_text() = p_goal;
}

String MCPAgentIntent::get_goal() {
	return goal_text();
}

void MCPAgentIntent::set_activity(const String &p_activity) {
	activity_text() = p_activity;
}

String MCPAgentIntent::get_activity() {
	return activity_text();
}

void MCPAgentIntent::clear() {
	goal_text() = String();
	activity_text() = String();
}

Dictionary MCPAgentIntent::to_dictionary() {
	Dictionary out;
	out["goal"] = goal_text();
	out["activity"] = activity_text();
	return out;
}

MCPAgentControl::State MCPAgentControl::get_state() {
	return control_state();
}

String MCPAgentControl::state_to_string(State p_state) {
	switch (p_state) {
		case STATE_RUNNING:
			return "running";
		case STATE_PAUSED:
			return "paused";
		case STATE_STOPPED:
			return "stopped";
	}
	return "unknown";
}

void MCPAgentControl::pause(const String &p_reason) {
	control_state() = STATE_PAUSED;
	control_reason() = p_reason.is_empty() ? String("the user paused this agent") : p_reason;
}

void MCPAgentControl::stop(const String &p_reason) {
	control_state() = STATE_STOPPED;
	control_reason() = p_reason.is_empty() ? String("the user stopped this agent") : p_reason;
}

void MCPAgentControl::resume() {
	control_state() = STATE_RUNNING;
	control_reason() = String();
}

String MCPAgentControl::get_reason() {
	return control_reason();
}

bool MCPAgentControl::may_run(const String &p_tool_name, bool p_is_mutating, String &r_reason) {
	if (control_state() == STATE_RUNNING) {
		return true;
	}
	if (is_always_allowed(p_tool_name)) {
		return true;
	}
	// Say what happened, who did it and what would clear it. A model that receives only
	// "denied" retries; one that is told a person pressed Stop can explain itself and
	// wait.
	const String held = control_state() == STATE_PAUSED ? "paused" : "stopped";
	r_reason = vformat(
			"'%s' did not run because this agent is %s: %s. Nothing was changed. Reads that "
			"explain what has already happened - Godot_GetActivity, Godot_ListCheckpoints, "
			"Godot_DiffCheckpoint - still work. The user resumes from the Agent Activity dock.",
			p_tool_name, held, control_reason());
	// Named but unused: every held tool is refused, mutating or not, because a paused
	// agent that keeps reading the project is still working.
	(void)p_is_mutating;
	return false;
}

Dictionary MCPAgentControl::to_dictionary() {
	Dictionary out;
	out["state"] = state_to_string(control_state());
	out["reason"] = control_reason();
	return out;
}
