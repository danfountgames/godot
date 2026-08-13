/**************************************************************************/
/*  mcp_profiler_recorder.h                                               */
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

#ifndef MCP_PROFILER_RECORDER_H
#define MCP_PROFILER_RECORDER_H

#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

// Records the running game's full profiler stream - per-function script times,
// server category breakdowns, GPU pass timings, performance monitors, CPU and
// video memory - into a JSON Lines file the client reads from disk, because a
// capture window is far too much data to hand a model inline.
//
// The engine's own debugger plumbing does the collection. Enabling the "servers"
// and "visual" profilers makes the game stream the same per-frame messages the
// editor's Profiler and Visual Profiler panels are built on, and every incoming
// message also fires ScriptEditorDebugger's `debug_data` signal before native
// dispatch - that signal is where this listens. Data crosses the debugger channel
// in the engine's own small per-frame messages, so the transport's 8 MiB message
// cap is never in play; the bulk lives only in the file this class writes.
//
// The window is explicit: start, drive the game with the input tools, stop. A
// capture left running auto-finalizes at its seconds cap, when the game exits,
// or when something else turns the profiler off underneath it - and the result
// of an auto-finalized capture is kept for the next Godot_StopProfiler call, so
// the caller still gets the summary and the file path.
class MCPProfilerRecorder : public RefCounted {
public:
	enum State {
		IDLE,
		RECORDING,
		STOPPING,
	};

private:
	static MCPProfilerRecorder *singleton;

	State state = IDLE;

	// Options as given to start().
	int max_functions = 64;
	bool include_native_calls = false;
	bool include_gpu = true;
	double max_seconds = 120.0;
	String label;

	// Export file.
	Ref<FileAccess> file;
	String user_path; // user://godot_ai_profiles/... when under the user dir, else empty.
	String absolute_path;
	uint64_t start_ticks_ms = 0;
	double started_unix = 0.0;
	bool attached = false; // True when the debugger signal and wire toggles are live.

	// Script function signatures. The game resets its signature ids on every
	// profiler enable, so the file owns identity: ids in the export are assigned
	// per unique name, and the wire map is rebuilt as signature messages arrive.
	HashMap<int64_t, String> wire_sigs;
	HashMap<String, int64_t> local_sig_ids;

	// Frame aggregates.
	int64_t frame_records = 0;
	int64_t first_process_frame = -1;
	int64_t last_process_frame = -1;
	Vector<double> frame_ms_series;
	double frame_ms_sum = 0.0;
	double frame_ms_worst = 0.0;
	int64_t frame_ms_worst_at = -1;
	double script_ms_sum = 0.0;

	struct FuncAgg {
		int64_t calls = 0;
		double self_ms = 0.0;
		double total_ms = 0.0;
	};
	// Fallback fold for the summary, used only when the game dies before it can
	// send the accumulated total (per-frame rows carry only each frame's top
	// functions, so the fold under-counts steadily warm code).
	HashMap<String, FuncAgg> func_fold;

	struct ServerItemAgg {
		double ms_sum = 0.0;
		int64_t samples = 0;
	};
	HashMap<String, ServerItemAgg> server_fold; // "physics_2d/step" -> agg

	// GPU aggregates. Frames arrive tagged with the RenderingServer's own frame
	// number and can repeat while a new profile is still in flight; repeats are
	// dropped rather than double-counted.
	int64_t gpu_records = 0;
	uint64_t last_gpu_frame = 0;
	double gpu_total_sum = 0.0;
	double gpu_total_worst = 0.0;
	struct GpuAgg {
		double gpu_ms_sum = 0.0;
		double cpu_ms_sum = 0.0;
		int64_t samples = 0;
	};
	HashMap<String, GpuAgg> gpu_fold;

	// Monitor aggregates (the ~1 Hz performance stream).
	int64_t mon_records = 0;
	Vector<String> custom_monitor_names;
	double mem_static_first = -1.0;
	double mem_static_last = 0.0;
	double mem_static_peak = 0.0;
	double vram_first = -1.0;
	double vram_last = 0.0;
	double vram_peak = 0.0;
	Dictionary last_monitor_named;

	// Window-edge samples and snapshots.
	Dictionary mem_start;
	Dictionary mem_end;
	bool got_mem_end = false;
	bool vram_start_seen = false;
	Dictionary vram_end_top; // Kept for the summary; full rows go to the file.
	bool got_vram_end = false;

	// Stop bookkeeping.
	bool got_total = false;
	Dictionary total_top_funcs; // Summary-shaped view of the accumulated total.
	double stop_deadline = 0.0;
	String end_reason;
	int64_t event_count = 0;
	int contention_reenables = 0;

	// The result of a capture that finalized without a Godot_StopProfiler call
	// waiting on it, kept until claimed.
	Dictionary unclaimed_result;

	bool signal_connected = false;

	double _now_ms() const;
	void _write_record(const Dictionary &p_record);
	void _write_event(const String &p_what);
	int64_t _local_sig_id(int64_t p_wire_id, String &r_name);
	void _handle_servers_frame(const Array &p_data, bool p_final);
	void _handle_visual_frame(const Array &p_data);
	void _handle_monitor_frame(const Array &p_data);
	void _handle_monitor_names(const Array &p_data);
	void _handle_memory_usage(const Array &p_data);
	void _connect_debugger();
	void _disconnect_debugger();
	bool _game_reachable() const;
	void _send_enable();
	void _send_disable();
	void _request_edge_samples(bool p_end);
	void _on_mem_reply(bool p_ok, const Dictionary &p_payload, bool p_end);
	Dictionary _build_summary() const;
	Dictionary _finalize(const String &p_reason);
	void _reset_capture_state();

public:
	static MCPProfilerRecorder *get_singleton() { return singleton; }

	// Starts a capture against the running game: opens the export file, enables
	// the game's profilers, and requests the start-edge memory and VRAM samples.
	// Fills r_error and returns an empty Dictionary on refusal.
	Dictionary start(const Dictionary &p_options, String &r_error);

	// Starts a capture writing to an explicit absolute path without touching the
	// debugger. This is the unit-test seam: tests feed wire payloads through
	// handle_message() and finalize with finalize_now().
	Dictionary start_detached(const String &p_absolute_path, const Dictionary &p_options, String &r_error);

	// Begins the stop sequence: asks for the end-edge samples, disables the
	// profilers, and moves to STOPPING. poll() finishes the job when the last
	// replies land (or its deadline passes). False when nothing is recording.
	bool begin_stop(const String &p_reason, String &r_error);

	// Non-empty when a finalized result is waiting to be claimed. Claims it.
	Dictionary take_result();

	// Immediate finalization, for tests and editor shutdown.
	Dictionary finalize_now(const String &p_reason);

	// One debugger message, as (message, data). Public because it is both the
	// debug_data signal target and the unit-test seam.
	void handle_message(const String &p_message, const Array &p_data);

	Dictionary status() const;
	State get_state() const { return state; }
	bool has_unclaimed_result() const { return !unclaimed_result.is_empty(); }

	// Called once per service poll: enforces the caps, notices a dead game, and
	// completes a stop whose replies have all arrived.
	void poll();

	// The constant text returned by both Godot_StartProfiler and
	// Godot_StopProfiler that explains how to read the export.
	static String reading_guide();

	MCPProfilerRecorder();
	~MCPProfilerRecorder();
};

#endif // MCP_PROFILER_RECORDER_H
