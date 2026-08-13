/**************************************************************************/
/*  mcp_profiler_recorder.cpp                                             */
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

#include "mcp_profiler_recorder.h"

#include "mcp_paths.h"
#include "mcp_runtime_bridge.h"

#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "main/performance.h"
#include "servers/debugger/servers_debugger.h"

MCPProfilerRecorder *MCPProfilerRecorder::singleton = nullptr;

namespace {

// A capture that outlives its use is a runaway; these are safety stops, not
// targets. The seconds cap is generous because the whole point of the window is
// that the agent drives the game with input tools between start and stop, and
// every one of those calls costs wall clock.
constexpr double MAX_SECONDS_FLOOR = 5.0;
constexpr double MAX_SECONDS_CEILING = 600.0;
constexpr double MAX_SECONDS_DEFAULT = 120.0;
constexpr int64_t HARD_FRAME_CAP = 200000;
constexpr double STOP_GRACE_SECONDS = 8.0;
constexpr int KEPT_EXPORTS = 10;
// The game refuses profiler options outside what the editor's own panel sends.
constexpr int MAX_FUNCTIONS_FLOOR = 16;
constexpr int MAX_FUNCTIONS_CEILING = 512;
constexpr int MAX_FUNCTIONS_DEFAULT = 64;

double now_unix() {
	return Time::get_singleton()->get_unix_time_from_system();
}

} // namespace

double MCPProfilerRecorder::_now_ms() const {
	return double(OS::get_singleton()->get_ticks_msec() - start_ticks_ms);
}

void MCPProfilerRecorder::_write_record(const Dictionary &p_record) {
	if (file.is_null()) {
		return;
	}
	file->store_line(JSON::stringify(p_record));
	// Frames are frequent and buffered; everything else is rare enough to flush,
	// so a capture that ends in a crash still leaves a readable file.
	const String type = p_record.get("type", "");
	if (type != "frame" || (frame_records % 32) == 0) {
		file->flush();
	}
}

void MCPProfilerRecorder::_write_event(const String &p_what) {
	event_count++;
	Dictionary record;
	record["type"] = "event";
	record["t_ms"] = _now_ms();
	record["what"] = p_what;
	_write_record(record);
}

int64_t MCPProfilerRecorder::_local_sig_id(int64_t p_wire_id, String &r_name) {
	String name;
	if (wire_sigs.has(p_wire_id)) {
		name = wire_sigs[p_wire_id];
	} else {
		// A frame referenced a signature whose announcement we never saw - the
		// capture attached mid-stream. The id still identifies it consistently.
		name = vformat("<unannounced signature %d>", p_wire_id);
	}
	r_name = name;
	if (local_sig_ids.has(name)) {
		return local_sig_ids[name];
	}
	const int64_t local = local_sig_ids.size();
	local_sig_ids[name] = local;
	Dictionary record;
	record["type"] = "sig";
	record["id"] = local;
	record["name"] = name;
	_write_record(record);
	return local;
}

