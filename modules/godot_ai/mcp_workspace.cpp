/**************************************************************************/
/*  mcp_workspace.cpp                                                     */
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

#include "mcp_workspace.h"

#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"

#include "editor/debugger/editor_debugger_node.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/run/editor_run.h"
#include "editor/run/editor_run_bar.h"
#include "editor/settings/editor_settings.h"
#include "editor/run/embedded_process.h"
#include "scene/gui/separator.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"

// ----------------------------------------------------------------------- tile ---

MCPWorkspaceTile::MCPWorkspaceTile(const String &p_instance_id) {
	instance_id = p_instance_id;
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	VBoxContainer *column = memnew(VBoxContainer);
	add_child(column);

	HBoxContainer *header = memnew(HBoxContainer);
	column->add_child(header);

	title = memnew(Label);
	title->set_h_size_flags(SIZE_EXPAND_FILL);
	title->set_theme_type_variation("HeaderSmall");
	header->add_child(title);

	control_button = memnew(Button);
	control_button->set_tooltip_text(TTR("Take control of this instance, or give it back to the agent."));
	control_button->connect(SceneStringName(pressed), callable_mp(this, &MCPWorkspaceTile::_toggle_control));
	header->add_child(control_button);

	pause_button = memnew(Button);
	pause_button->set_tooltip_text(TTR("Pause only this instance. The others keep running."));
	pause_button->connect(SceneStringName(pressed), callable_mp(this, &MCPWorkspaceTile::_toggle_pause));
	header->add_child(pause_button);

	stop_button = memnew(Button);
	stop_button->set_text(TTR("Stop"));
	stop_button->set_tooltip_text(TTR("Stop only this instance."));
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &MCPWorkspaceTile::_stop));
	header->add_child(stop_button);

	status = memnew(Label);
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	column->add_child(status);

	// One embedder per tile. This is the whole point: the platform layer keys embedding
	// by process id, so N of these means N embedded games.
	embedder = memnew(EmbeddedProcess);
	embedder->set_h_size_flags(SIZE_EXPAND_FILL);
	embedder->set_v_size_flags(SIZE_EXPAND_FILL);
	// Fit the game to this tile rather than leaving it at its launched resolution. An
	// embedded game is a native child window drawn above the editor's own controls, so a
	// game larger than its tile does not clip - it covers whatever is beside it,
	// including the neighbouring tiles' headers and buttons. An empty size means
	// "follow the frame".
	embedder->set_window_size(Size2i());
	embedder->set_keep_aspect(false);
	column->add_child(embedder);

	refresh();
}

Rect2i MCPWorkspaceTile::get_embed_rect() const {
	if (!embedder) {
		return Rect2i();
	}
	const Rect2i rect = embedder->get_screen_embedded_window_rect();
	if (rect.size.x > 0 && rect.size.y > 0) {
		return rect;
	}
	// Before the first layout pass the control has no usable rect. Fall back to the
	// tile's own, so a game launched immediately after the workspace opens still lands
	// somewhere sane rather than at 0x0.
	Rect2i fallback = Rect2i(get_global_position(), get_size());
	if (fallback.size.x < 64 || fallback.size.y < 64) {
		fallback.size = Size2i(640, 360);
	}
	return fallback;
}

void MCPWorkspaceTile::embed(ProcessID p_pid) {
	if (!embedder || p_pid == 0) {
		return;
	}
	MCPRuntimeInstances::set_lifecycle(instance_id, MCPRuntimeInstances::LIFECYCLE_EMBEDDING);
	embedder->embed_process(p_pid);
	MCPRuntimeInstances::set_lifecycle(instance_id, MCPRuntimeInstances::LIFECYCLE_RUNNING);
	refresh();
}

void MCPWorkspaceTile::_toggle_control() {
	MCPRuntimeInstances::Instance instance;
	if (!MCPRuntimeInstances::get(instance_id, instance)) {
		return;
	}
	// Ownership is recorded, never inferred. An instance the human took over must stop
	// receiving injected input, and the only way to be sure is to write it down.
	const MCPRuntimeInstances::Control next =
			instance.control == MCPRuntimeInstances::CONTROL_HUMAN
			? MCPRuntimeInstances::CONTROL_AGENT
			: MCPRuntimeInstances::CONTROL_HUMAN;
	MCPRuntimeInstances::set_control(instance_id, next);
	if (next == MCPRuntimeInstances::CONTROL_HUMAN && embedder) {
		embedder->grab_focus();
	}
	refresh();
}

