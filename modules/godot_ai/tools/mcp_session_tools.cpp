/**************************************************************************/
/*  mcp_session_tools.cpp                                                 */
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

// Recording a play session and replaying it.
//
// The store is in `mcp_sessions.{h,cpp}` and the scheduling and divergence rules are in
// `mcp_replay.{h,cpp}`, both testable without a game. This file is the part that needs
// one: it owns the round trips and the clock, and nothing else.
//
// Two deliberate boundaries:
//
//   * **Replay does not start the game.** It requires one already running, so its single
//     declared capability (`simulate_input`) is the whole of the authority it uses. A
//     replay that also launched the project would need `run_project` too, and the model
//     allows a tool exactly one capability - so rather than quietly holding more
//     authority than it declares, it asks the caller to press play. See DEC-0010.
//   * **Recording covers editor-injected input only.** The runtime trace is written by
//     the four `_send_*` handlers and sees nothing else, so a human at the game window
//     is invisible to it. Every reply says so rather than letting a caller assume
//     otherwise.

#include "mcp_builtin_tools.h"

#include "../mcp_deferred.h"
#include "../mcp_replay.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_sessions.h"
#include "../mcp_tool_registry.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/variant/array.h"

namespace {

// The runtime command that replays a recorded event of each kind. The trace stores the
// detail dictionary that was handed to the matching `_send_*` handler, so replaying is
// the same call again.
String command_for_kind(const String &p_kind) {
	if (p_kind == "pointer") {
		return "send_pointer";
	}
	if (p_kind == "key") {
		return "send_key";
	}
	if (p_kind == "touch") {
		return "send_touch";
	}
	if (p_kind == "gamepad") {
		return "send_gamepad";
	}
	return String();
}

// Bookkeeping the recorder adds, which must not be sent back as arguments.
Dictionary event_arguments(const Dictionary &p_event) {
	Dictionary arguments = p_event.duplicate();
	arguments.erase("kind");
	arguments.erase("frame");
	arguments.erase("msec");
	return arguments;
}

const char *READING_GUIDE =
		"Records are JSON Lines under the session directory. trace.jsonl is one input event "
		"per line, each carrying the game's process frame; asserts.jsonl is one assertion "
		"per line. To see the shape of a recording without loading it all: "
		"`jq -s 'group_by(.kind) | map({kind: .[0].kind, count: length})' trace.jsonl`. "
		"Frames are absolute in the file and relative when replayed, so a session recorded "
		"at frame 4000 replays correctly against a game that has just started.";

const char *INPUT_SOURCE_NOTE =
		"This records input the editor injected. It does not observe a person playing the "
		"game window - nothing in the runtime writes that to the trace - so a session is a "
		"record of what the tools did, not of what a player did.";

// ---------------------------------------------------------------- recording ---

class RecordSessionTool : public MCPTool {
	// One recording at a time, because there is one running game.
	String active_slug;
	String active_name;
	int64_t active_start_frame = 0;

public:
	bool is_recording() const { return !active_slug.is_empty(); }
	String get_active_slug() const { return active_slug; }

	virtual String get_tool_name() const override { return "Godot_RecordSession"; }
	virtual String get_description() const override {
		return "Start or stop recording a play session. While a recording is open, every input "
			   "these tools inject into the running game is captured with the frame it landed "
			   "on, and Godot_AssertRuntimeState adds checkpoints of what the game looked like. "
			   "Godot_ReplaySession then re-runs the whole thing after a change and tells you "
			   "the first thing that came out different - a gameplay regression test with no "
			   "test code in it. Recording captures input *these tools* send; a person playing "
			   "the window is invisible to it. Needs a running game: press play first.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> actions;
		actions.push_back("start");
		actions.push_back("stop");
		properties["action"] = MCPSchema::enum_property(
				"'start' opens a recording, 'stop' closes it and writes the trace.", actions);
		properties["name"] = MCPSchema::string_property(
				"What this session is called. Required to start. Becomes the directory name, "
				"lowercased with separators collapsed to '-'; recording the same name again "
				"replaces the previous take.");
		properties["note"] = MCPSchema::string_property(
				"Free text stored with the session - what it is meant to prove.");
		Vector<String> required;
		required.push_back("action");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["recording"] = MCPSchema::bool_property("True while a session is open.");
		properties["session"] = MCPSchema::string_property("Slug of the session.");
		properties["frame"] = MCPSchema::integer_property("Frame the game was on.");
		properties["event_count"] = MCPSchema::integer_property("Events captured (on stop).");
		properties["assertion_count"] = MCPSchema::integer_property("Assertions captured (on stop).");
		properties["directory"] = MCPSchema::string_property("Where the session was written.");
		properties["reading_guide"] = MCPSchema::string_property("How to read the files.");
		properties["input_source_note"] = MCPSchema::string_property("What the trace does and does not see.");
		return MCPSchema::object_schema(properties);
	}