void MCPProfilerRecorder::_handle_servers_frame(const Array &p_data, bool p_final) {
	ServersDebugger::ServersProfilerFrame frame;
	if (!frame.deserialize(p_data)) {
		return;
	}

	if (p_final) {
		// The one accumulated frame the game sends when the profiler turns off.
		// Requested by a stop, this is the whole-window truth for function times;
		// unrequested, it means something else - the human's Profiler panel, most
		// likely - turned the profiler off underneath the capture.
		if (state != STOPPING) {
			_write_event("the script profiler was turned off outside this capture (the editor's Profiler panel?); re-enabling");
			contention_reenables++;
			if (contention_reenables > 3) {
				String err;
				begin_stop("profiler_contention", err);
			} else {
				// Re-enabling resets the game's signature ids; the stale wire map
				// goes with it. File-local ids are keyed by name, so they hold.
				wire_sigs.clear();
				_send_enable();
			}
			return;
		}
		Array rows;
		Array summary_rows;
		for (const ServersDebugger::ScriptFunctionInfo &fi : frame.script_functions) {
			String name = wire_sigs.has(fi.sig_id) ? wire_sigs[fi.sig_id] : String(fi.name);
			if (name.is_empty()) {
				name = vformat("<unannounced signature %d>", fi.sig_id);
			}
			Array row;
			row.push_back(name);
			row.push_back(fi.call_count);
			row.push_back(fi.self_time * 1000.0);
			row.push_back(fi.total_time * 1000.0);
			row.push_back(fi.internal_time * 1000.0);
			rows.push_back(row);
			if (summary_rows.size() < 10) {
				Dictionary entry;
				entry["name"] = name;
				entry["calls"] = fi.call_count;
				entry["self_ms"] = fi.self_time * 1000.0;
				entry["total_ms"] = fi.total_time * 1000.0;
				summary_rows.push_back(entry);
			}
		}
		Dictionary record;
		record["type"] = "total";
		record["t_ms"] = _now_ms();
		record["columns"] = String("name, calls, self_ms, total_ms, internal_ms");
		record["funcs"] = rows;
		_write_record(record);
		got_total = true;
		total_top_funcs = Dictionary();
		total_top_funcs["source"] = "accumulated_total";
		total_top_funcs["rows"] = summary_rows;
		return;
	}

	Dictionary record;
	record["type"] = "frame";
	record["t_ms"] = _now_ms();
	record["n"] = (int64_t)frame.frame_number;
	const double frame_ms = frame.frame_time * 1000.0;
	record["frame_ms"] = frame_ms;
	record["process_ms"] = frame.process_time * 1000.0;
	record["physics_ms"] = frame.physics_time * 1000.0;
	record["physics_frame_ms"] = frame.physics_frame_time * 1000.0;
	record["script_ms"] = frame.script_time * 1000.0;

	Dictionary servers;
	for (const ServersDebugger::ServerInfo &srv : frame.servers) {
		Dictionary items;
		for (const ServersDebugger::ServerFunctionInfo &fn : srv.functions) {
			const double ms = fn.time * 1000.0;
			items[String(fn.name)] = ms;
			ServerItemAgg *agg = server_fold.getptr(String(srv.name) + "/" + String(fn.name));
			if (!agg) {
				server_fold.insert(String(srv.name) + "/" + String(fn.name), ServerItemAgg());
				agg = server_fold.getptr(String(srv.name) + "/" + String(fn.name));
			}
			agg->ms_sum += ms;
			agg->samples++;
		}
		servers[String(srv.name)] = items;
	}
	record["servers"] = servers;

	Array funcs;
	for (const ServersDebugger::ScriptFunctionInfo &fi : frame.script_functions) {
		String name;
		const int64_t local = _local_sig_id(fi.sig_id, name);
		Array row;
		row.push_back(local);
		row.push_back(fi.call_count);
		row.push_back(fi.self_time * 1000.0);
		row.push_back(fi.total_time * 1000.0);
		row.push_back(fi.internal_time * 1000.0);
		funcs.push_back(row);

		FuncAgg *agg = func_fold.getptr(name);
		if (!agg) {
			func_fold.insert(name, FuncAgg());
			agg = func_fold.getptr(name);
		}
		agg->calls += fi.call_count;
		agg->self_ms += fi.self_time * 1000.0;
		agg->total_ms += fi.total_time * 1000.0;
	}
	record["funcs"] = funcs;

	frame_records++;
	if (first_process_frame < 0) {
		first_process_frame = frame.frame_number;
	}
	last_process_frame = frame.frame_number;
	frame_ms_series.push_back(frame_ms);
	frame_ms_sum += frame_ms;
	script_ms_sum += frame.script_time * 1000.0;
	if (frame_ms > frame_ms_worst) {
		frame_ms_worst = frame_ms;
		frame_ms_worst_at = frame.frame_number;
	}

	_write_record(record);
}

void MCPProfilerRecorder::_handle_visual_frame(const Array &p_data) {
	ServersDebugger::VisualProfilerFrame frame;
	if (!frame.deserialize(p_data)) {
		return;
	}
	if (frame.areas.is_empty() || frame.frame_number == last_gpu_frame) {
		// GPU timestamps are read back late, so the same completed profile can be
		// reported for several game frames; counting it once keeps means honest.
		return;
	}
	last_gpu_frame = frame.frame_number;

	// Area times are cumulative from the frame's first timestamp; per-area cost is
	// the distance to the next row, and rows named '<'/'>' are the nesting markers
	// the engine's own GPU profile printer skips (rendering_server_default.cpp).
	Array areas;
	const int count = frame.areas.size();
	for (int i = 0; i < count - 1; i++) {
		const String name = frame.areas[i].name;
		if (name.begins_with("<") || name.begins_with(">")) {
			continue;
		}
		const double gpu_delta = MAX(0.0, frame.areas[i + 1].gpu_msec - frame.areas[i].gpu_msec);
		const double cpu_delta = MAX(0.0, frame.areas[i + 1].cpu_msec - frame.areas[i].cpu_msec);
		Array row;
		row.push_back(name);
		row.push_back(cpu_delta);
		row.push_back(gpu_delta);
		areas.push_back(row);

		GpuAgg *agg = gpu_fold.getptr(name);
		if (!agg) {
			gpu_fold.insert(name, GpuAgg());
			agg = gpu_fold.getptr(name);
		}
		agg->gpu_ms_sum += gpu_delta;
		agg->cpu_ms_sum += cpu_delta;
		agg->samples++;
	}
	const double total_gpu = count > 0 ? frame.areas[count - 1].gpu_msec : 0.0;
	const double total_cpu = count > 0 ? frame.areas[count - 1].cpu_msec : 0.0;

	Dictionary record;
	record["type"] = "gpu";
	record["t_ms"] = _now_ms();
	record["rs_frame"] = (int64_t)frame.frame_number;
	record["total_gpu_ms"] = total_gpu;
	record["total_cpu_ms"] = total_cpu;
	record["columns"] = String("name, cpu_ms, gpu_ms");
	record["areas"] = areas;
	_write_record(record);

	gpu_records++;
	gpu_total_sum += total_gpu;
	gpu_total_worst = MAX(gpu_total_worst, total_gpu);
}