void MCPWorkspaceTile::_toggle_pause() {
	suspended = !suspended;
	// Targeted, not broadcast. The Game workspace's own pause reaches every active
	// session; this one reaches exactly this instance.
	if (!MCPRuntimeInstances::set_suspended(instance_id, suspended)) {
		suspended = !suspended;
		if (status) {
			status->set_text(TTR("Could not reach this instance to pause it."));
		}
		return;
	}
	refresh();
}

void MCPWorkspaceTile::_stop() {
	MCPWorkspaceLauncher::stop(instance_id);
	refresh();
}

void MCPWorkspaceTile::refresh() {
	MCPRuntimeInstances::Instance instance;
	if (!MCPRuntimeInstances::get(instance_id, instance)) {
		return;
	}
	if (title) {
		title->set_text(instance.label);
	}
	if (control_button) {
		control_button->set_text(instance.control == MCPRuntimeInstances::CONTROL_HUMAN
						? TTR("Return to Agent")
						: TTR("Take Control"));
	}
	if (pause_button) {
		pause_button->set_text(suspended ? TTR("Resume") : TTR("Pause"));
		pause_button->set_disabled(instance.pid == 0);
	}
	if (stop_button) {
		stop_button->set_disabled(instance.pid == 0);
	}
	if (status) {
		// Concrete state in words, not only an icon or a colour. A person must be able to
		// read who is driving and whether it is safe to close.
		String text = vformat("%s  -  %s",
				MCPRuntimeInstances::lifecycle_to_string(instance.lifecycle),
				instance.control == MCPRuntimeInstances::CONTROL_HUMAN
						? TTR("You have control")
						: TTR("Agent has control"));
		if (!instance.role.is_empty()) {
			text += vformat("  -  %s", instance.role);
		}
		if (suspended) {
			text += vformat("  -  %s", TTR("Paused"));
		}
		if (!instance.detail.is_empty()) {
			text += vformat("\n%s", instance.detail);
		}
		status->set_text(text);
	}
}

void MCPWorkspaceTile::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		refresh();
	}
}

// ------------------------------------------------------------------ workspace ---

MCPWorkspace::MCPWorkspace() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	summary = memnew(Label);
	summary->set_h_size_flags(SIZE_EXPAND_FILL);
	toolbar->add_child(summary);

	stop_all_button = memnew(Button);
	stop_all_button->set_text(TTR("Stop All Agent Instances"));
	stop_all_button->set_tooltip_text(
			TTR("Stops every game this agent launched. Your own run from the Game workspace is "
				"left alone."));
	stop_all_button->connect(SceneStringName(pressed), callable_mp(this, &MCPWorkspace::_stop_all));
	toolbar->add_child(stop_all_button);

	add_child(memnew(HSeparator));

	stage = memnew(GridContainer);
	stage->set_columns(2);
	stage->set_h_size_flags(SIZE_EXPAND_FILL);
	stage->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(stage);

	set_process(true);
	refresh();
}

MCPWorkspaceTile *MCPWorkspace::add_tile(const String &p_instance_id) {
	if (tiles.has(p_instance_id)) {
		return tiles[p_instance_id];
	}
	// A tile with no registration could not be addressed or stopped, so it is refused
	// rather than created as an orphan.
	if (!MCPRuntimeInstances::exists(p_instance_id)) {
		return nullptr;
	}
	MCPWorkspaceTile *tile = memnew(MCPWorkspaceTile(p_instance_id));
	stage->add_child(tile);
	tiles.insert(p_instance_id, tile);
	// One column reads as a strip; beyond four tiles a third column keeps them legible.
	stage->set_columns(tiles.size() <= 1 ? 1 : (tiles.size() <= 4 ? 2 : 3));
	refresh();
	return tile;
}