	Dictionary _on_started(const Dictionary &p_payload) {
		const int64_t frame = p_payload.get("frame", 0);
		const MCPSessions::Result result =
				MCPSessions::begin(active_slug, active_name, frame, Dictionary());
		if (!result.ok) {
			// The recording never opened, so nothing may look like it did.
			const String slug = active_slug;
			active_slug = String();
			Dictionary failure;
			failure["recording"] = false;
			failure["session"] = slug;
			failure["error"] = result.error;
			return failure;
		}
		active_start_frame = frame;

		Dictionary answer;
		answer["recording"] = true;
		answer["session"] = active_slug;
		answer["frame"] = frame;
		answer["directory"] = String("user://godot_ai_sessions/") + active_slug;
		answer["input_source_note"] = INPUT_SOURCE_NOTE;
		return answer;
	}

	Dictionary _on_stopped(const Dictionary &p_payload) {
		const String slug = active_slug;
		const int64_t frame = p_payload.get("frame", active_start_frame);
		const Array events = p_payload.get("events", Array());

		// Closed before anything can fail, so a write error cannot leave the tool
		// believing a recording is still open.
		active_slug = String();

		Dictionary answer;
		answer["recording"] = false;
		answer["session"] = slug;
		answer["frame"] = frame;

		const MCPSessions::Result appended = MCPSessions::append_events(slug, events);
		if (!appended.ok) {
			answer["error"] = appended.error;
			MCPSessions::finish(slug, frame, "failed", Dictionary());
			return answer;
		}
		MCPSessions::finish(slug, frame, "recorded", Dictionary());

		const Dictionary meta = MCPSessions::read_meta(slug);
		answer["event_count"] = meta.get("event_count", 0);
		answer["assertion_count"] = meta.get("assertion_count", 0);
		answer["directory"] = meta.get("directory", String("user://godot_ai_sessions/") + slug);
		answer["reading_guide"] = READING_GUIDE;
		answer["input_source_note"] = INPUT_SOURCE_NOTE;
		if ((int)meta.get("event_count", 0) == 0) {
			answer["warning"] = "Nothing was captured. A session with no input replays as "
								"nothing and proves nothing - inject some input between start "
								"and stop, using the Godot_Send* tools.";
		}
		return answer;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no game is running; start one with Godot_PlayMainScene or "
					"Godot_PlayCurrentScene before recording");
			return Dictionary();
		}
		// get() rather than operator[]: a missing key read through a const Dictionary
		// inserts a null, which schema validation then rejects as wrongly typed.
		const String action = p_arguments.get("action", String());

		if (action == "start") {
			if (is_recording()) {
				r_error.set(MCPToolError::INVALID_STATE,
						vformat("already recording '%s'; there is one running game, so there is "
								"one recording. Stop it first.",
								active_slug));
				return Dictionary();
			}
			const String name = p_arguments.get("name", String());
			if (name.strip_edges().is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, "starting a recording needs a 'name'");
				return Dictionary();
			}
			String slug_error;
			const String slug = MCPSessions::slugify(name, slug_error);
			if (slug.is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, slug_error);
				return Dictionary();
			}
			active_slug = slug;
			active_name = name;
			// Clearing the trace is what makes the recording start *here* rather than
			// include whatever the session injected before it.
			Dictionary arguments;
			arguments["clear"] = true;
			const MCPDeferred::Token token = bridge->send("input_trace", arguments, 10.0,
					callable_mp(this, &RecordSessionTool::_on_started));
			if (token == MCPDeferred::INVALID_TOKEN) {
				active_slug = String();
				r_error.set(MCPToolError::FAILED, "the running game did not accept the request");
				return Dictionary();
			}
			return MCPDeferred::make_deferred_result(token);
		}

		if (action == "stop") {
			if (!is_recording()) {
				r_error.set(MCPToolError::INVALID_STATE, "no recording is open");
				return Dictionary();
			}
			Dictionary arguments;
			arguments["clear"] = false;
			const MCPDeferred::Token token = bridge->send("input_trace", arguments, 10.0,
					callable_mp(this, &RecordSessionTool::_on_stopped));
			if (token == MCPDeferred::INVALID_TOKEN) {
				r_error.set(MCPToolError::FAILED, "the running game did not accept the request");
				return Dictionary();
			}
			return MCPDeferred::make_deferred_result(token);
		}

		r_error.set(MCPToolError::INVALID_ARGUMENTS,
				vformat("unknown action '%s'; expected 'start' or 'stop'", action));
		return Dictionary();
	}
};

