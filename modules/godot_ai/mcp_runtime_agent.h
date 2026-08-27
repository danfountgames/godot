/**************************************************************************/
/*  mcp_runtime_agent.h                                                   */
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

// The half of the agent interface that lives inside the *running game*.
//
// Everything else in this module runs in the editor. But "play" launches a second
// process, and the questions that matter most - did that click reach the button, what
// does the player actually see, is the frame budget being met - can only be answered
// from inside it. The editor already talks to that process over the remote debugger;
// this registers a capture handler on the other end of that same channel.
//
// Two properties are deliberate:
//
//   * **Input is injected through `Input::parse_input_event()`**, the same entry point
//     the platform layer uses for real hardware. Not by calling a control's callback,
//     not by emitting `pressed`. A test that cannot tell the difference is not
//     evidence, so this must not be the kind of implementation that makes them
//     indistinguishable.
//   * **It is compiled under `TOOLS_ENABLED` and installs itself only when the process
//     is running as a game.** A game launched from the editor is the same executable,
//     so it is there; an exported game contains none of it.

#ifndef MCP_RUNTIME_AGENT_H
#define MCP_RUNTIME_AGENT_H

#ifdef TOOLS_ENABLED

#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/input/input_event.h"
#include "core/variant/variant.h"

// The debugger message namespace shared by both ends of the channel. Editor-side
// requests arrive as `godot_ai:<command>`; replies go back as `godot_ai:reply`.
#define MCP_RUNTIME_CHANNEL "godot_ai"

// Waits that resolve when the game reaches a state, checked every frame.
//
// This exists so an agent never has to sleep. "Wait 2 seconds and hope" is the single
// most common source of tests that pass on a fast machine and fail on a loaded one;
// "wait until the score is 10, or fail after 5 seconds" is a statement about the game.
class MCPRuntimeWatcher : public Object {
	GDCLASS(MCPRuntimeWatcher, Object);

	// How often a still-running gesture tells the editor it is still running. Small
	// enough that even a 1 fps game beats the shortest runtime deadline, large enough
	// that a normal gesture at 60 fps usually finishes without sending one at all.
	static constexpr int HEARTBEAT_FRAMES = 4;

	struct Watch {
		String request_id;
		String path;
		String property;
		Variant expected;
		double deadline = 0.0;
	};

	// Capturing a sequence and measuring a window of frames are the same shape as a
	// watch: both need to be there every frame, and both answer once.
	struct Sequence {
		String request_id;
		int remaining = 0;
		int interval_frames = 1;
		int countdown = 0;
		Array paths;
	};

	struct Profile {
		String request_id;
		int remaining = 0;
		double worst_frame_ms = 0.0;
		double total_ms = 0.0;
		int samples = 0;
		double budget_frame_ms = 0.0;
	};

	// A test scene decides when it has finished, so the only way to know is to look
	// every frame. The scene's contract is two values on its root node - `test_finished`
	// and `test_results` - readable as either script properties or node metadata,
	// because a scene without a script can only carry metadata.
	struct SceneTest {
		String request_id;
		double deadline = 0.0;
	};

	// Stacking is a thing that happens *during* a burst of input, not a thing that is
	// true when someone happens to ask. A sound triggered twice on the same frame may
	// have finished before the next call arrives, so the only way to catch it is to
	// watch every frame and remember the worst.
	struct AudioWindow {
		String request_id;
		int remaining = 0;
		int sampled = 0;
		int max_simultaneous = 0;
		HashMap<String, int> peak;
		HashMap<String, int> peak_frame;
		HashMap<String, Array> peak_players;
	};

	// A gesture is more than one event, and **the frames between them are part of it.**
	//
	// Delivering press+release, or a drag's whole sweep, inside one frame is not a faster
	// version of the gesture - it is a different one, and often one no hardware can
	// produce. A game polling `Input.is_action_just_pressed()` in `_process` never sees a
	// press and release that share a frame. A snap target with per-frame hysteresis never
	// gets a chance to claim a piece being dragged past it, so the piece returns home and
	// the route looks broken. A gesture threshold measured per frame never trips.
	//
	// So every multi-event gesture is queued here and delivered one event per frame, and
	// the tool call answers when the last one has gone.
	struct Gesture {
		String request_id;
		Vector<Ref<InputEvent>> events;
		int next = 0;
		int frames_per_step = 1;
		int countdown = 0;
		int first_frame = 0;
		// Frames since the editor was last told this gesture is still going. A gesture
		// is deliberately measured in frames, so on a slow game it can outlast a
		// seconds-based deadline while working exactly as designed.
		int since_heartbeat = 0;
		Dictionary result;
	};

