/**************************************************************************/
/*  mcp_playtest.cpp                                                      */
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

#include "mcp_playtest.h"

#include "mcp_activity.h"
#include "mcp_paths.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/variant.h"

namespace {

const char *REPORT_FILE = "report.json";
const char *ACTIVITY_FILE = "activity.jsonl";

// The tools that actually drive the game. A playtest's claim about what it pressed is
// only checkable against the calls that could have pressed anything.
bool is_input_tool(const String &p_tool) {
	return p_tool == "Godot_SendPointerInput" ||
			p_tool == "Godot_SendKeyInput" ||
			p_tool == "Godot_SendActionInput" ||
			p_tool == "Godot_SendEditorInput";
}

// Driving a game by writing a property on it, which is the other way to play.
//
// The reconciliation exists to catch a report written from the source rather than from
// the game, and it did that by counting injected input. But a game built for this
// interface exposes its verbs as properties precisely because a simulated drag is a bad
// unit of intent to assert about - the agent-facing workflow recommends that interface.
// So a playtest driven the
// recommended way scored "indeterminate: this playtest injected no input at all", with
// 391 calls behind it. A false negative aimed squarely at the workflow the product
// tells people to use.
//
// A write to the running game is acting on it. A *read* is not, and that distinction is
// the whole point: a report assembled from Godot_GetRuntimeProperty and nothing else
// still cannot account for anything.
// A call the agent got wrong, as opposed to something the game did.
//
// A schema rejection is the interface telling the caller it mistyped, and it is evidence
// about the caller. Counting it beside "the game logged an error" inflates a number a
// person reads as a verdict on the build - and it can flip a reached goal to
// indeterminate on the strength of a misspelt argument name.
bool is_caller_mistake(const String &p_detail) {
	return p_detail.begins_with("unknown argument") ||
			p_detail.begins_with("missing required argument") ||
			p_detail.contains("must be one of") ||
			p_detail.contains("is not a valid") ||
			p_detail.begins_with("unknown tool");
}

bool is_runtime_action_tool(const String &p_tool) {
	return p_tool == "Godot_SetRuntimeProperty" ||
			p_tool == "Godot_SetTimeScale" ||
			p_tool == "Godot_ReplaySession";
}

// The live session. A struct in a function-local static for the same reason the
// activity buffer is: a Variant-family member at namespace scope is constructed before
// the engine's memory subsystem exists and destroyed after it is gone.
struct Session {
	bool running = false;
	String slug;
	String goal;
	String oracle;
	int budget_seconds = 0;
	uint64_t started_msec = 0;
	String started_iso;
	int64_t first_sequence = 0;
	Dictionary context;
	Array observations;
};

Session &session() {
	static Session s;
	return s;
}

double median_of(const Vector<double> &p_values) {
	if (p_values.is_empty()) {
		return 0.0;
	}
	Vector<double> sorted = p_values;
	sorted.sort();
	const int count = sorted.size();
	if (count % 2 == 1) {
		return sorted[count / 2];
	}
	return (sorted[count / 2 - 1] + sorted[count / 2]) * 0.5;
}

} // namespace

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

String MCPPlaytest::verdict_to_string(Verdict p_verdict) {
	switch (p_verdict) {
		case VERDICT_REACHED:
			return "reached";
		case VERDICT_NOT_REACHED:
			return "not_reached";
		case VERDICT_BLOCKED:
			return "blocked";
		case VERDICT_INDETERMINATE:
			return "indeterminate";
		default:
			return "unknown";
	}
}