// The recorder instance, so the assertion tool can ask whether a session is open.
// Function-local, not namespace-scope: a static Ref<> is constructed before the engine's
// memory subsystem and released after it, which is the lifetime hazard behind the
// EditorFileSystem crash earlier in this tranche.
Ref<RecordSessionTool> &recorder() {
	static Ref<RecordSessionTool> instance;
	return instance;
}

// --------------------------------------------------------------- assertions ---

class AssertRuntimeStateTool : public MCPTool {
	String pending_node_path;
	String pending_property;

public:
	virtual String get_tool_name() const override { return "Godot_AssertRuntimeState"; }
	virtual String get_description() const override {
		return "Read a property from the running game and store it in the open recording as an "
			   "assertion: this is what this value was, at this frame. Replaying the session "
			   "checks it again and reports the first one that comes back different. Capture "
			   "these at the moments that matter - the boss died, the door opened, the score "
			   "hit a thousand - because a trace without assertions only detects crashes.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["node_path"] = MCPSchema::string_property(
				"Node in the running game, for example 'Main/Player'.");
		properties["property"] = MCPSchema::string_property("Property to capture.");
		Vector<String> required;
		required.push_back("node_path");
		required.push_back("property");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["session"] = MCPSchema::string_property("Session the assertion was added to.");
		properties["node_path"] = MCPSchema::string_property("Node that was read.");
		properties["property"] = MCPSchema::string_property("Property that was read.");
		properties["value"] = MCPSchema::any_property("The value recorded.");
		properties["text"] = MCPSchema::string_property("The value as the engine prints it.");
		properties["frame"] = MCPSchema::integer_property("Frame it was observed at.");
		properties["assertion_count"] = MCPSchema::integer_property("Assertions in the session now.");
		return MCPSchema::object_schema(properties);
	}

	Dictionary _on_read(const Dictionary &p_payload) {
		Dictionary answer;
		const String slug = recorder().is_valid() ? recorder()->get_active_slug() : String();
		if (slug.is_empty()) {
			answer["error"] = "the recording closed while this property was being read";
			return answer;
		}
		Dictionary assertion;
		assertion["node_path"] = pending_node_path;
		assertion["property"] = pending_property;
		assertion["value"] = p_payload.get("value", Variant());
		assertion["text"] = p_payload.get("text", String());
		assertion["frame"] = p_payload.get("frame", 0);

		Array assertions;
		assertions.push_back(assertion);
		const MCPSessions::Result result = MCPSessions::append_assertions(slug, assertions);

		answer["session"] = slug;
		answer["node_path"] = pending_node_path;
		answer["property"] = pending_property;
		answer["value"] = assertion["value"];
		answer["text"] = assertion["text"];
		answer["frame"] = assertion["frame"];
		if (!result.ok) {
			answer["error"] = result.error;
			return answer;
		}
		answer["assertion_count"] = MCPSessions::read_assertions(slug).size();
		return answer;
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (recorder().is_null() || !recorder()->is_recording()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no recording is open; an assertion belongs to a session, so call "
					"Godot_RecordSession with action='start' first");
			return Dictionary();
		}
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error.set(MCPToolError::INVALID_STATE, "no game is running");
			return Dictionary();
		}

		pending_node_path = p_arguments.get("node_path", String());
		pending_property = p_arguments.get("property", String());

		Dictionary arguments;
		arguments["path"] = pending_node_path;
		arguments["property"] = pending_property;
		const MCPDeferred::Token token = bridge->send("get_property", arguments, 10.0,
				callable_mp(this, &AssertRuntimeStateTool::_on_read));
		if (token == MCPDeferred::INVALID_TOKEN) {
			r_error.set(MCPToolError::FAILED, "the running game did not accept the request");
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(token);
	}
};

