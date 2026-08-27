/**************************************************************************/
/*  mcp_variant_tools.cpp                                                 */
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

// The live tuning workspace.
//
// One tool with actions rather than five tools, deliberately. The tool surface is itself
// a reliability risk - more similarly-named primitives means more chances to pick the
// wrong one - and `offer`, `switch`, `note`, `keep`, `discard` and `status` are six
// moments in one gesture, not six capabilities.
//
// The rules that make it worth having, all in `mcp_variants.{h,cpp}` where they are
// testable without a game:
//
//   * The original is captured first and is always a candidate. Comparing against what
//     the game already had is the comparison a designer forgets to make.
//   * A candidate that was never live cannot be kept. Keeping a value nobody played is
//     editing the scene by a longer route, and calling it the result of a comparison
//     would be a claim about something that did not happen.
//   * Discarding puts the original back, in the running game. Otherwise "discard" leaves
//     the last thing you tried in place and looks like it worked.
//   * How long each value was live is measured and reported. Flipping through five
//     numbers in a second is not tuning, and the reply says so in those words.

#include "mcp_builtin_tools.h"

#include "../mcp_deferred.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_tool_registry.h"
#include "../mcp_variants.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"

namespace {

class OfferVariantsTool : public MCPTool {
	MCPVariants set;
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	String pending_action;
	String pending_name;
	String pending_note;
	Array pending_candidates;
	String pending_runtime_path;
	String pending_property;
	bool running = false;

	static int64_t now_msec() { return (int64_t)OS::get_singleton()->get_ticks_msec(); }

	void _fail(MCPToolError::Kind p_kind, const String &p_message) {
		running = false;
		if (token != MCPDeferred::INVALID_TOKEN) {
			MCPDeferred::fail(token, p_kind, p_message);
			token = MCPDeferred::INVALID_TOKEN;
		}
	}

	void _complete(const Dictionary &p_result) {
		running = false;
		if (token != MCPDeferred::INVALID_TOKEN) {
			MCPDeferred::complete(token, p_result);
			token = MCPDeferred::INVALID_TOKEN;
		}
	}

	Dictionary _status_result() const {
		Dictionary result = set.summary();
		result["tuning"] = set.is_open();
		return result;
	}

	// Applies a candidate to the running game. The set's clock only moves once the game
	// confirms: a value the game refused was never live, and counting the time it spent
	// being refused would be the one number here nobody could check.
	bool _apply(const String &p_name, const Callable &p_on_reply) {
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			return false;
		}
		bool found = false;
		const Variant value = set.value_for(p_name, found);
		if (!found) {
			return false;
		}
		Dictionary arguments;
		arguments["path"] = set.get_runtime_path();
		arguments["property"] = set.get_property();
		arguments["value"] = value;
		return bridge->request("set_property", arguments, 10.0, p_on_reply);
	}

	// --- offer -----------------------------------------------------------------

	void _on_original_read(bool p_ok, const Dictionary &p_payload) {
		if (!running) {
			return;
		}
		if (!p_ok) {
			_fail(MCPToolError::NOT_FOUND,
					vformat("could not read '%s.%s' from the running game: %s", pending_runtime_path,
							pending_property, String(p_payload.get("error", "no reason given"))));
			return;
		}
		// The original is kept as the game's own *text* rather than as the JSON value.
		// JSON has no Vector2 and no Color: `get_property` sends both, and the JSON half of
		// a position arrives as the string "(128, 64)", which is not a position and cannot
		// be written back as one. The text form round-trips every type, and `set_property`
		// parses it - so what discard puts back is exactly what was read out.
		const String original_text = p_payload.get("text", String());
		const Variant original_value =
				original_text.is_empty() ? p_payload.get("value", Variant()) : Variant(original_text);

		String error;
		if (!set.open_set(pending_runtime_path, pending_property, original_value, original_text,
					pending_candidates, error)) {
			_fail(MCPToolError::INVALID_ARGUMENTS, error);
			return;
		}
		// Opened on the original, which is where the game already is - so nothing has been
		// changed yet and there is nothing to put back if the caller stops here.
		Dictionary result = _status_result();
		result["opened"] = true;
		result["next"] = "Switch to a candidate to put it in the running game, play for long "
						 "enough to judge it, and note what you saw. Then keep the one that "
						 "felt right, or discard the set to put the original back.";
		_complete(result);
	}

	// --- switch ----------------------------------------------------------------

	void _on_switched(bool p_ok, const Dictionary &p_payload) {
		if (!running) {
			return;
		}
		if (!p_ok) {
			_fail(MCPToolError::FAILED,
					vformat("the running game did not take '%s': %s", pending_name,
							String(p_payload.get("error", p_payload.get("message", "no reason given")))));
			return;
		}
		String error;
		if (!set.switch_to(pending_name, now_msec(), error)) {
			_fail(MCPToolError::INVALID_ARGUMENTS, error);
			return;
		}
		Dictionary result = _status_result();
		result["applied"] = pending_name;
		result["text"] = p_payload.get("text", set.text_for(pending_name));
		_complete(result);
	}