void MCPProfilerRecorder::_handle_monitor_frame(const Array &p_data) {
	Dictionary record;
	record["type"] = "mon";
	record["t_ms"] = _now_ms();

	// Headline fields are converted - times to milliseconds, memory in bytes -
	// while `raw` keeps every monitor untouched in its native unit.
	struct NamedMonitor {
		const char *key;
		Performance::Monitor monitor;
		double scale;
	};
	static const NamedMonitor named[] = {
		{ "fps", Performance::TIME_FPS, 1.0 },
		{ "process_ms", Performance::TIME_PROCESS, 1000.0 },
		{ "physics_ms", Performance::TIME_PHYSICS_PROCESS, 1000.0 },
		{ "static_mem", Performance::MEMORY_STATIC, 1.0 },
		{ "static_mem_max", Performance::MEMORY_STATIC_MAX, 1.0 },
		{ "video_mem", Performance::RENDER_VIDEO_MEM_USED, 1.0 },
		{ "texture_mem", Performance::RENDER_TEXTURE_MEM_USED, 1.0 },
		{ "buffer_mem", Performance::RENDER_BUFFER_MEM_USED, 1.0 },
		{ "objects", Performance::OBJECT_COUNT, 1.0 },
		{ "resources", Performance::OBJECT_RESOURCE_COUNT, 1.0 },
		{ "nodes", Performance::OBJECT_NODE_COUNT, 1.0 },
		{ "orphan_nodes", Performance::OBJECT_ORPHAN_NODE_COUNT, 1.0 },
		{ "draw_calls", Performance::RENDER_TOTAL_DRAW_CALLS_IN_FRAME, 1.0 },
	};
	Dictionary named_values;
	for (const NamedMonitor &m : named) {
		if ((int)m.monitor < p_data.size()) {
			const double value = double(p_data[(int)m.monitor]) * m.scale;
			record[m.key] = value;
			named_values[m.key] = value;
		}
	}
	record["raw"] = p_data.duplicate();
	_write_record(record);

	mon_records++;
	last_monitor_named = named_values;
	if ((int)Performance::MEMORY_STATIC < p_data.size()) {
		const double static_mem = p_data[(int)Performance::MEMORY_STATIC];
		if (mem_static_first < 0) {
			mem_static_first = static_mem;
		}
		mem_static_last = static_mem;
		mem_static_peak = MAX(mem_static_peak, static_mem);
	}
	if ((int)Performance::RENDER_VIDEO_MEM_USED < p_data.size()) {
		const double vram = p_data[(int)Performance::RENDER_VIDEO_MEM_USED];
		if (vram_first < 0) {
			vram_first = vram;
		}
		vram_last = vram;
		vram_peak = MAX(vram_peak, vram);
	}
}

void MCPProfilerRecorder::_handle_monitor_names(const Array &p_data) {
	if (p_data.size() != 2) {
		return;
	}
	const Array names = p_data[0];
	custom_monitor_names.clear();
	Array out;
	for (int i = 0; i < names.size(); i++) {
		custom_monitor_names.push_back(String(names[i]));
		out.push_back(String(names[i]));
	}
	Dictionary record;
	record["type"] = "mon_names";
	record["t_ms"] = _now_ms();
	record["note"] = "custom monitors appended to mon.raw after the built-in list";
	record["names"] = out;
	_write_record(record);
}

void MCPProfilerRecorder::_handle_memory_usage(const Array &p_data) {
	ServersDebugger::ResourceUsage usage;
	if (!usage.deserialize(p_data)) {
		return;
	}
	Vector<ServersDebugger::ResourceInfo> sorted;
	double total = 0.0;
	for (const ServersDebugger::ResourceInfo &info : usage.infos) {
		sorted.push_back(info);
		total += info.vram;
	}
	sorted.sort(); // ResourceInfo orders by VRAM, largest first.

	Array rows;
	for (const ServersDebugger::ResourceInfo &info : sorted) {
		Array row;
		row.push_back(info.path);
		row.push_back(info.type);
		row.push_back(info.format);
		row.push_back(info.vram);
		rows.push_back(row);
	}

	const String phase = state == STOPPING ? "end" : (vram_start_seen ? "mid" : "start");
	Dictionary record;
	record["type"] = "vram";
	record["t_ms"] = _now_ms();
	record["phase"] = phase;
	record["total_bytes"] = total;
	record["count"] = rows.size();
	record["columns"] = String("path, type, format, bytes");
	record["resources"] = rows;
	_write_record(record);

	if (state == STOPPING) {
		got_vram_end = true;
		Array top;
		for (int i = 0; i < sorted.size() && i < 8; i++) {
			Dictionary entry;
			entry["path"] = sorted[i].path;
			entry["type"] = sorted[i].type;
			entry["bytes"] = sorted[i].vram;
			top.push_back(entry);
		}
		vram_end_top = Dictionary();
		vram_end_top["snapshot_total_bytes"] = total;
		vram_end_top["top_resources"] = top;
	} else {
		vram_start_seen = true;
	}
}

