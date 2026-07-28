/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "mcp_service.h"
#include "mcp_tool.h"
#include "mcp_runtime_agent.h"
#include "mcp_tool_registry.h"
#include "tools/mcp_builtin_tools.h"

#include "core/config/engine.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#endif

static MCPToolRegistry *mcp_tool_registry = nullptr;

#ifdef TOOLS_ENABLED
static void _godot_ai_editor_init() {
	// The service is an EditorPlugin so its lifetime follows the editor's, the same
	// way the debug adapter and language servers do.
	MCPService *service = memnew(MCPService);
	EditorNode::get_singleton()->add_editor_plugin(service);
	mcp_register_builtin_tools();
}
#endif

void initialize_godot_ai_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_ABSTRACT_CLASS(MCPTool);
		GDREGISTER_CLASS(MCPCallableTool);
		GDREGISTER_CLASS(MCPToolRegistry);

		// The registry exists as soon as the scene level is up, so unit tests and
		// headless runs can use it without an editor.
		mcp_tool_registry = memnew(MCPToolRegistry);
		Engine::get_singleton()->add_singleton(Engine::Singleton("MCPToolRegistry", MCPToolRegistry::get_singleton()));

#ifdef TOOLS_ENABLED
		// The other end of the runtime channel. This is a no-op in an editor process
		// and when nothing is debugging us; in a game launched from the editor it is
		// what lets the editor deliver real input and read back what happened.
		MCPRuntimeAgent::install();
#endif
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback(&_godot_ai_editor_init);
	}
#endif
}

void uninitialize_godot_ai_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
#ifdef TOOLS_ENABLED
		MCPRuntimeAgent::uninstall();
#endif
		if (mcp_tool_registry) {
			memdelete(mcp_tool_registry);
			mcp_tool_registry = nullptr;
		}
	}
}