// ------------------------------------------------------------------ listing ---

class ListSessionsTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListSessions"; }
	virtual String get_description() const override {
		return "List the recorded play sessions, newest first, with how many input events and "
			   "assertions each holds and how its last replay went. Needs no running game.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		return MCPSchema::object_schema(Dictionary());
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["sessions"] = MCPSchema::array_property("One entry per recorded session.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["count"] = MCPSchema::integer_property("How many there are.");
		properties["root"] = MCPSchema::string_property("Where they are stored.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const Array sessions = MCPSessions::list();
		Dictionary result;
		result["sessions"] = sessions;
		result["count"] = sessions.size();
		result["root"] = "user://godot_ai_sessions";
		if (sessions.is_empty()) {
			result["note"] = "Nothing has been recorded yet. Start a game, then call "
							 "Godot_RecordSession with action='start'.";
		}
		return result;
	}
};

// ------------------------------------------------------------------- replay ---

// Drives a loaded MCPReplayPlan against the running game.
//
// A poll asks the game what frame it is on, hands that to the plan, and does what the
// plan says: inject these events, check these assertions. Each of those is another round
// trip, so the tool tracks how many answers it is still waiting for and does not advance
// until they are all in - otherwise it would ask the game for a frame it has already
// acted on and inject the same event twice.
class ReplaySessionTool : public MCPTool {
	MCPReplayPlan plan;
	String slug;
	bool running = false;
	bool bound = false;
	int outstanding = 0;
	double deadline = 0.0;
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;

	void _finish() {
		running = false;
		Dictionary report = plan.to_report();
		report["session"] = slug;
		report["reading_guide"] = READING_GUIDE;
		// Records the verdict without touching the recording's own frames: a replay
		// reports on a session, it does not re-record it.
		MCPSessions::set_replay_result(slug, String(report["verdict"]), report);
		if (token != MCPDeferred::INVALID_TOKEN) {
			MCPDeferred::complete(token, report);
			token = MCPDeferred::INVALID_TOKEN;
		}
	}

	void _fail(const String &p_message) {
		running = false;
		if (token != MCPDeferred::INVALID_TOKEN) {
			MCPDeferred::fail(token, MCPToolError::FAILED, p_message);
			token = MCPDeferred::INVALID_TOKEN;
		}
	}

	void _on_injected(bool p_ok, const Dictionary &p_payload) {
		outstanding = MAX(0, outstanding - 1);
	}

	// Callable::bind() appends its arguments after the caller's, so the bound index is
	// the *last* parameter, not the first.
	void _on_asserted(bool p_ok, const Dictionary &p_payload, int p_index) {
		outstanding = MAX(0, outstanding - 1);
		if (!p_ok) {
			// A node that has gone, or a property that no longer exists, is itself a
			// divergence: the recording reached it and this run cannot.
			plan.report_assertion(p_index, Variant());
			return;
		}
		plan.report_assertion(p_index, p_payload.get("value", Variant()));
	}

	void _on_frame(bool p_ok, const Dictionary &p_payload) {
		outstanding = MAX(0, outstanding - 1);
		if (!p_ok || !running) {
			return;
		}
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge) {
			return;
		}
		const int64_t frame = p_payload.get("frame", 0);
		if (!bound) {
			// The game is thousands of frames in; the recording starts at zero relative.
			// Binding here rather than in run() is what keeps the very first event from
			// looking thousands of frames late and the whole run indeterminate.
			plan.start(frame);
			bound = true;
		}

		Vector<MCPReplayPlan::DueAssertion> due;
		const Array to_inject = plan.observe(frame, due);

		for (int i = 0; i < to_inject.size(); i++) {
			const Dictionary event = to_inject[i];
			const String command = command_for_kind(event.get("kind", String()));
			if (command.is_empty()) {
				continue;
			}
			if (bridge->request(command, event_arguments(event), 10.0,
						callable_mp(this, &ReplaySessionTool::_on_injected))) {
				outstanding++;
			}
		}
		for (int i = 0; i < due.size(); i++) {
			Dictionary arguments;
			arguments["path"] = due[i].node_path;
			arguments["property"] = due[i].property;
			if (bridge->request("get_property", arguments, 10.0,
						callable_mp(this, &ReplaySessionTool::_on_asserted).bind(due[i].index))) {
				outstanding++;
			}
		}
	}

	bool _poll() {
		if (!running) {
			return true;
		}
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			_fail("the game stopped while the session was replaying");
			return true;
		}
		if (OS::get_singleton()->get_ticks_msec() / 1000.0 > deadline) {
			_fail(vformat("replaying '%s' ran past its time budget", slug));
			return true;
		}
		if (plan.is_finished()) {
			_finish();
			return true;
		}
		// One question at a time. Asking again while answers are outstanding would act on
		// a frame the plan has already moved past.
		if (outstanding == 0) {
			if (bridge->request("ping", Dictionary(), 10.0,
						callable_mp(this, &ReplaySessionTool::_on_frame))) {
				outstanding++;
			}
		}
		return false;
	}

