/**************************************************************************/
/*  mcp_runtime_instances.cpp                                             */
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

#include "mcp_runtime_instances.h"

#include "core/os/os.h"

#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"

Vector<MCPRuntimeInstances::Instance> &MCPRuntimeInstances::_all() {
	// Function-local: the registry holds engine containers, and a namespace-scope one is
	// constructed before the memory subsystem exists. That mistake already cost this
	// tranche a day of chasing an intermittent segfault.
	static Vector<Instance> instances;
	return instances;
}

String MCPRuntimeInstances::lifecycle_to_string(Lifecycle p_lifecycle) {
	switch (p_lifecycle) {
		case LIFECYCLE_QUEUED:
			return "queued";
		case LIFECYCLE_LAUNCHING:
			return "launching";
		case LIFECYCLE_EMBEDDING:
			return "embedding";
		case LIFECYCLE_RUNNING:
			return "running";
		case LIFECYCLE_FAILED:
			return "failed";
		case LIFECYCLE_CLOSED:
			return "closed";
	}
	return "unknown";
}

String MCPRuntimeInstances::control_to_string(Control p_control) {
	switch (p_control) {
		case CONTROL_AGENT:
			return "agent";
		case CONTROL_HUMAN:
			return "human";
		case CONTROL_OBSERVER:
			return "observer";
	}
	return "unknown";
}

String MCPRuntimeInstances::retention_to_string(Retention p_retention) {
	switch (p_retention) {
		case RETENTION_EPHEMERAL:
			return "ephemeral";
		case RETENTION_INTERACTIVE:
			return "interactive";
		case RETENTION_PERSISTENT:
			return "persistent";
		case RETENTION_BACKGROUND:
			return "background";
	}
	return "unknown";
}

int MCPRuntimeInstances::_index_of(const String &p_instance_id) {
	const Vector<Instance> &instances = _all();
	for (int i = 0; i < instances.size(); i++) {
		if (instances[i].instance_id == p_instance_id) {
			return i;
		}
	}
	return -1;
}

String MCPRuntimeInstances::create(const String &p_label, const String &p_role,
		const String &p_task, Retention p_retention) {
	static uint64_t counter = 0;
	counter++;

	Instance instance;
	// Monotonic rather than random: ids appear in logs and reports, and a reader should
	// be able to tell which came first.
	instance.instance_id = vformat("inst-%d", counter);
	instance.label = p_label.is_empty() ? instance.instance_id : p_label;
	instance.role = p_role;
	instance.task = p_task;
	instance.retention = p_retention;
	instance.lifecycle = LIFECYCLE_QUEUED;
	instance.control = CONTROL_AGENT;
	instance.started_msec = OS::get_singleton()->get_ticks_msec();
	_all().push_back(instance);
	return instance.instance_id;
}

bool MCPRuntimeInstances::bind_pid(const String &p_instance_id, ProcessID p_pid) {
	const int index = _index_of(p_instance_id);
	if (index < 0 || p_pid == 0) {
		return false;
	}
	// One process, one registration. Two registrations pointing at the same pid would
	// make "stop that one" ambiguous, and the wrong game would die.
	const String existing = find_by_pid(p_pid);
	if (!existing.is_empty() && existing != p_instance_id) {
		return false;
	}
	_all().write[index].pid = p_pid;
	return true;
}

bool MCPRuntimeInstances::set_lifecycle(const String &p_instance_id, Lifecycle p_lifecycle,
		const String &p_detail) {
	const int index = _index_of(p_instance_id);
	if (index < 0) {
		return false;
	}
	Instance &instance = _all().write[index];
	instance.lifecycle = p_lifecycle;
	if (!p_detail.is_empty()) {
		instance.detail = p_detail;
	}
	if (p_lifecycle == LIFECYCLE_CLOSED || p_lifecycle == LIFECYCLE_FAILED) {
		instance.ended_msec = OS::get_singleton()->get_ticks_msec();
		// The process is gone; keeping its pid would let a later lookup match a pid the
		// operating system has since handed to something else.
		instance.pid = 0;
	}
	return true;
}

bool MCPRuntimeInstances::set_control(const String &p_instance_id, Control p_control) {
	const int index = _index_of(p_instance_id);
	if (index < 0) {
		return false;
	}
	_all().write[index].control = p_control;
	return true;
}

bool MCPRuntimeInstances::set_group(const String &p_instance_id, const String &p_group) {
	const int index = _index_of(p_instance_id);
	if (index < 0) {
		return false;
	}
	_all().write[index].group = p_group;
	return true;
}