MCPPlaytest::Verdict MCPPlaytest::verdict_from_string(const String &p_text, bool &r_known) {
	r_known = true;
	const String lowered = p_text.strip_edges().to_lower();
	if (lowered == "reached") {
		return VERDICT_REACHED;
	}
	if (lowered == "not_reached") {
		return VERDICT_NOT_REACHED;
	}
	if (lowered == "blocked") {
		return VERDICT_BLOCKED;
	}
	if (lowered == "indeterminate") {
		return VERDICT_INDETERMINATE;
	}
	if (lowered == "unknown" || lowered.is_empty()) {
		return VERDICT_UNKNOWN;
	}
	r_known = false;
	return VERDICT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Naming and storage
// ---------------------------------------------------------------------------

String MCPPlaytest::slugify(const String &p_goal, String &r_error) {
	String slug;
	const String trimmed = p_goal.strip_edges();
	for (int i = 0; i < trimmed.length(); i++) {
		const char32_t c = trimmed[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			slug += String::chr(c);
		} else if (c >= 'A' && c <= 'Z') {
			slug += String::chr(c + ('a' - 'A'));
		} else if (c == ' ' || c == '.' || c == '/' || c == '\\') {
			// Separators become word breaks rather than vanishing, so two different
			// goals cannot collapse into one directory name.
			if (!slug.is_empty() && !slug.ends_with("-")) {
				slug += "-";
			}
		}
	}
	while (slug.ends_with("-")) {
		slug = slug.substr(0, slug.length() - 1);
	}
	if (slug.is_empty()) {
		r_error = vformat("'%s' has no characters usable in a playtest name; use letters, "
						  "digits, '-' or '_'",
				p_goal);
		return String();
	}
	if (slug.length() > 64) {
		slug = slug.substr(0, 64);
		while (slug.ends_with("-")) {
			slug = slug.substr(0, slug.length() - 1);
		}
	}
	return slug;
}

String &root_override() {
	static String override;
	return override;
}

String MCPPlaytest::get_root() {
	const String &override = root_override();
	if (!override.is_empty()) {
		return override;
	}
	// An absolute path, not a `user://` one. Directories are created through
	// DirAccess::ACCESS_FILESYSTEM, which does not understand Godot's schemes - so a
	// `user://` root here fails to create anything and the failure only shows up at the
	// moment a playtest opens. The session store resolves it the same way.
	return MCPPaths::get_user_root().path_join("godot_ai_playtests");
}

void MCPPlaytest::set_root_override(const String &p_absolute_root) {
	root_override() = p_absolute_root;
}

void MCPPlaytest::clear_root_override() {
	root_override() = String();
}

String MCPPlaytest::get_playtest_dir(const String &p_slug) {
	return get_root().path_join(p_slug);
}

// ---------------------------------------------------------------------------
// Assembly - pure, and the part worth testing
// ---------------------------------------------------------------------------

Array MCPPlaytest::activity_in_window(const Array &p_records, int64_t p_first_sequence,
		int64_t p_last_sequence) {
	Array kept;
	for (int i = 0; i < p_records.size(); i++) {
		const Dictionary record = p_records[i];
		const int64_t sequence = record.get("sequence", 0);
		if (sequence < p_first_sequence) {
			continue;
		}
		if (p_last_sequence > 0 && sequence > p_last_sequence) {
			continue;
		}
		kept.push_back(record);
	}
	return kept;
}

Array MCPPlaytest::input_in_window(const Array &p_activity) {
	Array inputs;
	for (int i = 0; i < p_activity.size(); i++) {
		const Dictionary record = p_activity[i];
		if (is_input_tool(record.get("tool", String()))) {
			inputs.push_back(record);
		}
	}
	return inputs;
}

Array MCPPlaytest::runtime_actions_in_window(const Array &p_activity) {
	Array actions;
	for (int i = 0; i < p_activity.size(); i++) {
		const Dictionary record = p_activity[i];
		if (is_runtime_action_tool(record.get("tool", String()))) {
			actions.push_back(record);
		}
	}
	return actions;
}

Array MCPPlaytest::problems_from_log(const Array &p_messages) {
	Array problems;
	for (int i = 0; i < p_messages.size(); i++) {
		const Dictionary message = p_messages[i];
		// `type` is what Godot_ReadOutputLog calls it and `severity` is what this report
		// calls it. Accepting both means the two can be wired together without either
		// pretending to be the other.
		String severity = String(message.get("severity", String())).to_lower();
		if (severity.is_empty()) {
			severity = String(message.get("type", String())).to_lower();
		}
		const String text = message.get("text", String());
		if (text.strip_edges().is_empty()) {
			continue;
		}
		// Only what a person would call a problem. An informational line in a busy game
		// is noise, and a report full of noise is one nobody reads.
		if (severity != "error" && severity != "warning") {
			continue;
		}
		Dictionary problem;
		problem["severity"] = severity;
		problem["text"] = text;
		if (message.has("frame")) {
			problem["frame"] = message["frame"];
		}
		problems.push_back(problem);
	}
	return problems;
}

Array MCPPlaytest::spikes_from_frame_times(const Array &p_frame_times, double p_multiplier) {
	Array spikes;
	if (p_frame_times.size() < 3 || p_multiplier <= 1.0) {
		// Fewer than three samples has no meaningful middle, and a multiplier of one or
		// less would call every frame a spike.
		return spikes;
	}

	Vector<double> values;
	for (int i = 0; i < p_frame_times.size(); i++) {
		const Dictionary sample = p_frame_times[i];
		values.push_back((double)sample.get("milliseconds", 0.0));
	}
	const double middle = median_of(values);
	if (middle <= 0.0) {
		return spikes;
	}

	for (int i = 0; i < p_frame_times.size(); i++) {
		const Dictionary sample = p_frame_times[i];
		const double milliseconds = sample.get("milliseconds", 0.0);
		if (milliseconds < middle * p_multiplier) {
			continue;
		}
		Dictionary spike;
		spike["frame"] = sample.get("frame", 0);
		spike["milliseconds"] = milliseconds;
		spike["times_median"] = milliseconds / middle;
		spikes.push_back(spike);
	}
	return spikes;
}

MCPPlaytest::Verdict MCPPlaytest::reconcile_verdict(Verdict p_claimed, int p_problem_count,
		int p_input_count, bool p_over_budget, String &r_reason, int p_runtime_action_count) {
	r_reason = String();

	if (p_claimed == VERDICT_REACHED && p_input_count == 0 && p_runtime_action_count == 0) {
		// Nothing was pressed and nothing was written to the running game, so whatever
		// happened, this run did not cause it. This is the check that catches a report
		// written from the source rather than from the game - and it counts both ways of
		// acting, because a game that exposes its verbs as properties is doing what this
		// interface asks of it and must not score zero for it.
		r_reason = "the goal was reported as reached, but this playtest neither injected any "
				   "input nor wrote anything to the running game, so nothing it did can "
				   "account for reaching it";
		return VERDICT_INDETERMINATE;
	}

	if (p_claimed == VERDICT_REACHED && p_problem_count > 0) {
		r_reason = vformat("the goal was reported as reached, but the game logged %d error(s) "
						   "or warning(s) during the run; reaching a goal past an error is not "
						   "the same as reaching it",
				p_problem_count);
		return VERDICT_INDETERMINATE;
	}

	if (p_claimed == VERDICT_NOT_REACHED && p_over_budget) {
		r_reason = "the run hit its budget before finishing, so 'not reached' says only that "
				   "there was not enough time";
		return VERDICT_INDETERMINATE;
	}

	if (p_claimed == VERDICT_UNKNOWN) {
		r_reason = "the playtest finished without stating a verdict";
		return VERDICT_INDETERMINATE;
	}

	return p_claimed;
}

Dictionary MCPPlaytest::build_report(const Dictionary &p_meta, const Array &p_activity,
		const Array &p_inputs, const Array &p_problems, const Array &p_spikes,
		const Array &p_observations, Verdict p_verdict, const String &p_verdict_reason,
		const String &p_summary) {
	Dictionary report = p_meta.duplicate(true);

	report["verdict"] = verdict_to_string(p_verdict);
	if (!p_verdict_reason.is_empty()) {
		report["verdict_reason"] = p_verdict_reason;
	}
	report["summary"] = p_summary;

	// Counts first, because a report is read before it is studied.
	Dictionary counts;
	counts["calls"] = p_activity.size();
	counts["inputs"] = p_inputs.size();
	counts["problems"] = p_problems.size();
	counts["spikes"] = p_spikes.size();
	counts["observations"] = p_observations.size();
	report["counts"] = counts;

	report["inputs"] = p_inputs.duplicate(true);
	report["problems"] = p_problems.duplicate(true);
	report["spikes"] = p_spikes.duplicate(true);
	report["observations"] = p_observations.duplicate(true);

	// The whole activity list is on disk beside the report rather than inside it: a
	// four-minute playtest makes thousands of records and the debugger channel drops
	// anything over 8 MiB, which is how the session store learned the same lesson.
	report["activity_file"] = ACTIVITY_FILE;

	// Says what the reader is looking at, so nothing downstream has to infer it.
	report["evidence_note"] = "The verdict and summary are the agent's conclusion. Everything "
							  "else here was recorded by the editor during the window and is "
							  "not the agent's account of itself.";
	return report;
}

// ---------------------------------------------------------------------------
// The live session
// ---------------------------------------------------------------------------

bool MCPPlaytest::is_running() {
	return session().running;
}

String MCPPlaytest::get_active_slug() {
	return session().running ? session().slug : String();
}

MCPPlaytest::Result MCPPlaytest::begin(const String &p_slug, const String &p_goal,
		int p_budget_seconds, const String &p_oracle, const Dictionary &p_context) {
	if (session().running) {
		// Two overlapping windows would each claim the same activity records, and
		// neither report would be true.
		return Result::bad(vformat("a playtest is already running ('%s'); finish or stop it first",
				session().slug));
	}
	if (p_slug.is_empty()) {
		return Result::bad("a playtest needs a name");
	}
	if (p_goal.strip_edges().is_empty()) {
		return Result::bad("a playtest needs a goal stated in prose; without one there is "
						   "nothing for the verdict to be about");
	}

	const String dir = get_playtest_dir(p_slug);
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_null()) {
		return Result::bad("could not reach the filesystem to create the playtest");
	}
	if (access->make_dir_recursive(dir) != OK) {
		return Result::bad(vformat("could not create '%s'", dir));
	}
	// Truncate a previous run under the same name: half an old activity log mixed into a
	// new one is worse than either.
	const String activity_path = dir.path_join(ACTIVITY_FILE);
	if (FileAccess::exists(activity_path)) {
		Ref<FileAccess> truncate = FileAccess::open(activity_path, FileAccess::WRITE);
		if (truncate.is_null()) {
			return Result::bad("could not clear the previous activity log");
		}
	}

	Session &live = session();
	live.running = true;
	live.slug = p_slug;
	live.goal = p_goal;
	live.oracle = p_oracle;
	live.budget_seconds = MAX(0, p_budget_seconds);
	live.started_msec = OS::get_singleton()->get_ticks_msec();
	live.started_iso = Time::get_singleton()->get_datetime_string_from_system(true);
	live.context = p_context.duplicate(true);
	live.observations = Array();
	// Everything from here on belongs to this playtest. Recorded before the caller can
	// make another call, so the window has no gap at its start.
	live.first_sequence = MCPActivity::get_latest_sequence() + 1;

	return Result::good();
}

