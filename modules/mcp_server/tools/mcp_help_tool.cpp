/**************************************************************************/
/*  mcp_help_tool.cpp                                                     */
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

#include "mcp_help_tool.h"

#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/version.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void MCPHelpTool::register_tools(MCPToolRegistry *p_registry) {
	// help — top-level tool, no category prefix.
	{
		Dictionary props;
		props["category"] = make_prop("string",
				"Optional: filter by category prefix (e.g. 'debug', 'editor', 'gdscript', 'project'). "
				"Omit to see all tools.");
		props["tool"] = make_prop("string",
				"Optional: get detailed help for a specific tool by name (e.g. 'debug/run_project').");

		p_registry->register_tool(
				"help",
				"Help",
				"List all available MCP tools with descriptions, grouped by category. "
				"Call with no arguments for a full overview. Pass 'category' to filter, "
				"or 'tool' for detailed info on a specific tool including its parameters.",
				make_schema(props),
				make_annotations(/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true),
				callable_mp_static(&MCPHelpTool::handle_help));
	}
}

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------

Dictionary MCPHelpTool::handle_help(const Dictionary &p_args) {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return make_tool_error("MCP protocol not available.");
	}

	MCPToolRegistry *registry = protocol->get_tool_registry();
	if (!registry) {
		return make_tool_error("Tool registry not available.");
	}

	String filter_category = p_args.get("category", "");
	String filter_tool = p_args.get("tool", "");

	// Get the full tool list via the registry's standard list method.
	Dictionary list_result = registry->list_tools(Dictionary());
	Array all_tools = list_result.get("tools", Array());

	// --- Single tool detail mode ---
	if (!filter_tool.is_empty()) {
		for (int i = 0; i < all_tools.size(); i++) {
			Dictionary t = all_tools[i];
			if (String(t["name"]) == filter_tool) {
				String text;
				text += "## " + String(t["name"]) + "\n";
				text += "**" + String(t["title"]) + "**\n\n";
				text += String(t["description"]) + "\n\n";

				// Parameters.
				Dictionary schema = t.get("inputSchema", Dictionary());
				Dictionary properties = schema.get("properties", Dictionary());
				Array required = schema.get("required", Array());

				if (!properties.is_empty()) {
					text += "### Parameters\n";
					Array keys = properties.keys();
					for (int k = 0; k < keys.size(); k++) {
						String key = keys[k];
						Dictionary prop = properties[key];
						String type = prop.get("type", "any");
						String desc = prop.get("description", "");
						bool is_required = required.has(key);
						text += "- **" + key + "** (" + type + ")";
						if (is_required) {
							text += " *required*";
						}
						text += ": " + desc + "\n";
					}
				} else {
					text += "No parameters.\n";
				}

				// Annotations.
				Dictionary ann = t.get("annotations", Dictionary());
				if (!ann.is_empty()) {
					text += "\n### Annotations\n";
					if ((bool)ann.get("readOnlyHint", false)) {
						text += "- Read-only (does not modify state)\n";
					}
					if ((bool)ann.get("destructiveHint", false)) {
						text += "- Destructive (may modify or delete data)\n";
					}
					if ((bool)ann.get("idempotentHint", false)) {
						text += "- Idempotent (safe to retry)\n";
					}
				}

				return make_tool_result(text);
			}
		}
		return make_tool_error("Unknown tool: " + filter_tool + ". Call help with no arguments to see all tools.");
	}

	// --- Category / full listing mode ---

	// Group tools by category prefix (everything before the first '/').
	HashMap<String, Vector<Dictionary>> categories;
	Vector<String> category_order; // Preserve insertion order.

	for (int i = 0; i < all_tools.size(); i++) {
		Dictionary t = all_tools[i];
		String name = t["name"];

		// Extract category from "category/action" naming.
		String cat;
		int slash = name.find("/");
		if (slash != -1) {
			cat = name.substr(0, slash);
		} else {
			cat = "general";
		}

		// Apply category filter if set.
		if (!filter_category.is_empty() && cat != filter_category) {
			continue;
		}

		if (!categories.has(cat)) {
			categories[cat] = Vector<Dictionary>();
			category_order.push_back(cat);
		}
		categories[cat].push_back(t);
	}

	// Build output text.
	String text;
	text += "# Godot MCP Server — Tool Reference\n";
	text += "Godot " + String(GODOT_VERSION_FULL_CONFIG) + " | ";
	text += itos(all_tools.size()) + " tools available\n\n";

	if (!filter_category.is_empty() && category_order.is_empty()) {
		text += "No tools found in category: " + filter_category + "\n";
		text += "Available categories: ";

		// Collect unique categories.
		HashMap<String, bool> cats;
		for (int i = 0; i < all_tools.size(); i++) {
			Dictionary t = all_tools[i];
			String name = t["name"];
			int slash = name.find("/");
			if (slash != -1) {
				cats[name.substr(0, slash)] = true;
			}
		}
		Array cat_keys = Array();
		for (const KeyValue<String, bool> &kv : cats) {
			text += kv.key + " ";
		}
		text += "\n";
	}

	for (int c = 0; c < category_order.size(); c++) {
		const String &cat = category_order[c];
		const Vector<Dictionary> &tools = categories[cat];

		// Category header with friendly names.
		String cat_title = cat;
		if (cat == "project") {
			cat_title = "Project";
		} else if (cat == "editor") {
			cat_title = "Editor / Files";
		} else if (cat == "gdscript") {
			cat_title = "GDScript Validation";
		} else if (cat == "debug") {
			cat_title = "Debug / Runtime";
		} else if (cat == "general") {
			cat_title = "General";
		}

		text += "## " + cat_title + " (" + itos(tools.size()) + " tools)\n";

		for (int t = 0; t < tools.size(); t++) {
			const Dictionary &tool = tools[t];
			String name = tool["name"];
			String title = tool["title"];
			String desc = tool["description"];

			// Truncate long descriptions for the overview.
			if (desc.length() > 120) {
				desc = desc.substr(0, 117) + "...";
			}

			text += "- **" + name + "** — " + desc + "\n";
		}
		text += "\n";
	}

	text += "---\n";
	text += "Use `help` with `tool: \"<name>\"` for detailed parameter info.\n";
	text += "Use `help` with `category: \"debug\"` to filter by category.\n";

	return make_tool_result(text);
}