bool MCPRuntimeInstances::get(const String &p_instance_id, Instance &r_instance) {
	const int index = _index_of(p_instance_id);
	if (index < 0) {
		return false;
	}
	r_instance = _all()[index];
	return true;
}

bool MCPRuntimeInstances::exists(const String &p_instance_id) {
	return _index_of(p_instance_id) >= 0;
}

Vector<MCPRuntimeInstances::Instance> MCPRuntimeInstances::list() {
	return _all();
}

Vector<MCPRuntimeInstances::Instance> MCPRuntimeInstances::live() {
	Vector<Instance> out;
	const Vector<Instance> &instances = _all();
	for (int i = 0; i < instances.size(); i++) {
		if (instances[i].pid != 0 && instances[i].lifecycle != LIFECYCLE_CLOSED &&
				instances[i].lifecycle != LIFECYCLE_FAILED) {
			out.push_back(instances[i]);
		}
	}
	return out;
}

String MCPRuntimeInstances::find_by_pid(ProcessID p_pid) {
	if (p_pid == 0) {
		return String();
	}
	const Vector<Instance> &instances = _all();
	for (int i = 0; i < instances.size(); i++) {
		if (instances[i].pid == p_pid) {
			return instances[i].instance_id;
		}
	}
	return String();
}

bool MCPRuntimeInstances::is_agent_owned(ProcessID p_pid) {
	return !find_by_pid(p_pid).is_empty();
}

bool MCPRuntimeInstances::remove(const String &p_instance_id) {
	const int index = _index_of(p_instance_id);
	if (index < 0) {
		return false;
	}
	_all().remove_at(index);
	return true;
}

void MCPRuntimeInstances::clear() {
	_all().clear();
}

Dictionary MCPRuntimeInstances::to_dictionary(const Instance &p_instance) {
	Dictionary out;
	out["instance_id"] = p_instance.instance_id;
	out["label"] = p_instance.label;
	out["role"] = p_instance.role;
	out["task"] = p_instance.task;
	out["group"] = p_instance.group;
	out["pid"] = (int64_t)p_instance.pid;
	out["lifecycle"] = lifecycle_to_string(p_instance.lifecycle);
	out["control"] = control_to_string(p_instance.control);
	out["retention"] = retention_to_string(p_instance.retention);
	out["started_msec"] = p_instance.started_msec;
	out["ended_msec"] = p_instance.ended_msec;
	out["detail"] = p_instance.detail;
	return out;
}

Array MCPRuntimeInstances::to_array() {
	Array out;
	const Vector<Instance> &instances = _all();
	for (int i = 0; i < instances.size(); i++) {
		out.push_back(to_dictionary(instances[i]));
	}
	return out;
}

// -------------------------------------------------------------------- routing ---

ScriptEditorDebugger *MCPRuntimeInstances::debugger_for(const String &p_instance_id) {
	Instance instance;
	if (!get(p_instance_id, instance) || instance.pid == 0) {
		return nullptr;
	}
	EditorDebuggerNode *node = EditorDebuggerNode::get_singleton();
	if (!node) {
		// Headless, or before the editor is up. Callers treat this as "cannot route".
		return nullptr;
	}
	int i = 0;
	while (ScriptEditorDebugger *debugger = node->get_debugger(i)) {
		// The same join GameView uses to find the session for its embedded process.
		if (debugger->is_session_active() && (ProcessID)debugger->get_remote_pid() == instance.pid) {
			return debugger;
		}
		i++;
	}
	return nullptr;
}

bool MCPRuntimeInstances::send_to(const String &p_instance_id, const String &p_message,
		const Array &p_arguments) {
	ScriptEditorDebugger *debugger = debugger_for(p_instance_id);
	if (!debugger) {
		return false;
	}
	debugger->send_message(p_message, p_arguments);
	return true;
}

bool MCPRuntimeInstances::set_suspended(const String &p_instance_id, bool p_suspended) {
	Array arguments;
	arguments.append(p_suspended);
	return send_to(p_instance_id, "scene:suspend_changed", arguments);
}

bool MCPRuntimeInstances::next_frame(const String &p_instance_id) {
	return send_to(p_instance_id, "scene:next_frame", Array());
}

bool MCPRuntimeInstances::set_time_scale(const String &p_instance_id, double p_scale) {
	Array arguments;
	arguments.append(p_scale);
	return send_to(p_instance_id, "scene:speed_changed", arguments);
}

bool MCPRuntimeInstances::set_muted(const String &p_instance_id, bool p_muted) {
	Array arguments;
	arguments.append(p_muted);
	return send_to(p_instance_id, "scene:debug_mute_audio", arguments);
}