MCPPlaytest::Result MCPPlaytest::observe(const String &p_note, const String &p_kind) {
	if (!session().running) {
		return Result::bad("no playtest is running");
	}
	if (p_note.strip_edges().is_empty()) {
		return Result::bad("an observation needs something to say");
	}
	Dictionary observation;
	observation["note"] = p_note;
	observation["kind"] = p_kind.strip_edges().is_empty() ? String("note") : p_kind;
	observation["at_second"] = get_elapsed_seconds();
	observation["sequence"] = MCPActivity::get_latest_sequence();
	session().observations.push_back(observation);
	return Result::good();
}

int MCPPlaytest::get_elapsed_seconds() {
	if (!session().running) {
		return 0;
	}
	return (int)((OS::get_singleton()->get_ticks_msec() - session().started_msec) / 1000);
}

bool MCPPlaytest::is_over_budget() {
	const Session &live = session();
	return live.running && live.budget_seconds > 0 && get_elapsed_seconds() > live.budget_seconds;
}

MCPPlaytest::Result MCPPlaytest::finish(Verdict p_verdict, const String &p_summary,
		Dictionary &r_report, const Array &p_frame_times) {
	if (!session().running) {
		return Result::bad("no playtest is running");
	}

	Session &live = session();
	const int64_t last_sequence = MCPActivity::get_latest_sequence();
	const Array all = MCPActivity::snapshot(live.first_sequence - 1, MCPActivity::DEFAULT_CAPACITY);
	const Array activity = activity_in_window(all, live.first_sequence, last_sequence);
	const Array inputs = input_in_window(activity);

	// Problems are read from the activity stream's own failures here; the tool layer
	// adds the game's output log, which this layer cannot reach.
	// Two different things used to be pooled into one count that a human reads as
	// evidence about the build: the game misbehaving, and the agent typing the wrong
	// argument name. A schema rejection says nothing whatever about the game, and a
	// report whose `problems` list is padded with the agent's own typos overstates what
	// it found. They are separated here and both are still shown.
	Array problems;
	Array caller_mistakes;
	for (int i = 0; i < activity.size(); i++) {
		const Dictionary record = activity[i];
		const String outcome = record.get("outcome", String());
		if (outcome != "failed" && outcome != "refused") {
			continue;
		}
		Dictionary entry;
		entry["severity"] = "error";
		entry["text"] = vformat("%s %s: %s", String(record.get("tool", String())), outcome,
				String(record.get("detail", String())));
		if (is_caller_mistake(record.get("detail", String()))) {
			caller_mistakes.push_back(entry);
		} else {
			problems.push_back(entry);
		}
	}

	const bool over_budget = live.budget_seconds > 0 && get_elapsed_seconds() > live.budget_seconds;
	String reason;
	const Array runtime_actions = runtime_actions_in_window(activity);
	const Verdict verdict = reconcile_verdict(p_verdict, problems.size(), inputs.size(),
			over_budget, reason, runtime_actions.size());

	Dictionary meta;
	meta["slug"] = live.slug;
	meta["goal"] = live.goal;
	meta["oracle"] = live.oracle;
	meta["budget_seconds"] = live.budget_seconds;
	meta["elapsed_seconds"] = get_elapsed_seconds();
	meta["over_budget"] = over_budget;
	if (!caller_mistakes.is_empty()) {
		// Shown, because hiding them would let an agent quietly fail half its calls, but
		// kept out of `problems` because they are not about the game.
		meta["caller_mistakes"] = caller_mistakes;
		meta["caller_mistake_count"] = caller_mistakes.size();
	}
	meta["started"] = live.started_iso;
	meta["finished"] = Time::get_singleton()->get_datetime_string_from_system(true);
	meta["first_sequence"] = live.first_sequence;
	meta["last_sequence"] = last_sequence;
	meta["context"] = live.context.duplicate(true);
	meta["claimed_verdict"] = verdict_to_string(p_verdict);

	// The multiplier is a judgement about what counts as a stutter rather than slowness.
	// Three times the median frame is a frame a player feels; twice is a frame a graph
	// shows. Kept here rather than made an argument, so two reports of the same run cannot
	// disagree about what a spike is.
	const Array spikes = spikes_from_frame_times(p_frame_times, 3.0);
	r_report = build_report(meta, activity, inputs, problems, spikes, live.observations,
			verdict, reason, p_summary);
	Dictionary frame_coverage;
	frame_coverage["samples"] = p_frame_times.size();
	// Said out loud, because "no spikes" and "nobody was measuring" look identical in a
	// report and mean completely different things.
	frame_coverage["measured"] = p_frame_times.size() >= 3;
	if (p_frame_times.size() < 3) {
		frame_coverage["note"] = "frame times were not measured over this window, so an empty "
								 "spike list means nothing was looking rather than that nothing "
								 "spiked";
	}
	r_report["frame_coverage"] = frame_coverage;

	const Result written = _write(live.slug, r_report, activity);
	live.running = false;
	return written;
}

