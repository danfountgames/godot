/**************************************************************************/
/*  test_mcp_profiler.h                                                   */
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

#ifndef TEST_MCP_PROFILER_H
#define TEST_MCP_PROFILER_H

#include "modules/godot_ai/mcp_profiler_recorder.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "main/performance.h"
#include "servers/debugger/servers_debugger.h"

#include "tests/test_macros.h"

namespace TestMCPProfiler {

// The recorder is fed the exact wire payloads the game's profilers produce,
// built with the engine's own serializers so the test cannot drift from the
// protocol. No debugger session exists here; start_detached() is the seam.

static String make_scratch_dir() {
	const String scratch = OS::get_singleton()->get_cache_path().simplify_path().path_join(
			vformat("godot_ai_test_profiler_%d", OS::get_singleton()->get_process_id()));
	DirAccess::make_dir_recursive_absolute(scratch);
	return scratch;
}

static Array serialized_signature(int p_id, const String &p_name) {
	ServersDebugger::ScriptFunctionSignature sig;
	sig.id = p_id;
	sig.name = p_name;
	return sig.serialize();
}

static Array serialized_frame(int p_frame_number, double p_frame_seconds, int p_sig_id,
		double p_self_seconds, int p_calls) {
	ServersDebugger::ServersProfilerFrame frame;
	frame.frame_number = p_frame_number;
	frame.frame_time = p_frame_seconds;
	frame.process_time = p_frame_seconds * 0.75;
	frame.physics_time = p_frame_seconds * 0.125;
	frame.physics_frame_time = p_frame_seconds;
	frame.script_time = p_self_seconds;

	ServersDebugger::ServerInfo server;
	server.name = "physics_2d";
	ServersDebugger::ServerFunctionInfo fn;
	fn.name = "step";
	fn.time = 0.001;
	server.functions.push_back(fn);
	frame.servers.push_back(server);

	ServersDebugger::ScriptFunctionInfo info;
	info.sig_id = p_sig_id;
	info.call_count = p_calls;
	info.self_time = p_self_seconds;
	info.total_time = p_self_seconds * 1.25;
	info.internal_time = p_self_seconds * 0.05;
	frame.script_functions.push_back(info);
	return frame.serialize();
}

static Array serialized_visual_frame(uint64_t p_rs_frame) {
	ServersDebugger::VisualProfilerFrame frame;
	frame.frame_number = p_rs_frame;
	// Cumulative timestamps with the engine's '>'/'<' nesting markers, the way
	// RenderingServer reports them.
	RenderingServerTypes::FrameProfileArea open;
	open.name = ">Frame";
	open.cpu_msec = 0.0;
	open.gpu_msec = 0.0;
	RenderingServerTypes::FrameProfileArea scene;
	scene.name = "Render Scene";
	scene.cpu_msec = 0.5;
	scene.gpu_msec = 1.0;
	RenderingServerTypes::FrameProfileArea post;
	post.name = "Post Effects";
	post.cpu_msec = 0.8;
	post.gpu_msec = 3.0;
	RenderingServerTypes::FrameProfileArea close;
	close.name = "<Frame";
	close.cpu_msec = 0.9;
	close.gpu_msec = 3.5;
	frame.areas.push_back(open);
	frame.areas.push_back(scene);
	frame.areas.push_back(post);
	frame.areas.push_back(close);
	return frame.serialize();
}

static Array monitor_frame() {
	Array values;
	values.resize(Performance::MONITOR_MAX);
	for (int i = 0; i < values.size(); i++) {
		values[i] = 0.0;
	}
	values[Performance::TIME_FPS] = 60.0;
	values[Performance::TIME_PROCESS] = 0.016;
	values[Performance::MEMORY_STATIC] = 1000000.0;
	values[Performance::RENDER_VIDEO_MEM_USED] = 2000000.0;
	return values;
}

static Array serialized_resource_usage() {
	ServersDebugger::ResourceUsage usage;
	ServersDebugger::ResourceInfo big;
	big.path = "res://big.png";
	big.type = "Texture2D";
	big.format = "1024x1024 RGBA8";
	big.vram = 4194304;
	usage.infos.push_back(big);
	ServersDebugger::ResourceInfo small;
	small.path = "res://small.png";
	small.type = "Texture2D";
	small.format = "64x64 RGBA8";
	small.vram = 16384;
	usage.infos.push_back(small);
	return usage.serialize();
}

static Vector<Dictionary> read_export(const String &p_path) {
	Vector<Dictionary> records;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	REQUIRE(file.is_valid());
	while (!file->eof_reached()) {
		const String line = file->get_line();
		if (line.is_empty()) {
			continue;
		}
		const Variant parsed = JSON::parse_string(line);
		REQUIRE_MESSAGE(parsed.get_type() == Variant::DICTIONARY, vformat("unparseable line: %s", line));
		records.push_back(parsed);
	}
	return records;
}

static Dictionary find_record(const Vector<Dictionary> &p_records, const String &p_type,
		const String &p_phase = String()) {
	for (const Dictionary &record : p_records) {
		if (String(record.get("type", "")) != p_type) {
			continue;
		}
		if (!p_phase.is_empty() && String(record.get("phase", "")) != p_phase) {
			continue;
		}
		return record;
	}
	return Dictionary();
}

TEST_CASE("[godot_ai] A profiler capture exports every stream and summarizes it") {
	const String scratch = make_scratch_dir();
	const String path = scratch.path_join("capture.jsonl");

	Ref<MCPProfilerRecorder> recorder;
	recorder.instantiate();

	// The options dictionary is read through a const reference; a lookup that
	// inserts (the Dictionary::operator[] trap) would grow it.
	const Dictionary options;
	String error;
	const Dictionary started = recorder->start_detached(path, options, error);
	REQUIRE_MESSAGE(!started.is_empty(), error);
	CHECK(options.is_empty());
	CHECK(String(started["export_absolute_path"]) == path);
	CHECK(!String(started["reading_guide"]).is_empty());

	// A second start must refuse while the first is live.
	String second_error;
	CHECK(recorder->start_detached(path, options, second_error).is_empty());
	CHECK(!second_error.is_empty());

	recorder->handle_message("servers:function_signature", serialized_signature(0, "res://player.gd::_process"));
	recorder->handle_message("servers:profile_frame", serialized_frame(100, 0.016, 0, 0.004, 2));
	recorder->handle_message("servers:profile_frame", serialized_frame(101, 0.040, 0, 0.020, 3));
	recorder->handle_message("visual:profile_frame", serialized_visual_frame(7));
	// The same completed GPU profile reported again must not be double-counted.
	recorder->handle_message("visual:profile_frame", serialized_visual_frame(7));
	recorder->handle_message("performance:profile_frame", monitor_frame());
	recorder->handle_message("servers:memory_usage", serialized_resource_usage());

	// An accumulated total outside a stop is contention, not data. Handling it
	// resets the wire signature map (a re-enabled game profiler reassigns ids), so
	// the game's re-announcement is replayed the way the real stream would.
	ServersDebugger::ServersProfilerFrame stray;
	stray.frame_number = 102;
	recorder->handle_message("servers:profile_total", stray.serialize());
	CHECK(recorder->get_state() == MCPProfilerRecorder::RECORDING);
	recorder->handle_message("servers:function_signature", serialized_signature(0, "res://player.gd::_process"));

	String stop_error;
	REQUIRE(recorder->begin_stop("stopped", stop_error));
	recorder->handle_message("servers:memory_usage", serialized_resource_usage());

	ServersDebugger::ServersProfilerFrame total;
	total.frame_number = 103;
	ServersDebugger::ScriptFunctionInfo accumulated;
	accumulated.sig_id = 0;
	accumulated.call_count = 120;
	accumulated.self_time = 0.4;
	accumulated.total_time = 0.5;
	accumulated.internal_time = 0.02;
	total.script_functions.push_back(accumulated);
	recorder->handle_message("servers:profile_total", total.serialize());

	const Dictionary result = recorder->finalize_now("stopped");
	REQUIRE(!result.is_empty());
	CHECK(recorder->get_state() == MCPProfilerRecorder::IDLE);
	CHECK_FALSE(bool(result["partial"]));
	CHECK(String(result["end_reason"]) == "stopped");
	CHECK(!String(result["reading_guide"]).is_empty());

	const Vector<Dictionary> records = read_export(path);
	REQUIRE(records.size() > 5);
	CHECK(String(records[0].get("type", "")) == "header");
	CHECK(String(records[records.size() - 1].get("type", "")) == "summary");

	const Dictionary sig = find_record(records, "sig");
	CHECK(String(sig.get("name", "")) == "res://player.gd::_process");

	const Dictionary frame = find_record(records, "frame");
	CHECK(double(frame.get("frame_ms", 0.0)) == doctest::Approx(16.0));
	const Dictionary servers = frame.get("servers", Dictionary());
	const Dictionary physics = servers.get("physics_2d", Dictionary());
	CHECK(double(physics.get("step", 0.0)) == doctest::Approx(1.0));
	const Array funcs = frame.get("funcs", Array());
	REQUIRE(funcs.size() == 1);
	CHECK(double(Array(funcs[0])[2]) == doctest::Approx(4.0)); // self_ms

	const Dictionary gpu = find_record(records, "gpu");
	CHECK(double(gpu.get("total_gpu_ms", 0.0)) == doctest::Approx(3.5));
	const Array areas = gpu.get("areas", Array());
	// The '>'/'<' marker rows are structure, not passes; two real areas remain.
	REQUIRE(areas.size() == 2);
	CHECK(String(Array(areas[0])[0]) == "Render Scene");
	CHECK(double(Array(areas[0])[2]) == doctest::Approx(2.0)); // gpu delta to next row

	const Dictionary mon = find_record(records, "mon");
	CHECK(double(mon.get("process_ms", 0.0)) == doctest::Approx(16.0));
	CHECK(double(mon.get("static_mem", 0.0)) == doctest::Approx(1000000.0));
	CHECK(double(mon.get("video_mem", 0.0)) == doctest::Approx(2000000.0));

	CHECK(!find_record(records, "vram", "start").is_empty());
	const Dictionary vram_end = find_record(records, "vram", "end");
	REQUIRE(!vram_end.is_empty());
	CHECK(double(vram_end.get("total_bytes", 0.0)) == doctest::Approx(4210688.0));
	const Array resources = vram_end.get("resources", Array());
	REQUIRE(resources.size() == 2);
	CHECK(String(Array(resources[0])[0]) == "res://big.png"); // Largest first.

	const Dictionary event = find_record(records, "event");
	CHECK(String(event.get("what", "")).contains("turned off outside this capture"));

	const Dictionary total_record = find_record(records, "total");
	REQUIRE(!total_record.is_empty());

	const Dictionary summary = result["summary"];
	const Dictionary window = summary["window"];
	CHECK(int(window.get("frames", 0)) == 2);
	const Dictionary frame_stats = summary["frame_ms"];
	CHECK(double(frame_stats.get("worst_ms", 0.0)) == doctest::Approx(40.0));
	CHECK(int(frame_stats.get("worst_at_process_frame", 0)) == 101);
	const Dictionary top = summary["top_functions"];
	CHECK(String(top.get("source", "")) == "accumulated_total");
	const Array rows = top.get("rows", Array());
	REQUIRE(rows.size() == 1);
	CHECK(String(Dictionary(rows[0]).get("name", "")) == "res://player.gd::_process");
	CHECK(double(Dictionary(rows[0]).get("self_ms", 0.0)) == doctest::Approx(400.0));
	const Dictionary gpu_summary = summary["gpu"];
	CHECK(int(gpu_summary.get("frames", 0)) == 1);

	// The result stays claimable exactly once, for a stop that arrives after the
	// capture already finalized.
	CHECK_FALSE(recorder->take_result().is_empty());
	CHECK(recorder->take_result().is_empty());

	mcp_test_remove_tree(scratch);
}

TEST_CASE("[godot_ai] A capture that never got the total is honestly partial") {
	const String scratch = make_scratch_dir();
	const String path = scratch.path_join("partial.jsonl");

	Ref<MCPProfilerRecorder> recorder;
	recorder.instantiate();

	String error;
	REQUIRE(!recorder->start_detached(path, Dictionary(), error).is_empty());
	recorder->handle_message("servers:function_signature", serialized_signature(0, "res://enemy.gd::_physics_process"));
	recorder->handle_message("servers:profile_frame", serialized_frame(1, 0.010, 0, 0.002, 1));

	const Dictionary result = recorder->finalize_now("game_stopped");
	CHECK(bool(result["partial"]));
	CHECK(String(result["end_reason"]) == "game_stopped");
	// Uncommanded ends carry a warning the caller cannot miss.
	CHECK(!String(result.get("note", "")).is_empty());

	const Dictionary summary = result["summary"];
	const Dictionary top = summary["top_functions"];
	CHECK(String(top.get("source", "")) == "per_frame_fold");
	const Array rows = top.get("rows", Array());
	REQUIRE(rows.size() == 1);
	CHECK(String(Dictionary(rows[0]).get("name", "")) == "res://enemy.gd::_physics_process");

	// The export ends with a summary line even for a partial capture.
	const Vector<Dictionary> records = read_export(path);
	CHECK(String(records[records.size() - 1].get("type", "")) == "summary");

	// The unclaimed result waits for the next stop call, once.
	CHECK_FALSE(recorder->take_result().is_empty());
	CHECK(recorder->take_result().is_empty());

	mcp_test_remove_tree(scratch);
}

TEST_CASE("[godot_ai] Profiler stop without start refuses with directions") {
	Ref<MCPProfilerRecorder> recorder;
	recorder.instantiate();

	String error;
	CHECK_FALSE(recorder->begin_stop("stopped", error));
	CHECK(error.contains("Godot_StartProfiler"));
}

} // namespace TestMCPProfiler

#endif // TEST_MCP_PROFILER_H
