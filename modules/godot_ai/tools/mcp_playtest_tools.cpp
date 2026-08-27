/**************************************************************************/
/*  mcp_playtest_tools.cpp                                                */
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

// Running a playtest and getting a report out of it.
//
// The assembly and the verdict rules are in `mcp_playtest.{h,cpp}`, testable without a
// game. This file is the part that needs one: it reads the output log, decides whether
// a game is running at all, and owns the tool surface.
//
// Composition, not new capability. A playtest presses the same input tools an agent
// would press by hand and reads the same activity stream the dock reads; what it adds is
// a window with a stated goal at one end and an assembled, checkable report at the
// other. That is the whole point of the tranche: the primitives existed and nothing
// strung them into a workflow.
//
// Like replay, a playtest does not start the game - see DEC-0010 and the header.

#include "mcp_builtin_tools.h"

#include "../mcp_playtest.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_schema.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"

#ifdef TOOLS_ENABLED
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#endif

namespace {

// Whether a game this editor can see is running. A playtest of nothing is a report about
// nothing, and saying so at the start beats an empty report at the end.
bool a_game_is_running(String &r_reason) {
#ifdef TOOLS_ENABLED
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (debugger && debugger->get_default_debugger() &&
			debugger->get_default_debugger()->is_session_active()) {
		return true;
	}
	r_reason = "no game is running. A playtest observes a running game; press play, or use "
			   "Godot_PlayMainScene or Godot_LaunchInstance first.";
	return false;
#else
	r_reason = "a playtest needs a running editor";
	return false;
#endif
}

// Errors and warnings the game printed. Read from the editor's own log, which is the
// same place Godot_ReadOutputLog answers from - so the report and that tool tell the
// same story rather than two slightly different ones.
Array recent_problems(int p_limit) {
	Array messages;
#ifdef TOOLS_ENABLED
	if (!EditorNode::get_singleton() || !EditorNode::get_log()) {
		return messages;
	}
	EditorLog *log = EditorNode::get_log();
	const int total = log->get_message_count();
	const int first = MAX(0, total - p_limit);
	for (int i = first; i < total; i++) {
		const EditorLog::MessageType type = log->get_message_type(i);
		if (type != EditorLog::MSG_TYPE_ERROR && type != EditorLog::MSG_TYPE_WARNING) {
			continue;
		}
		Dictionary message;
		message["severity"] = type == EditorLog::MSG_TYPE_ERROR ? "error" : "warning";
		message["text"] = log->get_message_text(i);
		messages.push_back(message);
	}
#endif
	return messages;
}

class StartPlaytestTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_StartPlaytest"; }
	virtual String get_description() const override {
		return "Open a playtest: a stated goal, a time budget, and a window during which "
			   "everything you do to the running game is collected into a report. Play it with "
			   "the input tools as you normally would, add what you notice with "
			   "Godot_NotePlaytestObservation, then close it with Godot_FinishPlaytest to get "
			   "the report. The report is the product - it carries what was actually pressed, "
			   "what the game logged, what you observed, and a verdict that is checked against "
			   "the evidence rather than taken on trust. Needs a running game: press play first.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["goal"] = MCPSchema::string_property(
				"What this playtest is trying to find out or reach, in prose. 'Reach the second "
				"room without taking damage', not 'test the game'. The verdict is about this.");
		properties["name"] = MCPSchema::string_property(
				"What to call it. Defaults to the goal. Becomes the directory name, lowercased "
				"with separators collapsed to '-'; the same name again replaces the previous run.");
		properties["budget_seconds"] = MCPSchema::integer_property(
				"How long this should take. The playtest is not stopped automatically - you "
				"decide what to do about it - but the report says whether it ran over, and a "
				"'not reached' that ran out of time is reported as indeterminate rather than "
				"as a failure.",
				120);
		properties["oracle"] = MCPSchema::string_property(
				"How you will know the goal was reached: the condition you will check. Stored "
				"in the report so a reader can judge the verdict rather than believe it.");
		Vector<String> required;
		required.push_back("goal");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["playtest"] = MCPSchema::string_property("Slug of the playtest.");
		properties["goal"] = MCPSchema::string_property("The goal it is about.");
		properties["budget_seconds"] = MCPSchema::integer_property("The budget it was given.");
		properties["directory"] = MCPSchema::string_property("Where the report will be written.");
		properties["next"] = MCPSchema::string_property("What to do now.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String goal = String(p_arguments.get("goal", String())).strip_edges();
		if (goal.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					"a playtest needs a goal stated in prose; without one there is nothing for "
					"the verdict to be about");
			return Dictionary();
		}