public:
	virtual String get_tool_name() const override { return "Godot_ReplaySession"; }
	virtual String get_description() const override {
		return "Replay a recorded session against the running game and report the first thing "
			   "that came out different. Events are re-injected at their recorded frame spacing, "
			   "not their recorded absolute frames, so a session recorded deep into a run "
			   "replays against a game that has just started. The verdict is 'passed', 'failed' "
			   "with the first divergence and both values, or 'indeterminate' - which means the "
			   "game fell too far behind the recording for the run to prove anything, and must "
			   "not be read as a pass. Needs a game already running: this tool does not start "
			   "one, so that injecting input is the only authority it holds.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_SIMULATE_INPUT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property("Session to replay, by name or slug.");
		properties["drift_tolerance_frames"] = MCPSchema::integer_property(
				"How many frames the game may fall behind the recording before the run is "
				"called indeterminate. Raising this does not make a drifting run trustworthy.",
				MCPReplayPlan::DEFAULT_DRIFT_TOLERANCE);
		properties["timeout_seconds"] = MCPSchema::integer_property(
				"How long to allow the whole replay.", 120);
		Vector<String> required;
		required.push_back("name");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["session"] = MCPSchema::string_property("Session that was replayed.");
		properties["verdict"] = MCPSchema::string_property(
				"'passed', 'failed' or 'indeterminate'.");
		properties["events_injected"] = MCPSchema::integer_property("Events re-injected.");
		properties["assertions_checked"] = MCPSchema::integer_property("Assertions checked.");
		properties["assertions_matched"] = MCPSchema::integer_property("Assertions that matched.");
		properties["max_drift_frames"] = MCPSchema::integer_property(
				"Worst lateness of any injected event, in frames.");
		properties["first_divergence"] = MCPSchema::object_schema(
				Dictionary(), Vector<String>(), true);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (running) {
			r_error.set(MCPToolError::INVALID_STATE,
					vformat("already replaying '%s'; there is one running game", slug));
			return Dictionary();
		}
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no game is running. This tool deliberately does not start one - press play "
					"with Godot_PlayMainScene first, so that replaying holds no more authority "
					"than injecting input");
			return Dictionary();
		}

		const String name = p_arguments.get("name", String());
		String slug_error;
		slug = MCPSessions::slugify(name, slug_error);
		if (slug.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, slug_error);
			return Dictionary();
		}
		if (!MCPSessions::exists(slug)) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("no recorded session '%s'; Godot_ListSessions shows what there is", slug));
			return Dictionary();
		}

		const Dictionary meta = MCPSessions::read_meta(slug);
		String load_error;
		if (!plan.load(MCPSessions::read_events(slug), MCPSessions::read_assertions(slug),
					meta.get("start_frame", 0), load_error)) {
			r_error.set(MCPToolError::INVALID_STATE, load_error);
			return Dictionary();
		}
		plan.set_drift_tolerance(p_arguments.get("drift_tolerance_frames",
				MCPReplayPlan::DEFAULT_DRIFT_TOLERANCE));

		const int budget = MAX(1, (int)p_arguments.get("timeout_seconds", 120));
		deadline = OS::get_singleton()->get_ticks_msec() / 1000.0 + budget;
		outstanding = 0;
		running = true;
		bound = false;

		token = MCPDeferred::begin_polled(budget + 15.0,
				callable_mp(this, &ReplaySessionTool::_poll));
		return MCPDeferred::make_deferred_result(token);
	}
};

} // namespace

void mcp_register_session_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	recorder() = Ref<RecordSessionTool>(memnew(RecordSessionTool));
	registry->register_tool(recorder());
	registry->register_tool(Ref<MCPTool>(memnew(AssertRuntimeStateTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ListSessionsTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ReplaySessionTool)));
}