void MCPProfilerRecorder::handle_message(const String &p_message, const Array &p_data) {
	if (state == IDLE || file.is_null()) {
		return;
	}
	if (p_message == "servers:profile_frame") {
		_handle_servers_frame(p_data, false);
	} else if (p_message == "servers:profile_total") {
		_handle_servers_frame(p_data, true);
	} else if (p_message == "servers:function_signature") {
		ServersDebugger::ScriptFunctionSignature sig;
		if (sig.deserialize(p_data)) {
			wire_sigs[sig.id] = String(sig.name);
		}
	} else if (p_message == "visual:profile_frame") {
		_handle_visual_frame(p_data);
	} else if (p_message == "performance:profile_frame") {
		_handle_monitor_frame(p_data);
	} else if (p_message == "performance:profile_names") {
		_handle_monitor_names(p_data);
	} else if (p_message == "servers:memory_usage") {
		_handle_memory_usage(p_data);
	}
}

void MCPProfilerRecorder::_connect_debugger() {
	if (signal_connected || !attached) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger || !debugger->get_default_debugger()) {
		return;
	}
	// Every message from the game fires this before native dispatch, profiler
	// frames included - they never reach EditorDebuggerPlugin::capture(), because
	// the editor's own handlers claim them first.
	debugger->get_default_debugger()->connect("debug_data", callable_mp(this, &MCPProfilerRecorder::handle_message));
	signal_connected = true;
}

void MCPProfilerRecorder::_disconnect_debugger() {
	if (!signal_connected) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (debugger && debugger->get_default_debugger()) {
		debugger->get_default_debugger()->disconnect("debug_data", callable_mp(this, &MCPProfilerRecorder::handle_message));
	}
	signal_connected = false;
}

bool MCPProfilerRecorder::_game_reachable() const {
	if (!attached) {
		return false;
	}
	MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
	return bridge && bridge->is_game_reachable();
}

void MCPProfilerRecorder::_send_enable() {
	if (!attached) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger || !debugger->get_default_debugger()) {
		return;
	}
	Array opts;
	opts.push_back(max_functions);
	opts.push_back(include_native_calls);
	debugger->get_default_debugger()->toggle_profiler("servers", true, opts);
	if (include_gpu) {
		debugger->get_default_debugger()->toggle_profiler("visual", true, Array());
	}
}

void MCPProfilerRecorder::_send_disable() {
	if (!attached) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger || !debugger->get_default_debugger()) {
		return;
	}
	if (include_gpu) {
		debugger->get_default_debugger()->toggle_profiler("visual", false, Array());
	}
	// Disabling "servers" makes the game send the accumulated profile_total.
	debugger->get_default_debugger()->toggle_profiler("servers", false, Array());
}

void MCPProfilerRecorder::_request_edge_samples(bool p_end) {
	if (!attached) {
		return;
	}
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (debugger && debugger->get_default_debugger()) {
		// Answered by servers:memory_usage - the per-resource VRAM inventory.
		debugger->get_default_debugger()->send_message("servers:memory", Array());
	}
	// Monitors arrive once a second, which quantizes the window edges; one bridge
	// round-trip pins the exact start and end memory numbers.
	MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
	if (!bridge || !bridge->request("performance", Dictionary(), 5.0,
					   callable_mp(this, &MCPProfilerRecorder::_on_mem_reply).bind(p_end))) {
		if (p_end) {
			got_mem_end = true;
		}
	}
}

void MCPProfilerRecorder::_on_mem_reply(bool p_ok, const Dictionary &p_payload, bool p_end) {
	if (state == IDLE) {
		return;
	}
	Dictionary record;
	record["type"] = "mem";
	record["t_ms"] = _now_ms();
	record["phase"] = p_end ? "end" : "start";
	if (p_ok) {
		for (const KeyValue<Variant, Variant> &kv : p_payload) {
			if (String(kv.key) != "note") {
				record[kv.key] = kv.value;
			}
		}
	} else {
		record["unavailable"] = String(p_payload.get("message", "the game did not answer"));
	}
	_write_record(record);
	if (p_end) {
		mem_end = record;
		got_mem_end = true;
	} else {
		mem_start = record;
	}
}

Dictionary MCPProfilerRecorder::start(const Dictionary &p_options, String &r_error) {
	return start_detached(String(), p_options, r_error);
}