	// --- discard ---------------------------------------------------------------

	void _on_restored(bool p_ok, const Dictionary &p_payload) {
		if (!running) {
			return;
		}
		set.close(now_msec());
		Dictionary result = _status_result();
		result["discarded"] = true;
		result["restored"] = p_ok;
		if (p_ok) {
			result["text"] = p_payload.get("text", set.get_original_text());
		} else {
			// Said rather than swallowed: the caller asked to put things back and they are
			// not back, so the running game is holding whatever was last applied.
			result["warning"] = vformat(
					"the set was closed but the running game did not take the original value "
					"back (%s); it is still holding '%s'",
					String(p_payload.get("error", p_payload.get("message", "no reason given"))),
					set.get_current());
		}
		set.reset();
		_complete(result);
	}

public:
	virtual String get_tool_name() const override { return "Godot_OfferVariants"; }
	virtual String get_description() const override {
		return "Try several values for one property against the running game and keep the one "
			   "that felt right. This is the live tuning workspace: 'offer' captures the current "
			   "value and takes a list of candidates, 'switch' puts one into the running game so "
			   "you can play it, 'note' records what you saw, 'keep' names the winner, and "
			   "'discard' puts the original back. It measures how long each value was actually "
			   "live and says so, because flipping through five numbers in a second is not "
			   "tuning. 'keep' hands you the value to promote - call Godot_PromoteRuntimeValue "
			   "to write it into the edited scene. Needs a running game.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> actions;
		actions.push_back("offer");
		actions.push_back("switch");
		actions.push_back("note");
		actions.push_back("keep");
		actions.push_back("discard");
		actions.push_back("status");
		properties["action"] = MCPSchema::enum_property(
				"'offer' opens a set, 'switch' makes a candidate live, 'note' records an "
				"observation about the live one, 'keep' chooses a winner and closes the set, "
				"'discard' closes it and puts the original back, 'status' reports without "
				"changing anything.",
				actions);
		properties["path"] = MCPSchema::string_property(
				"Node path in the running game, such as /root/Main/Player. Required to offer.");
		properties["property"] = MCPSchema::string_property(
				"Property to tune, such as 'jump_height'. Required to offer.");
		properties["values"] = MCPSchema::array_property(
				"The candidates. Each is either a plain value or {name, value}; an unnamed one "
				"is called after its own value. At least two are required - one value is not a "
				"choice, and Godot_SetRuntimeProperty already sets one value.",
				MCPSchema::any_property("A candidate value, or {name, value}."));
		properties["name"] = MCPSchema::string_property(
				"Which candidate. 'original' is always available and is the value the property "
				"held before any of this. Required to switch and to keep.");
		properties["note"] = MCPSchema::string_property(
				"What you observed. Recorded against the live candidate unless 'name' says "
				"otherwise.");
		Vector<String> required;
		required.push_back("action");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["tuning"] = MCPSchema::bool_property("True while a set is open.");
		properties["runtime_path"] = MCPSchema::string_property("Node being tuned.");
		properties["property"] = MCPSchema::string_property("Property being tuned.");
		properties["current"] = MCPSchema::string_property("Which candidate is live now.");
		properties["original"] = MCPSchema::string_property("The value the property started with.");
		properties["candidates"] = MCPSchema::array_property(
				"Each candidate with how many times it was applied, how long it was live, and "
				"any notes.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["comparison"] = MCPSchema::string_property(
				"How much of a comparison this actually was. Never flattering.");
		properties["kept"] = MCPSchema::string_property("The candidate chosen, on 'keep'.");
		properties["value"] = MCPSchema::any_property("The chosen value, on 'keep'.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (running) {
			r_error.set(MCPToolError::INVALID_STATE, "the previous call has not answered yet");
			return Dictionary();
		}
		// get() rather than operator[]: a missing key read through a const Dictionary
		// inserts a null, which schema validation then rejects as wrongly typed.
		const String action = p_arguments.get("action", String());

		if (action == "status") {
			return _status_result();
		}

		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		const bool reachable = bridge && bridge->is_game_reachable();

		if (action == "offer") {
			if (set.is_open()) {
				r_error.set(MCPToolError::INVALID_STATE,
						vformat("already tuning '%s.%s'; keep one or discard the set first",
								set.get_runtime_path(), set.get_property()));
				return Dictionary();
			}
			if (!reachable) {
				r_error.set(MCPToolError::INVALID_STATE,
						"no game is running. Tuning is something you do to a game you can see, "
						"so start one with Godot_PlayMainScene first");
				return Dictionary();
			}
			pending_runtime_path = p_arguments.get("path", String());
			pending_property = p_arguments.get("property", String());
			pending_candidates = p_arguments.get("values", Array());
			if (pending_runtime_path.is_empty() || pending_property.is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS,
						"offering variants needs a 'path' and a 'property'");
				return Dictionary();
			}
			if (pending_candidates.size() < 2) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS,
						"a tuning set needs at least two values; with one there is nothing to "
						"choose between, so use Godot_SetRuntimeProperty instead");
				return Dictionary();
			}

			// The original is read before anything is changed. Reading it afterwards would
			// capture a candidate and call it the original, and discarding would then
			// "restore" a value the game never started with.
			Dictionary arguments;
			arguments["path"] = pending_runtime_path;
			arguments["property"] = pending_property;
			running = true;
			token = MCPDeferred::begin(15.0, "the running game did not report the current value");
			if (!bridge->request("get_property", arguments, 12.0,
						callable_mp(this, &OfferVariantsTool::_on_original_read))) {
				running = false;
				MCPDeferred::abandon(token);
				token = MCPDeferred::INVALID_TOKEN;
				r_error.set(MCPToolError::FAILED, "the running game did not accept the request");
				return Dictionary();
			}
			return MCPDeferred::make_deferred_result(token);
		}

		if (!set.is_open()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no tuning set is open; call this with action='offer' first");
			return Dictionary();
		}

		if (action == "note") {
			const String note = p_arguments.get("note", String());
			if (note.strip_edges().is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, "a note needs some text");
				return Dictionary();
			}
			String error;
			if (!set.add_note(p_arguments.get("name", String()), note, error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
			}
			return _status_result();
		}

		if (action == "switch") {
			pending_name = p_arguments.get("name", String());
			if (pending_name.is_empty()) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, "switching needs a candidate 'name'");
				return Dictionary();
			}
			bool found = false;
			set.value_for(pending_name, found);
			if (!found) {
				r_error.set(MCPToolError::NOT_FOUND,
						vformat("no candidate called '%s' in this set", pending_name));
				return Dictionary();
			}
			if (!reachable) {
				r_error.set(MCPToolError::INVALID_STATE,
						"the game has stopped, so there is nothing to switch. The set is still "
						"open; discard it, or start the game again and switch then");
				return Dictionary();
			}
			running = true;
			token = MCPDeferred::begin(15.0, "the running game did not take the value in time");
			if (!_apply(pending_name, callable_mp(this, &OfferVariantsTool::_on_switched))) {
				running = false;
				MCPDeferred::abandon(token);
				token = MCPDeferred::INVALID_TOKEN;
				r_error.set(MCPToolError::FAILED, "the running game did not accept the request");
				return Dictionary();
			}
			return MCPDeferred::make_deferred_result(token);
		}

		if (action == "keep") {
			const String name = p_arguments.get("name", String());
			String error;
			if (!set.can_keep(name, error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
			}
			set.close(now_msec());

			bool found = false;
			const Variant value = set.value_for(name, found);
			Dictionary result = _status_result();
			result["kept"] = name;
			result["value"] = value;
			result["text"] = set.text_for(name);
			result["tuning"] = false;
			// Deliberately does not write the scene itself. Promotion is `edit_scene` and
			// this tool is `read_runtime`; a tool declares one capability, and one that
			// quietly held both would be holding more authority than it declares. The same
			// reasoning as DEC-0010 for replay.
			result["next"] = vformat(
					"Nothing has been written to the project yet. Call Godot_PromoteRuntimeValue "
					"with path='%s' and property='%s' to make '%s' the authored value, while the "
					"game is still running and still holding it.",
					set.get_runtime_path(), set.get_property(), name);
			if (set.get_current() != name) {
				// The winner is not what the game is currently holding, so promoting now
				// would promote the wrong value. Say which, rather than let a promotion
				// quietly keep the last thing that was tried.
				result["warning"] = vformat(
						"the running game is currently holding '%s', not '%s'. Switch to '%s' "
						"before promoting, or the authored value will be the wrong one.",
						set.get_current(), name, name);
			}
			set.reset();
			return result;
		}

		if (action == "discard") {
			if (!reachable) {
				// Nothing to restore into. Closing the set is still the right answer - the
				// caller asked to stop tuning - but it must not report a restore it did not
				// perform.
				set.close(now_msec());
				Dictionary result = _status_result();
				result["discarded"] = true;
				result["restored"] = false;
				result["warning"] = "the game had already stopped, so there was nothing to put "
									"the original value back into. Nothing was written to the "
									"project either way.";
				set.reset();
				return result;
			}
			running = true;
			token = MCPDeferred::begin(15.0, "the running game did not take the original value back");
			if (!_apply(MCPVariants::ORIGINAL_NAME,
						callable_mp(this, &OfferVariantsTool::_on_restored))) {
				running = false;
				MCPDeferred::abandon(token);
				token = MCPDeferred::INVALID_TOKEN;
				r_error.set(MCPToolError::FAILED, "the running game did not accept the request");
				return Dictionary();
			}
			return MCPDeferred::make_deferred_result(token);
		}

		r_error.set(MCPToolError::INVALID_ARGUMENTS,
				vformat("unknown action '%s'; expected offer, switch, note, keep, discard or "
						"status",
						action));
		return Dictionary();
	}
};

} // namespace

void mcp_register_variant_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(OfferVariantsTool)));
}
