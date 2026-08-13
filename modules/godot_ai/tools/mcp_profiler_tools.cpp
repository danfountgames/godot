/**************************************************************************/
/*  mcp_profiler_tools.cpp                                                */
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

#include "mcp_builtin_tools.h"

#include "../mcp_deferred.h"
#include "../mcp_profiler_recorder.h"
#include "../mcp_tool_registry.h"

#include "core/object/callable_mp.h"

// The full profiler as a windowed capture: start, drive the game, stop. The bulk
// data goes to a JSON Lines file both processes can reach, because a profiling
// window is megabytes - far more than a tool result should carry inline - and the
// stop reply carries a decision-grade summary plus the guide for reading the rest.

namespace {

class StartProfilerTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_StartProfiler"; }
	virtual String get_description() const override {
		return "Start recording the running game's full profiler: per-function script times, "
			   "server category breakdowns (physics, audio, navigation), GPU pass timings, "
			   "performance monitors, and CPU and video memory including a per-resource VRAM "
			   "inventory. Everything streams into a JSON Lines export file - it is far too "
			   "much to return inline - and the reply names the file and explains how to read "
			   "it. Drive the game with the input tools while the capture runs, then "
			   "Godot_StopProfiler writes the summary. A capture left running stops itself at "
			   "max_seconds and keeps its result for the next stop call. Godot_ProfileWindow "
			   "remains the quick answer when frame times alone will do.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["max_seconds"] = MCPSchema::number_property(
				"Safety stop, 5 to 600. The default is deliberately roomy: every input-tool "
				"call between start and stop costs wall clock.",
				120);
		properties["max_functions"] = MCPSchema::integer_property(
				"Script functions reported per frame and in the whole-window total, 16 to 512. "
				"Raise it when a wide codebase makes the top table look truncated.",
				64);
		properties["include_native_calls"] = MCPSchema::bool_property(
				"Also profile calls into engine built-ins, at extra overhead.", false);
		properties["include_gpu"] = MCPSchema::bool_property(
				"Collect GPU pass timings. Headless runs and software renderers produce none "
				"either way.",
				true);
		properties["label"] = MCPSchema::string_property(
				"Free-form tag written into the export header, to tell captures apart.", "");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["recording"] = MCPSchema::bool_property("True once the capture is live.");
		properties["export_path"] = MCPSchema::string_property(
				"The export file as a user:// path, readable through Godot_ReadUserFile.");
		properties["export_absolute_path"] = MCPSchema::string_property(
				"The same file as an absolute path, for a client on this machine.");
		properties["reading_guide"] = MCPSchema::string_property(
				"How to read the export: record types, units, and jq recipes.");
		properties["note"] = MCPSchema::string_property("What happens next.");
		return MCPSchema::object_schema(properties, Vector<String>(), true);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPProfilerRecorder *recorder = MCPProfilerRecorder::get_singleton();
		if (!recorder) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		String error;
		Dictionary result = recorder->start(p_arguments, error);
		if (result.is_empty()) {
			r_error.set(MCPToolError::INVALID_STATE, error);
			return Dictionary();
		}
		return result;
	}
};

class StopProfilerTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_StopProfiler"; }
	virtual String get_description() const override {
		return "Stop the profiler capture Godot_StartProfiler began and finish its export: the "
			   "game sends its whole-window per-function totals and a closing VRAM inventory, "
			   "the summary line is written, and the reply carries that summary - worst and "
			   "p95 frame, top functions by self time, server and GPU means, memory deltas - "
			   "with the export path and the reading guide. Also returns the kept result of a "
			   "capture that already finalized itself (game exited, max_seconds passed).";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override {
		return MCPSchema::object_schema(Dictionary());
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["export_path"] = MCPSchema::string_property(
				"The export file as a user:// path, readable through Godot_ReadUserFile.");
		properties["export_absolute_path"] = MCPSchema::string_property(
				"The same file as an absolute path, for a client on this machine.");
		properties["export_bytes"] = MCPSchema::integer_property("Size of the export file.");
		properties["end_reason"] = MCPSchema::string_property(
				"Why the capture ended: stopped, max_seconds, max_frames, game_stopped, "
				"profiler_contention, or editor_shutdown.");
		properties["partial"] = MCPSchema::bool_property(
				"True when the whole-window function totals never arrived; the summary then "
				"folds per-frame rows and under-counts steadily warm code.");
		properties["summary"] = MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
		properties["reading_guide"] = MCPSchema::string_property(
				"How to read the export: record types, units, and jq recipes.");
		return MCPSchema::object_schema(properties, Vector<String>(), true);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPProfilerRecorder *recorder = MCPProfilerRecorder::get_singleton();
		if (!recorder) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		if (recorder->get_state() == MCPProfilerRecorder::IDLE) {
			if (recorder->has_unclaimed_result()) {
				return recorder->take_result();
			}
			r_error.set(MCPToolError::INVALID_STATE,
					"nothing is recording; Godot_StartProfiler begins a capture");
			return Dictionary();
		}
		String error;
		if (recorder->get_state() == MCPProfilerRecorder::RECORDING && !recorder->begin_stop("stopped", error)) {
			r_error.set(MCPToolError::INVALID_STATE, error);
			return Dictionary();
		}
		// The game still owes the accumulated totals, the closing VRAM inventory
		// and the end memory sample; the recorder finalizes when they land (or its
		// own grace deadline passes), which is well inside this window.
		return MCPDeferred::make_deferred_result(
				MCPDeferred::begin_polled(20.0, callable_mp(this, &StopProfilerTool::_poll)));
	}

private:
	Variant _poll() {
		MCPProfilerRecorder *recorder = MCPProfilerRecorder::get_singleton();
		if (!recorder) {
			return Variant();
		}
		const Dictionary result = recorder->take_result();
		if (result.is_empty()) {
			return Variant();
		}
		return result;
	}
};

class GetProfilerStatusTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetProfilerStatus"; }
	virtual String get_description() const override {
		return "Report the profiler capture's state without disturbing it: recording or not, "
			   "elapsed time, frames and monitor samples collected so far, export file size, "
			   "and whether a finished capture is waiting to be claimed. The mid-window sanity "
			   "check between driving inputs - confirm data is flowing before investing a long "
			   "gameplay sequence in the window.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override {
		return MCPSchema::object_schema(Dictionary());
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["state"] = MCPSchema::string_property("idle, recording, or stopping.");
		properties["elapsed_seconds"] = MCPSchema::number_property("Time since the capture started.");
		properties["frames"] = MCPSchema::integer_property("Profiler frames collected so far.");
		properties["gpu_frames"] = MCPSchema::integer_property("GPU profile frames collected so far.");
		properties["monitor_samples"] = MCPSchema::integer_property("Monitor samples collected so far.");
		return MCPSchema::object_schema(properties, Vector<String>(), true);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPProfilerRecorder *recorder = MCPProfilerRecorder::get_singleton();
		if (!recorder) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		return recorder->status();
	}
};

} // namespace

void mcp_register_profiler_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(StartProfilerTool)));
	registry->register_tool(Ref<MCPTool>(memnew(StopProfilerTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetProfilerStatusTool)));
}