MCPWorkspaceTile *MCPWorkspace::get_tile(const String &p_instance_id) const {
	const HashMap<String, MCPWorkspaceTile *>::ConstIterator found = tiles.find(p_instance_id);
	return found ? found->value : nullptr;
}

void MCPWorkspace::remove_tile(const String &p_instance_id) {
	MCPWorkspaceTile *tile = get_tile(p_instance_id);
	if (!tile) {
		return;
	}
	tiles.erase(p_instance_id);
	stage->remove_child(tile);
	memdelete(tile);
	refresh();
}

void MCPWorkspace::_stop_all() {
	MCPWorkspaceLauncher::stop_all();
	refresh();
}

void MCPWorkspace::_sync_tiles() {
	for (KeyValue<String, MCPWorkspaceTile *> &entry : tiles) {
		entry.value->refresh();
	}
}

void MCPWorkspace::refresh() {
	_sync_tiles();
	if (!summary) {
		return;
	}
	const int live = MCPRuntimeInstances::live().size();
	const int total = MCPRuntimeInstances::list().size();
	if (total == 0) {
		summary->set_text(TTR("No agent instances. The agent will open them here when it runs a game."));
	} else {
		// A sentence, not a row of counts.
		String text = vformat(TTR("%d running, %d recorded in this session."), live, total);

		// A game the user started with Embed on Play turned off is a top-level window.
		// The editor cannot put it behind itself - that is the window manager's business
		// - so it floats over these tiles, and the first report of "embedded games draw
		// over the tile chrome" turned out to be exactly this and nothing else. Say so
		// rather than let it look like the workspace is broken.
		if (live > 0 && EditorSettings::get_singleton() && EditorRunBar::get_singleton() &&
				EditorRunBar::get_singleton()->is_playing()) {
			const bool embed_on_play = (bool)EditorSettings::get_singleton()->get_project_metadata(
					"game_view", "embed_on_play", true);
			if (!embed_on_play) {
				text += " ";
				text += TTR("Your own run is not embedded, so its window floats over these tiles. Turn on Embed on Play in the Game workspace to keep it in place.");
			}
		}

		summary->set_text(text);
	}
	if (stop_all_button) {
		stop_all_button->set_disabled(live == 0);
	}
}

void MCPWorkspace::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			// Cheap poll rather than a signal from every state change: lifecycle moves
			// from several places, including a process dying on its own, and a missed
			// signal would leave a tile claiming a game is running when it is not.
			refresh_accumulator += get_process_delta_time();
			if (refresh_accumulator >= 0.5) {
				refresh_accumulator = 0.0;
				refresh();
			}
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			refresh();
		} break;
	}
}

// ------------------------------------------------------------------- launcher ---

