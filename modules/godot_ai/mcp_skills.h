/**************************************************************************/
/*  mcp_skills.h                                                          */
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

#ifndef MCP_SKILLS_H
#define MCP_SKILLS_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

// Reusable workflow instructions discovered from the filesystem, as SKILL.md files
// with YAML frontmatter.
//
// Skills are *discovered* but never automatically trusted: a skill is an instruction
// file that anyone could have dropped into a project or an addon, so it stays denied
// until the user allows it by name.
struct MCPSkill {
	String name;
	String description;
	bool enabled = true; // The skill's own frontmatter switch.
	String required_editor_version; // e.g. ">=4.3"; empty means "any".
	Vector<String> tools; // Tools the skill declares it needs.

	String path; // Absolute path of the SKILL.md file.
	String res_path; // res:// form when inside the project, otherwise empty.
	String root_kind; // "project", "plugin" or "user".

	bool allowed = false; // User trust decision.
	bool version_supported = true; // Whether required_editor_version matches.
	String problem; // Non-empty when the file was found but is unusable.

	// A skill is only usable when it parsed, is enabled, matches this editor, and
	// the user has allowed it.
	bool is_usable() const { return problem.is_empty() && enabled && version_supported && allowed; }

	Dictionary to_dictionary() const;
};

// How a skill should be presented to the user: the reason it is or is not usable,
// and whether offering an allow/revoke button would mean anything.
//
// Split out of the approvals dialog so the decision can be tested without a display;
// the dialog only renders what this returns.
String mcp_skill_status_text(const MCPSkill &p_skill, bool &r_can_toggle, bool &r_needs_decision);

class MCPSkills {
	static bool _parse_frontmatter(const String &p_text, Dictionary &r_fields, String &r_body, String &r_error);

public:
	// Directories scanned for `*/SKILL.md`, in precedence order.
	static Vector<String> get_roots();

	// Discovers every skill, newest precedence first. Files that fail to parse are
	// still returned, with `problem` set, so the user can see why.
	static Vector<MCPSkill> discover();

	// Parses one SKILL.md. Exposed for tests, and free of filesystem access.
	static bool parse(const String &p_text, MCPSkill &r_skill, String &r_body, String &r_error);

	// Evaluates a version constraint (">=4.3", "4.3", "<5", "==4.3") against a
	// major.minor pair. An unparsable constraint is treated as unsatisfied.
	static bool version_satisfied(const String &p_constraint, int p_major, int p_minor);

	// Trust state, persisted in editor settings.
	static bool is_allowed(const String &p_name);
	// Builtin skills are trusted unless turned off by name; see the comment where
	// they are discovered for why they are the one exception.
	static bool is_revoked(const String &p_name);
	static void set_allowed(const String &p_name, bool p_allowed);

	// Instruction body of a skill, read on demand.
	static bool read_instructions(const MCPSkill &p_skill, String &r_body, String &r_error);

	// A supporting file next to the skill (`references/naming.md`), read on demand
	// and confined to the skill's own directory.
	static bool read_resource(const MCPSkill &p_skill, const String &p_relative, String &r_contents, String &r_error);

	// Test seams.
	static void set_roots_override(const Vector<String> &p_roots);
	static void clear_roots_override();
	static void set_allow_override(const Vector<String> &p_allowed);
	static void clear_allow_override();
};

#endif // MCP_SKILLS_H
