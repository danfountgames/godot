/**************************************************************************/
/*  mcp_agent_state.h                                                     */
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

#ifndef MCP_AGENT_STATE_H
#define MCP_AGENT_STATE_H

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// What the agent says it is doing, and whether it is allowed to carry on.
//
// Two of the six questions the Activity dock must answer come from here:
//
//   * *What is the agent trying to achieve?* - the goal.
//   * *Can I stop it?* - the gate.
//
// The other four are answered by the activity stream (`mcp_activity.h`), which stamps
// every record with whatever this holds at the time.
//
// **Declared, never inferred.** The goal and the current step are text the agent supplies
// about its own work. They are not a reconstruction from tool names, and they are not
// model reasoning: the dock shows what the agent *said* it was doing beside what it
// *actually did*, and the audit log remains the record of the latter. Where the two
// disagree the audit log is right, and that disagreement is worth being able to see.
class MCPAgentIntent {
public:
	// The overall objective of this session, in the user's terms: "Improve the player
	// jump", "Test every menu".
	static void set_goal(const String &p_goal);
	static String get_goal();

	// What the agent is doing right now, as a person would say it: "Measuring the current
	// jump height", "Waiting for the boss health to reach zero". One line.
	static void set_activity(const String &p_activity);
	static String get_activity();

	// Cleared when a session ends, so a stale goal cannot sit over unrelated work.
	static void clear();

	static Dictionary to_dictionary();
};

// Whether the agent may act, and why not when it may not.
//
// A refusal, not a block. A paused tool call could have waited for the user - holding the
// socket open until they resume - but a client with no timeout would hang, and the
// deferred machinery already owns the one long-wait path in this module. Refusing with a
// message the model can act on keeps the failure legible at both ends.
//
// Reads are still allowed while paused. A user who stops an agent mid-change still wants
// to ask it what it did, and refusing `Godot_GetActivity` would take the dock's own data
// source away at the moment it matters most.
class MCPAgentControl {
public:
	enum State {
		STATE_RUNNING,
		STATE_PAUSED, // The user asked it to hold. Resumable.
		STATE_STOPPED, // The user ended it. Needs an explicit resume to act again.
	};

	static State get_state();
	static String state_to_string(State p_state);

	static void pause(const String &p_reason = String());
	static void stop(const String &p_reason = String());
	static void resume();

	// Why the agent is held, for the refusal message and the dock.
	static String get_reason();

	// False when a tool must not run. `r_reason` is filled with something the model can
	// act on rather than a bare "denied".
	static bool may_run(const String &p_tool_name, bool p_is_mutating, String &r_reason);

	static Dictionary to_dictionary();
};

#endif // MCP_AGENT_STATE_H