String MCPWorkspaceLauncher::launch(MCPWorkspace *p_workspace, const String &p_label,
		const String &p_role, const String &p_task, const String &p_scene,
		MCPRuntimeInstances::Retention p_retention, String &r_error, Rect2i *r_embed_rect) {
	if (!p_workspace) {
		r_error = "the GodotAI workspace is not available";
		return String();
	}
	if (!DisplayServer::get_singleton() ||
			!DisplayServer::get_singleton()->has_feature(DisplayServerEnums::FEATURE_WINDOW_EMBEDDING)) {
		r_error = "this display server cannot embed a game window";
		return String();
	}

	// Show the workspace before laying out a tile. A hidden main screen has no size, so
	// every tile would report the same fallback rect and all the games would land on top
	// of one another - which is exactly what happened the first time this ran. The Game
	// workspace does the same thing when you press play.
	EditorMainScreen *main_screen = EditorNode::get_singleton()
			? EditorNode::get_singleton()->get_editor_main_screen()
			: nullptr;
	MCPWorkspacePlugin *plugin = mcp_workspace_get_plugin();
	if (main_screen && plugin) {
		const int index = main_screen->get_plugin_index(plugin);
		if (index >= 0 && main_screen->get_selected_index() != index) {
			main_screen->select(index);
		}
	}

	const String instance_id = MCPRuntimeInstances::create(p_label, p_role, p_task, p_retention);
	MCPWorkspaceTile *tile = p_workspace->add_tile(instance_id);
	// Force the pending layout so the new tile has a real rect to launch into, rather
	// than the one it had before it existed.
	if (tile) {
		MessageQueue::get_singleton()->flush();
	}
	if (!tile) {
		MCPRuntimeInstances::remove(instance_id);
		r_error = "could not create a tile for the instance";
		return String();
	}
	MCPRuntimeInstances::set_lifecycle(instance_id, MCPRuntimeInstances::LIFECYCLE_LAUNCHING);

	// Make sure something is listening before handing the game a --remote-debug URI.
	//
	// build_base_arguments() copies the debugger's server URI into the command line, but
	// the server is only started by the run bar when a human presses play. Launching
	// without this produced a game that ran and embedded perfectly and could never be
	// controlled - and, because a human's own run *does* start the server, whether
	// targeted control worked depended on whether they happened to be playing. That is
	// exactly the kind of race that passes a test once and fails it later.
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	if (debugger_node) {
		String uri = debugger_node->get_server_uri();
		if (uri.is_empty()) {
			uri = "tcp://";
			debugger_node->start(uri);
		}
	}

	// The same two builders the play button uses. Reimplementing either would drift the
	// moment a flag is added upstream.
	List<String> arguments;
	EditorRun::build_base_arguments(p_scene, String(), Vector<String>(), arguments);
	Window *window = p_workspace->get_window();
	if (!window) {
		MCPRuntimeInstances::set_lifecycle(instance_id, MCPRuntimeInstances::LIFECYCLE_FAILED,
				"the workspace has no window to embed into");
		r_error = "the workspace has no window to embed into";
		return instance_id;
	}
	const Rect2i embed_rect = tile->get_embed_rect();
	if (r_embed_rect) {
		*r_embed_rect = embed_rect;
	}
	embedded_process_apply_arguments(arguments, window->get_window_id(), embed_rect);

	ProcessID pid = 0;
	const Error error = OS::get_singleton()->create_instance(arguments, &pid);
	if (error != OK || pid == 0) {
		MCPRuntimeInstances::set_lifecycle(instance_id, MCPRuntimeInstances::LIFECYCLE_FAILED,
				"the game process did not start");
		r_error = "the game process did not start";
		return instance_id;
	}

	MCPRuntimeInstances::bind_pid(instance_id, pid);
	tile->embed(pid);
	p_workspace->refresh();
	return instance_id;
}

bool MCPWorkspaceLauncher::stop(const String &p_instance_id) {
	MCPRuntimeInstances::Instance instance;
	if (!MCPRuntimeInstances::get(p_instance_id, instance) || instance.pid == 0) {
		return false;
	}
	// Only ever a pid this module registered. That is what keeps "stop the agent's
	// games" away from the one the user pressed play on.
	OS::get_singleton()->kill(instance.pid);
	MCPRuntimeInstances::set_lifecycle(p_instance_id, MCPRuntimeInstances::LIFECYCLE_CLOSED,
			"stopped");
	return true;
}

int MCPWorkspaceLauncher::stop_all() {
	int stopped = 0;
	for (const MCPRuntimeInstances::Instance &instance : MCPRuntimeInstances::live()) {
		if (stop(instance.instance_id)) {
			stopped++;
		}
	}
	return stopped;
}

// --------------------------------------------------------------------- plugin ---

void MCPWorkspacePlugin::make_visible(bool p_visible) {
	if (workspace) {
		workspace->set_visible(p_visible);
	}
}

MCPWorkspacePlugin::MCPWorkspacePlugin() {
	workspace = memnew(MCPWorkspace);
	workspace->set_visible(false);
	EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(workspace);
}

static MCPWorkspacePlugin *&_workspace_plugin() {
	static MCPWorkspacePlugin *plugin = nullptr;
	return plugin;
}

void mcp_workspace_set_plugin(MCPWorkspacePlugin *p_plugin) {
	_workspace_plugin() = p_plugin;
}

MCPWorkspacePlugin *mcp_workspace_get_plugin() {
	return _workspace_plugin();
}