	Vector<Watch> watches;
	Vector<Sequence> sequences;
	Vector<Profile> profiles;
	Vector<SceneTest> scene_tests;
	Vector<AudioWindow> audio_windows;
	Vector<Gesture> gestures;

	static MCPRuntimeWatcher *singleton;

public:
	static MCPRuntimeWatcher *get_singleton() { return singleton; }
	static void create();
	static void destroy();

	// Reads a test scene's contract off the root node. False when the scene declares
	// nothing, which is how "that is not a test scene" is told apart from "it has not
	// finished yet".
	static bool read_scene_test(bool &r_finished, Array &r_cases);

	void add(const String &p_request_id, const String &p_path, const String &p_property, const Variant &p_expected, double p_timeout_seconds);
	void add_sequence(const String &p_request_id, int p_frames, int p_interval_frames);
	void add_profile(const String &p_request_id, int p_frames, double p_budget_frame_ms);
	void add_scene_test(const String &p_request_id, double p_timeout_seconds);
	void add_audio_window(const String &p_request_id, int p_frames);
	// Queues a gesture for frame-paced delivery. `p_result` is echoed back when the last
	// event has been delivered, with the frames it spanned added.
	void add_gesture(const String &p_request_id, const Vector<Ref<InputEvent>> &p_events,
			const Dictionary &p_result, int p_frames_per_step);
	void on_frame();
};

class MCPRuntimeAgent {
	static bool installed;

	// Requests carry an id so a reply can be matched to the call that caused it; the
	// editor may have several in flight.
	static void _reply(const String &p_request_id, const Dictionary &p_result);
	static void _fail(const String &p_request_id, const String &p_message);

	static Dictionary _handle(const String &p_command, const Dictionary &p_arguments, String &r_error);

	// Commands.
	static Dictionary _ping(const Dictionary &p_arguments, String &r_error);
	static Dictionary _send_pointer(const Dictionary &p_arguments, String &r_error);
	static Dictionary _send_key(const Dictionary &p_arguments, String &r_error);
	static Dictionary _capture(const Dictionary &p_arguments, String &r_error);
	static Dictionary _get_property(const Dictionary &p_arguments, String &r_error);
	static Dictionary _set_property(const Dictionary &p_arguments, String &r_error);
	static Dictionary _node_info(const Dictionary &p_arguments, String &r_error);
	static Dictionary _performance(const Dictionary &p_arguments, String &r_error);
	static Dictionary _window_info(const Dictionary &p_arguments, String &r_error);
	static Dictionary _send_touch(const Dictionary &p_arguments, String &r_error);
	static Dictionary _send_gamepad(const Dictionary &p_arguments, String &r_error);
	static Dictionary _send_action(const Dictionary &p_arguments, String &r_error);
	static Dictionary _input_trace(const Dictionary &p_arguments, String &r_error);
	static Dictionary _runtime_errors(const Dictionary &p_arguments, String &r_error);
	static Dictionary _resize_window(const Dictionary &p_arguments, String &r_error);
	static Dictionary _time_scale(const Dictionary &p_arguments, String &r_error);
	static Dictionary _audio_state(const Dictionary &p_arguments, String &r_error);

	// Every injected event is recorded, so "a click was sent" and "a click arrived" can
	// be told apart after the fact rather than argued about. Returns the frame it stamped,
	// which every input reply echoes back: the editor's own mirror of the trace
	// (`mcp_bug_capture.h`) has no other way to learn where an event landed, and a mirror
	// with no frames cannot be replayed after the game process is gone.
	static int64_t _record(const String &p_kind, const Dictionary &p_detail);



public:
	// Installs the capture handler. Safe to call more than once; does nothing in an
	// editor process, and nothing when there is no debugger connection.
	static void install();
	static void uninstall();

	// The engine's own capture entry point.
	static Error parse_message(void *p_user, const String &p_message, const Array &p_args, bool &r_captured);

	// Turns a value that arrived as JSON into the type the property actually holds.
	// Returns false when no honest conversion exists.
	static bool coerce(const Variant &p_value, Variant::Type p_target, Variant &r_out, String &r_error);

	// Saves one frame of the running game and returns its path, or an empty string.
	static String capture_frame(String &r_error);

	// Used by the watcher to answer a wait it started.
	static void reply(const String &p_request_id, const Dictionary &p_result) { _reply(p_request_id, p_result); }
	static void fail(const String &p_request_id, const String &p_message) { _fail(p_request_id, p_message); }
};

#endif // TOOLS_ENABLED

#endif // MCP_RUNTIME_AGENT_H