MCPPlaytest::Result MCPPlaytest::abandon(const String &p_reason, Dictionary &r_report,
		const Array &p_frame_times) {
	if (!session().running) {
		return Result::bad("no playtest is running");
	}
	const Result result = finish(VERDICT_INDETERMINATE,
			vformat("Stopped before finishing: %s", p_reason.strip_edges().is_empty()
							? String("no reason given")
							: p_reason),
			r_report, p_frame_times);
	if (result.ok) {
		r_report["partial"] = true;
	}
	return result;
}

MCPPlaytest::Result MCPPlaytest::_write(const String &p_slug, const Dictionary &p_report,
		const Array &p_activity) {
	const String dir = get_playtest_dir(p_slug);
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_valid()) {
		access->make_dir_recursive(dir);
	}

	Ref<FileAccess> report_file = FileAccess::open(dir.path_join(REPORT_FILE), FileAccess::WRITE);
	if (report_file.is_null()) {
		return Result::bad(vformat("could not write the report in '%s'", dir));
	}
	report_file->store_string(JSON::stringify(p_report, "  "));

	Ref<FileAccess> activity_file = FileAccess::open(dir.path_join(ACTIVITY_FILE), FileAccess::WRITE);
	if (activity_file.is_null()) {
		return Result::bad(vformat("could not write the activity log in '%s'", dir));
	}
	for (int i = 0; i < p_activity.size(); i++) {
		activity_file->store_line(JSON::stringify(p_activity[i]));
	}
	return Result::good();
}