Dictionary MCPProfilerRecorder::start_detached(const String &p_absolute_path, const Dictionary &p_options, String &r_error) {
	if (state != IDLE) {
		r_error = state == RECORDING
				? "a profiler capture is already recording; Godot_StopProfiler finishes it"
				: "the previous capture is still finalizing; try again in a moment";
		return Dictionary();
	}
	const bool wants_attach = p_absolute_path.is_empty();
	if (wants_attach) {
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error = "no game is running; start one with Godot_PlayCurrentScene or Godot_PlayMainScene first";
			return Dictionary();
		}
	}
	attached = wants_attach;

	const String discarded_previous = unclaimed_result.get("export_path", "");
	unclaimed_result = Dictionary();
	_reset_capture_state();

	max_functions = CLAMP(int(p_options.get("max_functions", MAX_FUNCTIONS_DEFAULT)), MAX_FUNCTIONS_FLOOR, MAX_FUNCTIONS_CEILING);
	include_native_calls = bool(p_options.get("include_native_calls", false));
	include_gpu = bool(p_options.get("include_gpu", true));
	max_seconds = CLAMP(double(p_options.get("max_seconds", MAX_SECONDS_DEFAULT)), MAX_SECONDS_FLOOR, MAX_SECONDS_CEILING);
	label = String(p_options.get("label", ""));

	start_ticks_ms = OS::get_singleton()->get_ticks_msec();
	started_unix = now_unix();

	if (p_absolute_path.is_empty()) {
		const String user_root = MCPPaths::get_user_root();
		if (user_root.is_empty()) {
			r_error = "this process has no user:// directory to export into";
			return Dictionary();
		}
		const String dir = user_root.path_join("godot_ai_profiles");
		if (DirAccess::make_dir_recursive_absolute(dir) != OK) {
			r_error = vformat("could not create '%s'", dir);
			return Dictionary();
		}
		// Old exports are pruned by age so captures cannot grow the directory
		// without bound; the newest few stay readable for follow-up questions.
		{
			Ref<DirAccess> da = DirAccess::open(dir);
			if (da.is_valid()) {
				struct Entry {
					String name;
					uint64_t mtime = 0;
					bool operator<(const Entry &p_other) const { return mtime > p_other.mtime; }
				};
				Vector<Entry> entries;
				da->list_dir_begin();
				for (String f = da->get_next(); !f.is_empty(); f = da->get_next()) {
					if (!da->current_is_dir() && f.begins_with("profile-") && f.ends_with(".jsonl")) {
						Entry entry;
						entry.name = f;
						entry.mtime = FileAccess::get_modified_time(dir.path_join(f));
						entries.push_back(entry);
					}
				}
				da->list_dir_end();
				entries.sort();
				for (int i = KEPT_EXPORTS - 1; i < entries.size(); i++) {
					da->remove(entries[i].name);
				}
			}
		}
		const String fname = vformat("profile-%d-%d.jsonl", (int64_t)started_unix,
				(int64_t)(OS::get_singleton()->get_ticks_usec() % 1000000));
		absolute_path = dir.path_join(fname);
		user_path = String("user://godot_ai_profiles/") + fname;
	} else {
		absolute_path = p_absolute_path;
		user_path = String();
	}

	Error open_error = OK;
	file = FileAccess::open(absolute_path, FileAccess::WRITE, &open_error);
	if (file.is_null()) {
		r_error = vformat("could not open '%s' for writing (%d)", absolute_path, (int)open_error);
		return Dictionary();
	}

	Dictionary options_echo;
	options_echo["max_functions"] = max_functions;
	options_echo["include_native_calls"] = include_native_calls;
	options_echo["include_gpu"] = include_gpu;
	options_echo["max_seconds"] = max_seconds;
	if (!label.is_empty()) {
		options_echo["label"] = label;
	}

	Array builtin_monitors;
	if (Performance::get_singleton()) {
		for (int i = 0; i < Performance::MONITOR_MAX; i++) {
			builtin_monitors.push_back(Performance::get_singleton()->get_monitor_name(Performance::Monitor(i)));
		}
	}

	Dictionary header;
	header["type"] = "header";
	header["format"] = 1;
	header["generator"] = "godot_ai profiler capture";
	header["started_unix"] = started_unix;
	header["options"] = options_echo;
	header["units"] = "every *_ms field is milliseconds and every *mem*/bytes field is bytes; mon.raw is untouched monitor values in native units, where time monitors are seconds";
	header["frame_funcs_columns"] = String("sig_id, calls, self_ms, total_ms, internal_ms");
	header["builtin_monitors"] = builtin_monitors;
	_write_record(header);

	state = RECORDING;
	_connect_debugger();
	_send_enable();
	_request_edge_samples(false);

	Dictionary result;
	result["recording"] = true;
	if (!user_path.is_empty()) {
		result["export_path"] = user_path;
	}
	result["export_absolute_path"] = absolute_path;
	result["started_unix"] = started_unix;
	result["options"] = options_echo;
	result["note"] = vformat(
			"recording; drive the game with the input tools, then Godot_StopProfiler "
			"writes the summary and finishes the export. Left alone, the capture stops "
			"itself after %.0f seconds and keeps the result for the next stop call.",
			max_seconds);
	if (!discarded_previous.is_empty()) {
		result["previous_export_path"] = discarded_previous;
	}
	result["reading_guide"] = reading_guide();
	return result;
}

