/**************************************************************************/
/*  mcp_workspace.h                                                       */
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

#ifndef MCP_WORKSPACE_H
#define MCP_WORKSPACE_H

#include "mcp_runtime_instances.h"

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"

class EmbeddedProcessBase;

// The GodotAI workspace: several agent-owned games, side by side, in the main screen.
//
// The user's workspace specification puts this in the main screen rather than a dock,
// because several playable instances squeezed into a side panel are unusable. The dock
// is the control plane; this is the view plane.
//
// It deliberately does **not** touch the ordinary Game workspace. A human pressing play
// still gets their own run there, and nothing here can stop it: every process this
// launches is registered in `MCPRuntimeInstances`, and every action addresses one
// registered instance.
//
// One tile hosts one process. That is the whole reason this can exist at all - see
// DEC-0011: the platform layer keys embedding by process id, so N embedder controls
// means N embedded games, and `GameView`'s single embedder was an editor-layer
// limitation rather than a platform one.
class MCPWorkspaceTile : public PanelContainer {
	GDCLASS(MCPWorkspaceTile, PanelContainer);

	String instance_id;

	Label *title = nullptr;
	Label *status = nullptr;
	Button *control_button = nullptr;
	Button *pause_button = nullptr;
	Button *stop_button = nullptr;
	EmbeddedProcessBase *embedder = nullptr;

	bool suspended = false;

	void _toggle_control();
	void _toggle_pause();
	void _stop();

protected:
	void _notification(int p_what);

public:
	// Not get_instance_id(): Object already has one, returning an ObjectID.
	String get_runtime_instance_id() const { return instance_id; }
	EmbeddedProcessBase *get_embedder() const { return embedder; }

	// The screen rect the game should occupy, for the launch arguments.
	Rect2i get_embed_rect() const;

	// Hands a launched process to this tile.
	void embed(ProcessID p_pid);

	// Re-reads the instance from the registry and repaints the header.
	void refresh();

	MCPWorkspaceTile(const String &p_instance_id);
};

class MCPWorkspace : public VBoxContainer {
	GDCLASS(MCPWorkspace, VBoxContainer);

	Label *summary = nullptr;
	Button *stop_all_button = nullptr;
	GridContainer *stage = nullptr;
	HashMap<String, MCPWorkspaceTile *> tiles;

	double refresh_accumulator = 0.0;

	void _stop_all();
	void _sync_tiles();

protected:
	void _notification(int p_what);

public:
	// Adds a tile for an already-registered instance. Returns null if the instance is
	// unknown, because a tile with no registration could not be addressed or stopped.
	MCPWorkspaceTile *add_tile(const String &p_instance_id);
	MCPWorkspaceTile *get_tile(const String &p_instance_id) const;
	void remove_tile(const String &p_instance_id);

	int get_tile_count() const { return tiles.size(); }

	void refresh();

	MCPWorkspace();
};

// Launches agent-owned game processes and embeds them into workspace tiles.
//
// Launching goes through `EditorRun::build_base_arguments()` and
// `embedded_process_apply_arguments()` - the same two functions the play button uses -
// so an agent-owned game is configured exactly like the user's own. Reimplementing
// either would drift the moment a flag is added upstream.
//
// This starts the editor's own executable with computed arguments. That is not the
// "arbitrary shell command" the safety rules forbid: no tool supplies a command line,
// the binary is fixed, and every argument comes from the engine's own builders.
class MCPWorkspaceLauncher {
public:
	// Registers, launches and embeds one instance. Returns the instance id, or an empty
	// string with r_error filled in.
	static String launch(MCPWorkspace *p_workspace, const String &p_label, const String &p_role,
			const String &p_task, const String &p_scene, MCPRuntimeInstances::Retention p_retention,
			String &r_error, Rect2i *r_embed_rect = nullptr);

	// Stops one agent-owned instance. Never touches a process this module did not launch.
	static bool stop(const String &p_instance_id);

	// Stops every agent-owned instance, leaving the user's own run alone.
	static int stop_all();
};

class MCPWorkspacePlugin : public EditorPlugin {
	GDCLASS(MCPWorkspacePlugin, EditorPlugin);

	MCPWorkspace *workspace = nullptr;

public:
	virtual String get_plugin_name() const override { return "GodotAI"; }
	virtual bool has_main_screen() const override { return true; }
	virtual void make_visible(bool p_visible) override;

	MCPWorkspace *get_workspace() const { return workspace; }

	MCPWorkspacePlugin();
};

// The workspace plugin for this editor, or null headlessly. Set once at editor init.
// A plain accessor rather than a singleton on the plugin, so nothing is tempted to
// construct one outside the editor.
void mcp_workspace_set_plugin(MCPWorkspacePlugin *p_plugin);
MCPWorkspacePlugin *mcp_workspace_get_plugin();

#endif // MCP_WORKSPACE_H