		String reason;
		if (!a_game_is_running(reason)) {
			r_error.set(MCPToolError::UNSUPPORTED, reason);
			return Dictionary();
		}

		const String name = String(p_arguments.get("name", goal)).strip_edges();
		String slug_error;
		const String slug = MCPPlaytest::slugify(name.is_empty() ? goal : name, slug_error);
		if (slug.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, slug_error);
			return Dictionary();
		}

		const int budget = (int)p_arguments.get("budget_seconds", 120);
		const String oracle = p_arguments.get("oracle", String());

		Dictionary context;
		context["oracle"] = oracle;

		const MCPPlaytest::Result result = MCPPlaytest::begin(slug, goal, budget, oracle, context);
		if (!result.ok) {
			r_error.set(MCPToolError::FAILED, result.error);
			return Dictionary();
		}

		Dictionary answer;
		answer["playtest"] = slug;
		answer["goal"] = goal;
		answer["budget_seconds"] = budget;
		answer["directory"] = MCPPlaytest::get_playtest_dir(slug);
		answer["next"] = "Play the game with the input tools. Record what you notice with "
						 "Godot_NotePlaytestObservation. Close it with Godot_FinishPlaytest.";
		return answer;
	}
};

class NotePlaytestObservationTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_NotePlaytestObservation"; }
	virtual String get_description() const override {
		return "Record something you noticed during the running playtest - what you saw, what "
			   "you tried, what surprised you. These are kept apart from the collected evidence "
			   "in the report, because one is your account and the other is what the editor "
			   "recorded.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["note"] = MCPSchema::string_property("What you noticed.");
		Vector<String> kinds;
		kinds.push_back("note");
		kinds.push_back("problem");
		kinds.push_back("progress");
		kinds.push_back("blocked");
		properties["kind"] = MCPSchema::enum_property(
				"What sort of observation this is.", kinds, "note");
		Vector<String> required;
		required.push_back("note");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["recorded"] = MCPSchema::bool_property("True when it was stored.");
		properties["playtest"] = MCPSchema::string_property("The playtest it belongs to.");
		properties["at_second"] = MCPSchema::integer_property("Seconds into the playtest.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String note = p_arguments.get("note", String());
		const String kind = p_arguments.get("kind", "note");
		const int at_second = MCPPlaytest::get_elapsed_seconds();

		const MCPPlaytest::Result result = MCPPlaytest::observe(note, kind);
		if (!result.ok) {
			r_error.set(MCPToolError::FAILED, result.error);
			return Dictionary();
		}

		Dictionary answer;
		answer["recorded"] = true;
		answer["playtest"] = MCPPlaytest::get_active_slug();
		answer["at_second"] = at_second;
		return answer;
	}
};

class FinishPlaytestTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_FinishPlaytest"; }
	virtual String get_description() const override {
		return "Close the running playtest and get its report. You give the verdict and a "
			   "summary; everything else is assembled from what the editor recorded during the "
			   "window - every call you made, every input you injected, what the game logged, "
			   "and your own observations. The verdict is reconciled against that evidence: a "
			   "goal reported as reached while the game logged errors, or while no input was "
			   "injected at all, comes back as indeterminate with the reason attached. Use "
			   "'stop' to end a playtest early and keep the partial results.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> verdicts;
		verdicts.push_back("reached");
		verdicts.push_back("not_reached");
		verdicts.push_back("blocked");
		verdicts.push_back("indeterminate");
		verdicts.push_back("stop");
		properties["verdict"] = MCPSchema::enum_property(
				"'reached' if the goal was met, 'not_reached' if it was not and nothing broke, "
				"'blocked' if something prevented the attempt, 'indeterminate' if the run does "
				"not support a conclusion, 'stop' to end early and keep partial results.",
				verdicts);
		properties["summary"] = MCPSchema::string_property(
				"What happened, in prose, for whoever reads the report.");
		Vector<String> required;
		required.push_back("verdict");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["report"] = MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
		properties["directory"] = MCPSchema::string_property("Where it was written.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!MCPPlaytest::is_running()) {
			r_error.set(MCPToolError::FAILED, "no playtest is running");
			return Dictionary();
		}

		const String requested = String(p_arguments.get("verdict", String())).strip_edges().to_lower();
		const String summary = p_arguments.get("summary", String());
		const String slug = MCPPlaytest::get_active_slug();

		// Read the game's own errors before closing, so they land in the report rather
		// than being something the reader has to go and look up.
		const Array problems = MCPPlaytest::problems_from_log(recent_problems(400));

		Dictionary report;
		MCPPlaytest::Result result;
		if (requested == "stop") {
			result = MCPPlaytest::abandon(summary.is_empty() ? String("stopped by the caller") : summary,
					report);
		} else {
			bool known = false;
			const MCPPlaytest::Verdict verdict = MCPPlaytest::verdict_from_string(requested, known);
			if (!known) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS,
						vformat("'%s' is not a verdict; use reached, not_reached, blocked, "
								"indeterminate or stop",
								requested));
				return Dictionary();
			}
			result = MCPPlaytest::finish(verdict, summary, report);
		}

		if (!result.ok) {
			r_error.set(MCPToolError::FAILED, result.error);
			return Dictionary();
		}

		// The game's own problems are merged in after assembly, and the counts follow, so
		// a reader is never shown a count that disagrees with the list beside it.
		if (!problems.is_empty()) {
			Array merged = report.get("problems", Array());
			for (int i = 0; i < problems.size(); i++) {
				merged.push_back(problems[i]);
			}
			report["problems"] = merged;
			Dictionary counts = report.get("counts", Dictionary());
			counts["problems"] = merged.size();
			report["counts"] = counts;
		}

		Dictionary answer;
		answer["report"] = report;
		answer["directory"] = MCPPlaytest::get_playtest_dir(slug);
		return answer;
	}
};

class GetPlaytestReportTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetPlaytestReport"; }
	virtual String get_description() const override {
		return "Read a finished playtest's report back, or list what has been recorded. The "
			   "report carries the goal, the verdict and why it is that verdict, every input "
			   "the playtest injected, what the game logged, and the observations made along "
			   "the way.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["playtest"] = MCPSchema::string_property(
				"Which one. Omit to list every recorded playtest instead.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["report"] = MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
		properties["playtests"] = MCPSchema::array_property(
				"Every recorded playtest, when none was named.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["running"] = MCPSchema::string_property(
				"Slug of the playtest currently open, if any.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary answer;
		answer["running"] = MCPPlaytest::get_active_slug();

		const String requested = String(p_arguments.get("playtest", String())).strip_edges();
		if (requested.is_empty()) {
			answer["playtests"] = MCPPlaytest::list();
			return answer;
		}

		String error;
		const Dictionary report = MCPPlaytest::get_report(requested, error);
		if (report.is_empty()) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}
		answer["report"] = report;
		return answer;
	}
};

} // namespace

void mcp_register_playtest_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(StartPlaytestTool)));
	registry->register_tool(Ref<MCPTool>(memnew(NotePlaytestObservationTool)));
	registry->register_tool(Ref<MCPTool>(memnew(FinishPlaytestTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetPlaytestReportTool)));
}