bool MCPProfilerRecorder::begin_stop(const String &p_reason, String &r_error) {
	if (state == STOPPING) {
		r_error = "the capture is already finalizing";
		return false;
	}
	if (state != RECORDING) {
		r_error = "nothing is recording; Godot_StartProfiler begins a capture";
		return false;
	}
	end_reason = p_reason;
	stop_deadline = double(OS::get_singleton()->get_ticks_msec()) / 1000.0 + STOP_GRACE_SECONDS;
	state = STOPPING;
	if (_game_reachable()) {
		_request_edge_samples(true);
		_send_disable();
	}
	// With no reachable game nothing will answer; poll() notices and finalizes
	// with what was collected, honestly marked partial.
	return true;
}

Dictionary MCPProfilerRecorder::take_result() {
	const Dictionary result = unclaimed_result;
	unclaimed_result = Dictionary();
	return result;
}

Dictionary MCPProfilerRecorder::finalize_now(const String &p_reason) {
	if (state == IDLE) {
		return take_result();
	}
	end_reason = p_reason;
	return _finalize(p_reason);
}

Dictionary MCPProfilerRecorder::_build_summary() const {
	Dictionary summary;

	Dictionary window;
	window["seconds"] = _now_ms() / 1000.0;
	window["frames"] = frame_records;
	if (first_process_frame >= 0) {
		window["process_frame_from"] = first_process_frame;
		window["process_frame_to"] = last_process_frame;
	}
	summary["window"] = window;

	Dictionary frame_stats;
	if (frame_records > 0) {
		const double mean = frame_ms_sum / frame_records;
		frame_stats["mean_ms"] = mean;
		Vector<double> sorted = frame_ms_series;
		sorted.sort();
		frame_stats["p95_ms"] = sorted[int((sorted.size() - 1) * 0.95)];
		frame_stats["worst_ms"] = frame_ms_worst;
		frame_stats["worst_at_process_frame"] = frame_ms_worst_at;
		frame_stats["fps_mean_estimate"] = mean > 0.0 ? 1000.0 / mean : 0.0;
		frame_stats["mean_script_ms"] = script_ms_sum / frame_records;
	} else {
		frame_stats["note"] = "no profiler frames arrived; the game may have been paused, stopped, or never enabled";
	}
	summary["frame_ms"] = frame_stats;

	Dictionary script;
	if (got_total) {
		script = total_top_funcs.duplicate();
	} else {
		// Folding per-frame rows under-counts steadily warm functions (each frame
		// carries only that frame's top rows), so this is labelled as the fallback
		// it is rather than passed off as the whole-window truth.
		script["source"] = "per_frame_fold";
		script["note"] = "the accumulated total never arrived (the game exited mid-capture?); these totals fold each frame's top rows and under-count steadily warm code";
		struct Ranked {
			String name;
			FuncAgg agg;
			bool operator<(const Ranked &p_other) const { return agg.self_ms > p_other.agg.self_ms; }
		};
		Vector<Ranked> ranked;
		for (const KeyValue<String, FuncAgg> &kv : func_fold) {
			Ranked entry;
			entry.name = kv.key;
			entry.agg = kv.value;
			ranked.push_back(entry);
		}
		ranked.sort();
		Array rows;
		for (int i = 0; i < ranked.size() && i < 10; i++) {
			Dictionary entry;
			entry["name"] = ranked[i].name;
			entry["calls"] = ranked[i].agg.calls;
			entry["self_ms"] = ranked[i].agg.self_ms;
			entry["total_ms"] = ranked[i].agg.total_ms;
			rows.push_back(entry);
		}
		script["rows"] = rows;
	}
	summary["top_functions"] = script;

	{
		struct Ranked {
			String name;
			double mean_ms = 0.0;
			bool operator<(const Ranked &p_other) const { return mean_ms > p_other.mean_ms; }
		};
		Vector<Ranked> ranked;
		for (const KeyValue<String, ServerItemAgg> &kv : server_fold) {
			Ranked entry;
			entry.name = kv.key;
			entry.mean_ms = frame_records > 0 ? kv.value.ms_sum / frame_records : 0.0;
			ranked.push_back(entry);
		}
		ranked.sort();
		Array rows;
		for (int i = 0; i < ranked.size() && i < 8; i++) {
			Dictionary entry;
			entry["category"] = ranked[i].name;
			entry["mean_ms"] = ranked[i].mean_ms;
			rows.push_back(entry);
		}
		summary["servers_top_mean_ms"] = rows;
	}

	Dictionary gpu;
	if (gpu_records > 0) {
		gpu["frames"] = gpu_records;
		gpu["mean_total_ms"] = gpu_total_sum / gpu_records;
		gpu["worst_total_ms"] = gpu_total_worst;
		struct Ranked {
			String name;
			double gpu_mean = 0.0;
			double cpu_mean = 0.0;
			bool operator<(const Ranked &p_other) const { return gpu_mean > p_other.gpu_mean; }
		};
		Vector<Ranked> ranked;
		for (const KeyValue<String, GpuAgg> &kv : gpu_fold) {
			if (kv.value.samples == 0) {
				continue;
			}
			Ranked entry;
			entry.name = kv.key;
			entry.gpu_mean = kv.value.gpu_ms_sum / kv.value.samples;
			entry.cpu_mean = kv.value.cpu_ms_sum / kv.value.samples;
			ranked.push_back(entry);
		}
		ranked.sort();
		Array rows;
		for (int i = 0; i < ranked.size() && i < 8; i++) {
			Dictionary entry;
			entry["area"] = ranked[i].name;
			entry["mean_gpu_ms"] = ranked[i].gpu_mean;
			entry["mean_cpu_ms"] = ranked[i].cpu_mean;
			rows.push_back(entry);
		}
		gpu["top_areas"] = rows;
	} else {
		gpu["note"] = include_gpu
				? "no GPU pass timings arrived; headless runs and software or Compatibility renderers do not produce them"
				: "GPU timing was not requested (include_gpu was false)";
	}
	summary["gpu"] = gpu;

	Dictionary memory;
	if (mem_static_first >= 0) {
		memory["static_start_bytes"] = mem_static_first;
		memory["static_end_bytes"] = mem_static_last;
		memory["static_delta_bytes"] = mem_static_last - mem_static_first;
		memory["static_peak_bytes"] = mem_static_peak;
	}
	if (!mem_start.is_empty()) {
		memory["precise_start"] = mem_start;
	}
	if (!mem_end.is_empty()) {
		memory["precise_end"] = mem_end;
	}
	if (memory.is_empty()) {
		memory["note"] = "no memory samples arrived";
	}
	summary["cpu_memory"] = memory;

	Dictionary vram;
	if (vram_first >= 0) {
		vram["video_mem_start_bytes"] = vram_first;
		vram["video_mem_end_bytes"] = vram_last;
		vram["video_mem_delta_bytes"] = vram_last - vram_first;
		vram["video_mem_peak_bytes"] = vram_peak;
	}
	if (!vram_end_top.is_empty()) {
		vram["snapshot_total_bytes"] = vram_end_top.get("snapshot_total_bytes", 0);
		vram["top_resources"] = vram_end_top.get("top_resources", Array());
	}
	if (vram.is_empty()) {
		vram["note"] = "no video memory data arrived; headless runs report none";
	}
	summary["gpu_memory"] = vram;

	Dictionary monitors;
	monitors["samples"] = mon_records;
	if (!last_monitor_named.is_empty()) {
		monitors["last"] = last_monitor_named;
	}
	summary["monitors"] = monitors;

	summary["events"] = event_count;
	return summary;
}

