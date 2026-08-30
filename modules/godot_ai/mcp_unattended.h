/**************************************************************************/
/*  mcp_unattended.h                                                      */
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

#ifndef MCP_UNATTENDED_H
#define MCP_UNATTENDED_H

#include "mcp_types.h"

#include "core/string/ustring.h"

// Running with nobody watching: CI, a scripted batch, an agent driving the editor
// from a shell.
//
// This is a first-class mode rather than a degraded one, and it needs two things the
// interactive path gets for free.
//
// **Somebody to ask.** A tool that waits for a human must know whether one exists.
// Checking for an EditorNode does not answer that: `--headless --editor` has a full
// EditorNode and no human at all, so a dialog opens on the dummy display server and
// the caller waits out its entire timeout - five minutes of a CI job, for a question
// nobody will ever see. The question to ask is about the display, not the editor.
//
// **A policy that can be stated up front.** Deny-by-default is right, but a headless
// run has no settings dialog to change it in, and blanket auto-approval is a poor
// substitute for saying what an unattended agent may do. `GODOT_AI_POLICY` lets an
// operator declare exactly that, per capability, in the environment that starts the
// editor.
class MCPUnattended {
public:
	// Environment variables, named here so the documentation and the code cannot
	// drift apart.
	static const char *ENV_POLICY; // "read_project=allow,edit_files=deny"
	static const char *ENV_UNATTENDED; // "1" - declare no human even with a display.
	static const char *ENV_APPROVE_CLIENTS; // "1" - any client may connect.
	static const char *ENV_AUTO_APPROVE; // "1" - the blanket switch, kept for CI.

	// True when there is nobody to show a dialog to: no DisplayServer, the headless
	// driver, or an operator declaring it with GODOT_AI_UNATTENDED=1.
	static bool is_unattended();

	// What a tool that needs a human should return instead of opening a dialog and
	// waiting. Names the thing that cannot be shown, and what to do instead.
	static String no_user_reason(const String &p_what, const String &p_alternative = String());

	// True when clients may connect without anyone approving them by hand.
	static bool clients_pre_approved();

	// Per-capability policy declared in GODOT_AI_POLICY. False when the variable is
	// unset or says nothing about this capability, in which case the ordinary
	// editor-settings path decides.
	//
	// This can never reach `dangerous_exec`: that capability is refused in
	// MCPPermissions::evaluate() before any policy is consulted, and parse_policy
	// rejects it outright so a configuration that tries is reported rather than
	// silently ignored.
	static bool policy_override(MCPCapability p_capability, MCPPolicy &r_policy);

	// Parses the policy string. Free of the environment so it can be tested directly.
	// Fills r_policies (indexed by capability) and r_set, and returns false with
	// r_error on a malformed entry - a misspelt capability in CI must be reported,
	// not quietly dropped into whatever the default was.
	static bool parse_policy(const String &p_text, MCPPolicy *r_policies, bool *r_set, String &r_error);

	// Reports the effective unattended configuration once, at startup, so a CI log
	// records what the agent was permitted to do. Empty when nothing is configured.
	static String describe();

	// Test seams: the doctest binary has a real display and no environment.
	static void set_environment_override(const String &p_policy, bool p_unattended, bool p_approve_clients);
	static void clear_environment_override();
};

#endif // MCP_UNATTENDED_H
