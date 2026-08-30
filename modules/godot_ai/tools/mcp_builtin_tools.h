/**************************************************************************/
/*  mcp_builtin_tools.h                                                   */
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

#ifndef MCP_BUILTIN_TOOLS_H
#define MCP_BUILTIN_TOOLS_H

// Registers every tool that ships with the editor. Called once, after the AI
// service plugin is installed.
void mcp_register_builtin_tools();

// Project-scoped tools that need nothing but the filesystem. Registered separately
// so they can be exercised without an editor.
void mcp_register_project_tools();

// Structural scene editing, which goes through the editor's undo history.
void mcp_register_scene_tools();

// Skill discovery and reading. Filesystem-only, so usable without an editor.
void mcp_register_skill_tools();

// Listing and restoring the file snapshots taken before mutating tools run.
void mcp_register_checkpoint_tools();

// Reading the editor Output panel, which also carries the running game's output.
void mcp_register_output_tools();

// Persistent and runtime property edits, kept deliberately separate.
void mcp_register_property_tools();

// Screenshots of what the editor is rendering.
void mcp_register_capture_tools();

// Putting a question to the user and waiting for their answer.
void mcp_register_ask_user_tool();
void mcp_register_input_tools();
void mcp_register_user_data_tools();
void mcp_register_project_config_tools();

// The import pipeline, checkpoint diffs, and the editor's open windows.
void mcp_register_asset_tools();

// Locating parts of the editor's own interface on screen.
void mcp_register_editor_ui_tools();

// Discovering and running the project's own test scenes.
void mcp_register_scene_test_tools();

// Windowed full-profiler captures of the running game, exported to a file.
void mcp_register_profiler_tools();

// Reading back the live stream of what the agent has been doing.
void mcp_register_activity_tools();

// Recording a play session and replaying it against the running game.
void mcp_register_session_tools();

// Launching and addressing several agent-owned game instances at once.
void mcp_register_workspace_tools();

// Running a goal-directed playtest and assembling its report.
void mcp_register_playtest_tools();

// Keeping a value that was tuned while the game was running.
void mcp_register_promote_tools();

// Trying several values for one property against the running game.
void mcp_register_variant_tools();

// Putting a plan to the user before any of it happens.
void mcp_register_proposal_tools();

// What previous sessions learned about this project, kept in the project.
void mcp_register_memory_tools();

// The editor's class reference, including the project's own script classes.
void mcp_register_docs_tools();

#endif // MCP_BUILTIN_TOOLS_H