Dictionary MCPProfilerRecorder::_finalize(const String &p_reason) {
	const Dictionary summary = _build_summary();
	int64_t export_bytes = 0;
	if (file.is_valid()) {
		Dictionary record = summary.duplicate();
		record["type"] = "summary";
		record["t_ms"] = _now_ms();
		record["end_reason"] = p_reason;
		_write_record(record);
		file->flush();
		export_bytes = (int64_t)file->get_position();
		file.unref();
	}

	Dictionary result;
	if (!user_path.is_empty()) {
		result["export_path"] = user_path;
	}
	result["export_absolute_path"] = absolute_path;
	result["export_bytes"] = export_bytes;
	result["end_reason"] = p_reason;
	result["partial"] = !got_total;
	result["summary"] = summary;
	result["reading_guide"] = reading_guide();
	if (p_reason != "stopped") {
		result["note"] = vformat(
				"the capture finalized itself ('%s') rather than by Godot_StopProfiler; "
				"the window may be shorter than intended, so check summary.window before "
				"judging a budget",
				p_reason);
	}

	state = IDLE;
	unclaimed_result = result;
	return result;
}

void MCPProfilerRecorder::_reset_capture_state() {
	wire_sigs.clear();
	local_sig_ids.clear();
	frame_records = 0;
	first_process_frame = -1;
	last_process_frame = -1;
	frame_ms_series.clear();
	frame_ms_sum = 0.0;
	frame_ms_worst = 0.0;
	frame_ms_worst_at = -1;
	script_ms_sum = 0.0;
	func_fold.clear();
	server_fold.clear();
	gpu_records = 0;
	last_gpu_frame = 0;
	gpu_total_sum = 0.0;
	gpu_total_worst = 0.0;
	gpu_fold.clear();
	mon_records = 0;
	custom_monitor_names.clear();
	mem_static_first = -1.0;
	mem_static_last = 0.0;
	mem_static_peak = 0.0;
	vram_first = -1.0;
	vram_last = 0.0;
	vram_peak = 0.0;
	last_monitor_named = Dictionary();
	mem_start = Dictionary();
	mem_end = Dictionary();
	got_mem_end = false;
	vram_start_seen = false;
	vram_end_top = Dictionary();
	got_vram_end = false;
	got_total = false;
	total_top_funcs = Dictionary();
	stop_deadline = 0.0;
	end_reason = String();
	event_count = 0;
	contention_reenables = 0;
}