Dictionary MCPPlaytest::get_report(const String &p_slug, String &r_error) {
	const String path = get_playtest_dir(p_slug).path_join(REPORT_FILE);
	if (!FileAccess::exists(path)) {
		r_error = vformat("no playtest report named '%s'", p_slug);
		return Dictionary();
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(path));
	if (parsed.get_type() != Variant::DICTIONARY) {
		r_error = vformat("the report for '%s' is not readable", p_slug);
		return Dictionary();
	}
	return parsed;
}

Array MCPPlaytest::list() {
	Array reports;
	Ref<DirAccess> dir = DirAccess::open(get_root());
	if (dir.is_null()) {
		return reports;
	}
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (dir->current_is_dir() && entry != "." && entry != "..") {
			String error;
			const Dictionary report = get_report(entry, error);
			if (!report.is_empty()) {
				Dictionary row;
				row["slug"] = entry;
				row["goal"] = report.get("goal", String());
				row["verdict"] = report.get("verdict", String());
				row["finished"] = report.get("finished", String());
				row["counts"] = report.get("counts", Dictionary());
				reports.push_back(row);
			}
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
	return reports;
}

void MCPPlaytest::reset_for_tests() {
	Session &live = session();
	live.running = false;
	live.slug = String();
	live.goal = String();
	live.oracle = String();
	live.budget_seconds = 0;
	live.started_msec = 0;
	live.started_iso = String();
	live.first_sequence = 0;
	live.context = Dictionary();
	live.observations = Array();
}
