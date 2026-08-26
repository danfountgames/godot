/**************************************************************************/
/*  mcp_runtime_instances.h                                               */
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

#ifndef MCP_RUNTIME_INSTANCES_H
#define MCP_RUNTIME_INSTANCES_H

#include "core/os/process_id.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class ScriptEditorDebugger;

// Which game processes this editor's agent owns, and how to talk to exactly one of them.
//
// Two problems, one registry, because they share a key.
//
// **Ownership.** The editor can run several games at once, but nothing distinguishes the
// one a human pressed play on from the three an agent launched to compare. Stopping "the
// game" therefore means stopping all of them. Every agent-owned process is registered
// here with a stable id, so it can be listed, addressed and shut down without touching
// the user's own run.
//
// **Routing.** `GameViewDebugger::set_suspend()`, `next_frame()` and `set_time_scale()`
// iterate every active debugger session and send to all of them, so pausing one instance
// pauses all of them. The join key that fixes it already exists in the editor:
// `ScriptEditorDebugger::get_remote_pid()`, which `GameView` already uses to find the
// session belonging to its embedded process. This generalises that one lookup.
//
// Nothing here launches or embeds anything. It records what exists and addresses it.
class MCPRuntimeInstances {
public:
	// Where an instance is in its life. Named for what a person watching would say,
	// because these strings reach the dock.
	enum Lifecycle {
		LIFECYCLE_QUEUED,
		LIFECYCLE_LAUNCHING,
		LIFECYCLE_EMBEDDING,
		LIFECYCLE_RUNNING,
		LIFECYCLE_FAILED,
		LIFECYCLE_CLOSED,
	};

	// Who is driving. Never inferred - an instance the human took over must not receive
	// injected input, and the only way to be sure is to record it.
	enum Control {
		CONTROL_AGENT,
		CONTROL_HUMAN,
		CONTROL_OBSERVER,
	};

	// What should happen to the instance when its work finishes.
	enum Retention {
		RETENTION_EPHEMERAL, // A quick test; may collapse to a result card.
		RETENTION_INTERACTIVE, // The user is expected to look at or play it.
		RETENTION_PERSISTENT, // A long playtest; only an explicit stop ends it.
		RETENTION_BACKGROUND, // Runs on without a live view.
	};

	struct Instance {
		String instance_id;
		String label;
		String role;
		String task;
		String group;
		ProcessID pid = 0;
		Lifecycle lifecycle = LIFECYCLE_QUEUED;
		Control control = CONTROL_AGENT;
		Retention retention = RETENTION_EPHEMERAL;
		int64_t started_msec = 0;
		int64_t ended_msec = 0;
		String detail;
	};

	static String lifecycle_to_string(Lifecycle p_lifecycle);
	static String control_to_string(Control p_control);
	static String retention_to_string(Retention p_retention);

	// Registers an instance before it launches and returns its id. The id is stable for
	// the instance's whole life, including after the process exits, so evidence can
	// still be attributed to it.
	static String create(const String &p_label, const String &p_role, const String &p_task,
			Retention p_retention);

	// Binds a launched process to its registration. Refuses a pid already bound to a
	// different live instance - two registrations pointing at one process would make
	// "stop that one" ambiguous.
	static bool bind_pid(const String &p_instance_id, ProcessID p_pid);

	static bool set_lifecycle(const String &p_instance_id, Lifecycle p_lifecycle, const String &p_detail = String());
	static bool set_control(const String &p_instance_id, Control p_control);
	static bool set_group(const String &p_instance_id, const String &p_group);

	static bool get(const String &p_instance_id, Instance &r_instance);
	static bool exists(const String &p_instance_id);

	// Every registration, oldest first, closed ones included.
	static Vector<Instance> list();
	// Only those with a live process.
	static Vector<Instance> live();

	// The instance owning a process, or an empty string. This is what keeps an agent
	// action off the human's own run.
	static String find_by_pid(ProcessID p_pid);

	static bool is_agent_owned(ProcessID p_pid);

	// Drops the registration entirely. Prefer set_lifecycle(CLOSED), which keeps the
	// evidence trail; use this only when a registration was made in error.
	static bool remove(const String &p_instance_id);
	static void clear();

	// As Dictionaries, for the dock and for tools.
	static Array to_array();
	static Dictionary to_dictionary(const Instance &p_instance);

	// ---------------------------------------------------------------- routing ---

	// The debugger session talking to this instance's process, or null. Needs an editor;
	// returns null headlessly, which every caller must handle.
	static ScriptEditorDebugger *debugger_for(const String &p_instance_id);

	// Sends one debugger message to one instance. False when there is no editor, no
	// such instance, or its session is not active.
	static bool send_to(const String &p_instance_id, const String &p_message, const Array &p_arguments);

	// Targeted equivalents of the Game workspace's controls, which broadcast to every
	// session. Each affects exactly one instance.
	static bool set_suspended(const String &p_instance_id, bool p_suspended);
	static bool next_frame(const String &p_instance_id);
	static bool set_time_scale(const String &p_instance_id, double p_scale);
	static bool set_muted(const String &p_instance_id, bool p_muted);

private:
	static Vector<Instance> &_all();
	static int _index_of(const String &p_instance_id);
};

#endif // MCP_RUNTIME_INSTANCES_H