Dictionary MCPProfilerRecorder::status() const {
	Dictionary result;
	switch (state) {
		case IDLE:
			result["state"] = "idle";
			break;
		case RECORDING:
			result["state"] = "recording";
			break;
		case STOPPING:
			result["state"] = "stopping";
			break;
	}
	if (state != IDLE) {
		result["elapsed_seconds"] = _now_ms() / 1000.0;
		result["frames"] = frame_records;
		result["gpu_frames"] = gpu_records;
		result["monitor_samples"] = mon_records;
		result["events"] = event_count;
		if (file.is_valid()) {
			result["export_bytes_so_far"] = (int64_t)file->get_position();
		}
		if (!user_path.is_empty()) {
			result["export_path"] = user_path;
		}
		result["export_absolute_path"] = absolute_path;
		result["auto_stop_seconds"] = max_seconds;
	}
	if (!unclaimed_result.is_empty()) {
		result["finished_capture_waiting"] = true;
		result["note"] = "a finalized capture is waiting; Godot_StopProfiler returns it";
	}
	return result;
}

void MCPProfilerRecorder::poll() {
	if (state == RECORDING) {
		if (attached && !_game_reachable()) {
			_finalize("game_stopped");
			return;
		}
		String err;
		if (_now_ms() / 1000.0 > max_seconds) {
			begin_stop("max_seconds", err);
		} else if (frame_records >= HARD_FRAME_CAP) {
			begin_stop("max_frames", err);
		}
		return;
	}
	if (state == STOPPING) {
		const bool everything_arrived = got_total && got_mem_end && got_vram_end;
		const bool deadline_passed = double(OS::get_singleton()->get_ticks_msec()) / 1000.0 > stop_deadline;
		if (everything_arrived || deadline_passed || (attached && !_game_reachable())) {
			_finalize(end_reason.is_empty() ? "stopped" : end_reason);
		}
	}
}

String MCPProfilerRecorder::reading_guide() {
	return String(
			"HOW TO READ THE EXPORT. The file is JSON Lines: one JSON object per line, "
			"discriminated by its `type` field. Every *_ms field is milliseconds; memory "
			"is bytes. Types: `header` (options, units, the built-in monitor list that "
			"names mon.raw's indices) - `sig` (script function id -> name; frame.funcs "
			"rows use these ids) - `frame` (one per game frame: t_ms editor-receipt time "
			"since capture start, n the game's process frame number, frame_ms/process_ms/"
			"physics_ms/script_ms, servers category timings, funcs rows [sig_id, calls, "
			"self_ms, total_ms, internal_ms]) - `gpu` (one per rendered frame when GPU "
			"timing exists: total_gpu_ms and per-pass areas [name, cpu_ms, gpu_ms]; "
			"absent under headless or software rendering) - `mon` (performance monitors "
			"about once a second: converted headline fields plus `raw`, native units, "
			"where time monitors are seconds) - `mon_names` (custom monitors appended "
			"after the built-ins in raw) - `mem` (precise CPU memory samples at the "
			"window edges, phase start/end) - `vram` (per-resource video memory "
			"snapshots, rows [path, type, format, bytes], phase start/end) - `event` "
			"(anything unusual mid-capture) - `total` (per-function times accumulated "
			"over the whole window; prefer this to folding frames) - `summary` (the "
			"same aggregates returned inline; a file with NO summary line ended "
			"abruptly and is a partial capture). "
			"RECIPES (jq): "
			"worst 10 frames: jq -s '[.[]|select(.type==\"frame\")]|sort_by(-.frame_ms)[:10]|map({n,t_ms,frame_ms,script_ms})' FILE ; "
			"whole-window top functions: jq 'select(.type==\"total\").funcs[:15]' FILE ; "
			"resolve a sig id: jq 'select(.type==\"sig\" and .id==7)' FILE ; "
			"memory over time: jq -c 'select(.type==\"mon\")|{t_ms,static_mem,video_mem}' FILE ; "
			"worst GPU frame's passes: jq -s '[.[]|select(.type==\"gpu\")]|sort_by(-.total_gpu_ms)[0]' FILE ; "
			"biggest VRAM users: jq 'select(.type==\"vram\" and .phase==\"end\").resources[:15]' FILE . "
			"A client on this machine reads export_absolute_path directly; one without "
			"local file access pages export_path through Godot_ReadUserFile.");
}

MCPProfilerRecorder::MCPProfilerRecorder() {
	singleton = this;
}

MCPProfilerRecorder::~MCPProfilerRecorder() {
	if (state != IDLE) {
		// The editor is going away mid-capture; leave a finished file behind.
		_finalize("editor_shutdown");
	}
	_disconnect_debugger();
	if (singleton == this) {
		singleton = nullptr;
	}
}
