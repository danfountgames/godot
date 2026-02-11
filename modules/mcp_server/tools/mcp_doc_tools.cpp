/**************************************************************************/
/*  mcp_doc_tools.cpp                                                     */
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

#include "mcp_doc_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/doc_data.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/doc/doc_tools.h"
#include "editor/doc/editor_help.h"

// ============================================================================
// BBCode-to-text conversion
// ============================================================================

String MCPDocTools::_bbcode_to_text(const String &p_bbcode) {
	String text = p_bbcode;

	// Strip codeblock containers first (before processing inner blocks).
	text = text.replace("[codeblocks]", "").replace("[/codeblocks]", "");

	// Process [gdscript]...[/gdscript] blocks.
	int pos = 0;
	while ((pos = text.find("[gdscript]", pos)) != -1) {
		int end = text.find("[/gdscript]", pos);
		if (end == -1) {
			break;
		}
		String code = text.substr(pos + 10, end - pos - 10);
		text = text.substr(0, pos) + "\n```gdscript" + code + "```\n" + text.substr(end + 11);
	}

	// Process [csharp]...[/csharp] blocks.
	pos = 0;
	while ((pos = text.find("[csharp]", pos)) != -1) {
		int end = text.find("[/csharp]", pos);
		if (end == -1) {
			break;
		}
		String code = text.substr(pos + 8, end - pos - 8);
		text = text.substr(0, pos) + "\n```csharp" + code + "```\n" + text.substr(end + 9);
	}

	// Process [codeblock]...[/codeblock] blocks.
	pos = 0;
	while ((pos = text.find("[codeblock]", pos)) != -1) {
		int end = text.find("[/codeblock]", pos);
		if (end == -1) {
			break;
		}
		String code = text.substr(pos + 11, end - pos - 11);
		text = text.substr(0, pos) + "\n```" + code + "```\n" + text.substr(end + 12);
	}

	// Inline formatting.
	text = text.replace("[b]", "**").replace("[/b]", "**");
	text = text.replace("[i]", "*").replace("[/i]", "*");
	text = text.replace("[code]", "`").replace("[/code]", "`");
	text = text.replace("[br]", "\n");
	text = text.replace("[lb]", "[").replace("[rb]", "]");

	// Tags that strip to content.
	text = text.replace("[kbd]", "").replace("[/kbd]", "");
	text = text.replace("[center]", "").replace("[/center]", "");
	text = text.replace("[indent]", "  ").replace("[/indent]", "");
	text = text.replace("[ol]", "").replace("[/ol]", "");
	text = text.replace("[ul]", "").replace("[/ul]", "");

	// Cross-reference tags: [method name], [member name], [param name], etc.
	const char *ref_tags[] = { "method", "member", "param", "signal", "constant",
		"enum", "annotation", "theme_item", nullptr };
	for (int t = 0; ref_tags[t]; t++) {
		String open = String("[") + ref_tags[t] + " ";
		pos = 0;
		while ((pos = text.find(open, pos)) != -1) {
			int end = text.find("]", pos);
			if (end == -1) {
				break;
			}
			String content = text.substr(pos + open.length(), end - pos - open.length());
			String replacement;
			if (String(ref_tags[t]) == "method") {
				replacement = "`" + content + "()`";
			} else {
				replacement = "`" + content + "`";
			}
			text = text.substr(0, pos) + replacement + text.substr(end + 1);
			pos += replacement.length();
		}
	}

	// [url=URL]text[/url] -> text (URL)
	pos = 0;
	while ((pos = text.find("[url=", pos)) != -1) {
		int url_end = text.find("]", pos);
		if (url_end == -1) {
			break;
		}
		String url = text.substr(pos + 5, url_end - pos - 5);
		int close = text.find("[/url]", url_end);
		if (close == -1) {
			break;
		}
		String link_text = text.substr(url_end + 1, close - url_end - 1);
		text = text.substr(0, pos) + link_text + " (" + url + ")" + text.substr(close + 6);
	}

	// [url]URL[/url] -> URL
	pos = 0;
	while ((pos = text.find("[url]", pos)) != -1) {
		int close = text.find("[/url]", pos);
		if (close == -1) {
			break;
		}
		String url = text.substr(pos + 5, close - pos - 5);
		text = text.substr(0, pos) + url + text.substr(close + 6);
	}

	// Strip [color=...] and [/color].
	pos = 0;
	while ((pos = text.find("[color=", pos)) != -1) {
		int end = text.find("]", pos);
		if (end == -1) {
			break;
		}
		text = text.substr(0, pos) + text.substr(end + 1);
	}
	text = text.replace("[/color]", "");

	// Strip [font_size=...] and [/font_size].
	pos = 0;
	while ((pos = text.find("[font_size=", pos)) != -1) {
		int end = text.find("]", pos);
		if (end == -1) {
			break;
		}
		text = text.substr(0, pos) + text.substr(end + 1);
	}
	text = text.replace("[/font_size]", "");

	// Strip [img]...[/img].
	pos = 0;
	while ((pos = text.find("[img]", pos)) != -1) {
		int end = text.find("[/img]", pos);
		if (end == -1) {
			break;
		}
		text = text.substr(0, pos) + text.substr(end + 6);
	}

	// Strip [img=...]...[/img] variant.
	pos = 0;
	while ((pos = text.find("[img=", pos)) != -1) {
		int end = text.find("[/img]", pos);
		if (end == -1) {
			pos++;
			break;
		}
		text = text.substr(0, pos) + text.substr(end + 6);
	}

	return text.strip_edges();
}

// ============================================================================
// Signature formatting
// ============================================================================

String MCPDocTools::_format_method_sig(const DocData::MethodDoc &p_method) {
	String sig = p_method.name + "(";
	for (int i = 0; i < p_method.arguments.size(); i++) {
		if (i > 0) {
			sig += ", ";
		}
		const DocData::ArgumentDoc &arg = p_method.arguments[i];
		sig += arg.name + ": " + arg.type;
		if (!arg.default_value.is_empty()) {
			sig += " = " + arg.default_value;
		}
	}
	sig += ")";
	if (!p_method.return_type.is_empty() && p_method.return_type != "void") {
		sig += " -> " + p_method.return_type;
	}
	if (!p_method.qualifiers.is_empty()) {
		sig += "  [" + p_method.qualifiers + "]";
	}
	return sig;
}

String MCPDocTools::_format_property_sig(const DocData::PropertyDoc &p_prop) {
	String sig = p_prop.name + ": " + p_prop.type;
	if (!p_prop.default_value.is_empty()) {
		sig += " = " + p_prop.default_value;
	}
	return sig;
}

// ============================================================================
// Relevance scoring
// ============================================================================

int MCPDocTools::_score_class(const DocData::ClassDoc &p_class, const Vector<String> &p_terms) {
	int score = 0;
	bool all_matched = true;

	String name_lower = p_class.name.to_lower();
	String brief_lower = p_class.brief_description.to_lower();
	String desc_lower = p_class.description.to_lower();
	String keywords_lower = p_class.keywords.to_lower();

	for (int t = 0; t < p_terms.size(); t++) {
		const String &term = p_terms[t];
		bool term_found = false;

		if (name_lower == term) {
			score += 10;
			term_found = true;
		} else if (name_lower.find(term) != -1) {
			score += 8;
			term_found = true;
		}

		if (brief_lower.find(term) != -1) {
			score += 5;
			term_found = true;
		}
		if (keywords_lower.find(term) != -1) {
			score += 3;
			term_found = true;
		}
		if (desc_lower.find(term) != -1) {
			score += 2;
			term_found = true;
		}

		if (!term_found) {
			for (int m = 0; m < p_class.methods.size(); m++) {
				if (p_class.methods[m].name.to_lower().find(term) != -1) {
					score += 1;
					term_found = true;
					break;
				}
			}
		}
		if (!term_found) {
			for (int p = 0; p < p_class.properties.size(); p++) {
				if (p_class.properties[p].name.to_lower().find(term) != -1) {
					score += 1;
					term_found = true;
					break;
				}
			}
		}
		if (!term_found) {
			for (int s = 0; s < p_class.signals.size(); s++) {
				if (p_class.signals[s].name.to_lower().find(term) != -1) {
					score += 1;
					term_found = true;
					break;
				}
			}
		}

		if (!term_found) {
			all_matched = false;
		}
	}

	return all_matched ? score : 0;
}

int MCPDocTools::_score_method(const DocData::MethodDoc &p_method, const Vector<String> &p_terms) {
	int score = 0;
	bool all_matched = true;

	String name_lower = p_method.name.to_lower();
	String desc_lower = p_method.description.to_lower();
	String keywords_lower = p_method.keywords.to_lower();

	for (int t = 0; t < p_terms.size(); t++) {
		const String &term = p_terms[t];
		bool term_found = false;

		if (name_lower == term) {
			score += 10;
			term_found = true;
		} else if (name_lower.find(term) != -1) {
			score += 6;
			term_found = true;
		}

		if (desc_lower.find(term) != -1) {
			score += 3;
			term_found = true;
		}
		if (keywords_lower.find(term) != -1) {
			score += 2;
			term_found = true;
		}

		// Check parameter names and types.
		if (!term_found) {
			for (int a = 0; a < p_method.arguments.size(); a++) {
				if (p_method.arguments[a].name.to_lower().find(term) != -1 ||
						p_method.arguments[a].type.to_lower().find(term) != -1) {
					score += 1;
					term_found = true;
					break;
				}
			}
		}

		if (!term_found) {
			all_matched = false;
		}
	}

	return all_matched ? score : 0;
}

bool MCPDocTools::_all_terms_match(const String &p_haystack, const Vector<String> &p_terms) {
	String lower = p_haystack.to_lower();
	for (int i = 0; i < p_terms.size(); i++) {
		if (lower.find(p_terms[i]) == -1) {
			return false;
		}
	}
	return true;
}

// ============================================================================
// Inheritance helpers
// ============================================================================

String MCPDocTools::_get_inheritance_chain(const String &p_class_name) {
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return p_class_name;
	}

	String chain = p_class_name;
	String current = p_class_name;

	for (int depth = 0; depth < 20; depth++) {
		const DocData::ClassDoc *cd = docs->class_list.getptr(current);
		if (!cd || cd->inherits.is_empty()) {
			break;
		}
		chain += " < " + cd->inherits;
		current = cd->inherits;
	}

	return chain;
}

bool MCPDocTools::_class_matches_category(const DocData::ClassDoc &p_class, const String &p_category) {
	if (p_category.is_empty()) {
		return true;
	}

	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return true;
	}

	// Map category name to base class name.
	String target;
	if (p_category == "node") {
		target = "Node";
	} else if (p_category == "resource") {
		target = "Resource";
	} else if (p_category == "refcounted") {
		target = "RefCounted";
	} else {
		return true;
	}

	String current = p_class.name;
	for (int depth = 0; depth < 20; depth++) {
		if (current == target) {
			return true;
		}
		const DocData::ClassDoc *cd = docs->class_list.getptr(current);
		if (!cd || cd->inherits.is_empty()) {
			break;
		}
		current = cd->inherits;
	}
	return false;
}

// ============================================================================
// Class output builders
// ============================================================================

String MCPDocTools::_build_class_summary(const DocData::ClassDoc &p_class) {
	String out;
	out += "## " + p_class.name;
	if (!p_class.inherits.is_empty()) {
		out += " (inherits " + p_class.inherits + ")";
	}
	out += "\n";

	if (p_class.is_deprecated) {
		out += "**DEPRECATED**";
		if (!p_class.deprecated_message.is_empty()) {
			out += ": " + _bbcode_to_text(p_class.deprecated_message);
		}
		out += "\n";
	}
	if (p_class.is_experimental) {
		out += "**EXPERIMENTAL**";
		if (!p_class.experimental_message.is_empty()) {
			out += ": " + _bbcode_to_text(p_class.experimental_message);
		}
		out += "\n";
	}

	if (!p_class.brief_description.is_empty()) {
		out += _bbcode_to_text(p_class.brief_description) + "\n";
	}

	return out;
}

String MCPDocTools::_build_class_full(const DocData::ClassDoc &p_class, const String &p_sections, bool p_include_inherited) {
	// Parse sections filter.
	bool show_all = (p_sections.is_empty() || p_sections == "all");
	HashSet<String> sections;
	if (!show_all) {
		Vector<String> parts = p_sections.split(",");
		for (int i = 0; i < parts.size(); i++) {
			sections.insert(parts[i].strip_edges().to_lower());
		}
	}

	auto want = [&](const String &s) { return show_all || sections.has(s); };

	// Collect inherited members if requested.
	DocTools *docs = EditorHelp::get_doc_data();
	Vector<const DocData::ClassDoc *> class_chain;
	class_chain.push_back(&p_class);
	if (p_include_inherited && docs) {
		String current = p_class.inherits;
		for (int depth = 0; depth < 20 && !current.is_empty(); depth++) {
			const DocData::ClassDoc *parent = docs->class_list.getptr(current);
			if (!parent) {
				break;
			}
			class_chain.push_back(parent);
			current = parent->inherits;
		}
	}

	// Estimate size for auto-summary mode.
	int estimated_members = 0;
	for (int c = 0; c < class_chain.size(); c++) {
		estimated_members += class_chain[c]->methods.size();
		estimated_members += class_chain[c]->properties.size();
		estimated_members += class_chain[c]->signals.size();
		estimated_members += class_chain[c]->constants.size();
	}
	bool summary_mode = show_all && estimated_members > 60;

	String out;

	// Header.
	out += "# " + p_class.name + "\n";
	out += "Inherits: " + _get_inheritance_chain(p_class.name) + "\n";
	out += "Godot " + String(GODOT_VERSION_BRANCH) + "\n\n";

	if (p_class.is_deprecated) {
		out += "**DEPRECATED**";
		if (!p_class.deprecated_message.is_empty()) {
			out += ": " + _bbcode_to_text(p_class.deprecated_message);
		}
		out += "\n\n";
	}
	if (p_class.is_experimental) {
		out += "**EXPERIMENTAL**";
		if (!p_class.experimental_message.is_empty()) {
			out += ": " + _bbcode_to_text(p_class.experimental_message);
		}
		out += "\n\n";
	}

	// Brief description.
	if (!p_class.brief_description.is_empty()) {
		out += _bbcode_to_text(p_class.brief_description) + "\n\n";
	}

	// Description.
	if (want("description") && !p_class.description.is_empty()) {
		out += "## Description\n";
		out += _bbcode_to_text(p_class.description) + "\n\n";
	}

	// Tutorials.
	if (want("tutorials") && !p_class.tutorials.is_empty()) {
		out += "## Tutorials\n";
		for (int i = 0; i < p_class.tutorials.size(); i++) {
			if (!p_class.tutorials[i].title.is_empty()) {
				out += "- " + p_class.tutorials[i].title;
				if (!p_class.tutorials[i].link.is_empty()) {
					out += " (" + p_class.tutorials[i].link + ")";
				}
			} else {
				out += "- " + p_class.tutorials[i].link;
			}
			out += "\n";
		}
		out += "\n";
	}

	// Properties.
	if (want("properties")) {
		bool has_props = false;
		for (int c = 0; c < class_chain.size(); c++) {
			if (!class_chain[c]->properties.is_empty()) {
				has_props = true;
				break;
			}
		}
		if (has_props) {
			out += "## Properties\n";
			for (int c = 0; c < class_chain.size(); c++) {
				const DocData::ClassDoc *cd = class_chain[c];
				if (cd->properties.is_empty()) {
					continue;
				}
				if (c > 0) {
					out += "\n*Inherited from " + cd->name + ":*\n";
				}

				for (int i = 0; i < cd->properties.size(); i++) {
					const DocData::PropertyDoc &prop = cd->properties[i];
					out += "- `" + _format_property_sig(prop) + "`";
					if (prop.is_deprecated) {
						out += " **DEPRECATED**";
					}
					out += "\n";
					if (!summary_mode && !prop.description.is_empty()) {
						out += "  " + _bbcode_to_text(prop.description).replace("\n", "\n  ") + "\n";
					}
				}
			}
			out += "\n";
		}
	}

	// Methods.
	if (want("methods")) {
		bool has_methods = false;
		for (int c = 0; c < class_chain.size(); c++) {
			if (!class_chain[c]->methods.is_empty()) {
				has_methods = true;
				break;
			}
		}
		if (has_methods) {
			out += "## Methods\n";
			for (int c = 0; c < class_chain.size(); c++) {
				const DocData::ClassDoc *cd = class_chain[c];
				if (cd->methods.is_empty()) {
					continue;
				}
				if (c > 0) {
					out += "\n*Inherited from " + cd->name + ":*\n";
				}

				for (int i = 0; i < cd->methods.size(); i++) {
					const DocData::MethodDoc &method = cd->methods[i];
					out += "- `" + _format_method_sig(method) + "`";
					if (method.is_deprecated) {
						out += " **DEPRECATED**";
					}
					out += "\n";
					if (!summary_mode && !method.description.is_empty()) {
						out += "  " + _bbcode_to_text(method.description).replace("\n", "\n  ") + "\n";
					}
				}
			}
			out += "\n";
		}
	}

	// Signals.
	if (want("signals")) {
		bool has_signals = false;
		for (int c = 0; c < class_chain.size(); c++) {
			if (!class_chain[c]->signals.is_empty()) {
				has_signals = true;
				break;
			}
		}
		if (has_signals) {
			out += "## Signals\n";
			for (int c = 0; c < class_chain.size(); c++) {
				const DocData::ClassDoc *cd = class_chain[c];
				if (cd->signals.is_empty()) {
					continue;
				}
				if (c > 0) {
					out += "\n*Inherited from " + cd->name + ":*\n";
				}

				for (int i = 0; i < cd->signals.size(); i++) {
					out += "- `" + _format_method_sig(cd->signals[i]) + "`\n";
					if (!summary_mode && !cd->signals[i].description.is_empty()) {
						out += "  " + _bbcode_to_text(cd->signals[i].description).replace("\n", "\n  ") + "\n";
					}
				}
			}
			out += "\n";
		}
	}

	// Constants & Enums.
	if (want("constants")) {
		bool has_constants = false;
		for (int c = 0; c < class_chain.size(); c++) {
			if (!class_chain[c]->constants.is_empty()) {
				has_constants = true;
				break;
			}
		}
		if (has_constants) {
			out += "## Constants & Enums\n";
			for (int c = 0; c < class_chain.size(); c++) {
				const DocData::ClassDoc *cd = class_chain[c];
				if (cd->constants.is_empty()) {
					continue;
				}
				if (c > 0) {
					out += "\n*Inherited from " + cd->name + ":*\n";
				}

				// Group by enum.
				String current_enum;
				for (int i = 0; i < cd->constants.size(); i++) {
					const DocData::ConstantDoc &con = cd->constants[i];
					if (con.enumeration != current_enum) {
						current_enum = con.enumeration;
						if (!current_enum.is_empty()) {
							out += "\n**enum " + current_enum + "**\n";
						}
					}
					out += "- `" + con.name;
					if (con.is_value_valid) {
						out += " = " + con.value;
					}
					out += "`";
					if (con.is_deprecated) {
						out += " **DEPRECATED**";
					}
					out += "\n";
					if (!summary_mode && !con.description.is_empty()) {
						out += "  " + _bbcode_to_text(con.description).replace("\n", "\n  ") + "\n";
					}
				}
			}
			out += "\n";
		}
	}

	// Constructors.
	if (want("constructors")) {
		if (!p_class.constructors.is_empty()) {
			out += "## Constructors\n";
			for (int i = 0; i < p_class.constructors.size(); i++) {
				out += "- `" + _format_method_sig(p_class.constructors[i]) + "`\n";
				if (!summary_mode && !p_class.constructors[i].description.is_empty()) {
					out += "  " + _bbcode_to_text(p_class.constructors[i].description).replace("\n", "\n  ") + "\n";
				}
			}
			out += "\n";
		}
	}

	// Operators.
	if (want("operators")) {
		if (!p_class.operators.is_empty()) {
			out += "## Operators\n";
			for (int i = 0; i < p_class.operators.size(); i++) {
				out += "- `" + _format_method_sig(p_class.operators[i]) + "`\n";
			}
			out += "\n";
		}
	}

	// Annotations.
	if (want("annotations")) {
		if (!p_class.annotations.is_empty()) {
			out += "## Annotations\n";
			for (int i = 0; i < p_class.annotations.size(); i++) {
				out += "- `" + _format_method_sig(p_class.annotations[i]) + "`\n";
				if (!summary_mode && !p_class.annotations[i].description.is_empty()) {
					out += "  " + _bbcode_to_text(p_class.annotations[i].description).replace("\n", "\n  ") + "\n";
				}
			}
			out += "\n";
		}
	}

	// Theme properties.
	if (want("theme_properties")) {
		if (!p_class.theme_properties.is_empty()) {
			out += "## Theme Properties\n";
			for (int i = 0; i < p_class.theme_properties.size(); i++) {
				const DocData::ThemeItemDoc &ti = p_class.theme_properties[i];
				out += "- `" + ti.name + ": " + ti.type + "`";
				if (!ti.default_value.is_empty()) {
					out += " = " + ti.default_value;
				}
				out += "\n";
				if (!summary_mode && !ti.description.is_empty()) {
					out += "  " + _bbcode_to_text(ti.description).replace("\n", "\n  ") + "\n";
				}
			}
			out += "\n";
		}
	}

	// Summary mode note.
	if (summary_mode) {
		out += "---\n";
		out += "**Note:** This class has " + itos(estimated_members) + " members. ";
		out += "Descriptions are omitted for brevity. Use `doc/get_method` or `doc/get_property` ";
		out += "to get details for specific members, or use `doc/get_class` with the `sections` ";
		out += "parameter (e.g., \"methods\" or \"signals\") to see a specific section with full descriptions.\n";
	}

	// Hard cap to prevent excessively large responses.
	if (out.length() > 32000) {
		out = out.substr(0, 32000);
		out += "\n\n... (output truncated at 32KB -- use `sections` parameter to request specific sections)\n";
	}

	return out;
}

// ============================================================================
// Sort comparators for scored results
// ============================================================================

namespace {

struct ScoredClass {
	String name;
	int score;
};

struct ScoredClassCompare {
	_FORCE_INLINE_ bool operator()(const ScoredClass &a, const ScoredClass &b) const {
		return a.score > b.score;
	}
};

struct ScoredMethod {
	String class_name;
	String method_name;
	int score;
};

struct ScoredMethodCompare {
	_FORCE_INLINE_ bool operator()(const ScoredMethod &a, const ScoredMethod &b) const {
		return a.score > b.score;
	}
};

} // anonymous namespace

// ============================================================================
// Tool: doc/search_classes
// ============================================================================

Dictionary MCPDocTools::handle_search_classes(const Dictionary &p_args) {
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return make_tool_error("Documentation not loaded yet. Please wait for editor initialization.");
	}

	String query = String(p_args.get("query", "")).strip_edges();
	if (query.is_empty()) {
		return make_tool_error("Missing required parameter: query");
	}

	int limit = CLAMP((int)p_args.get("limit", 20), 1, 50);
	String category = String(p_args.get("category", "")).strip_edges().to_lower();

	// Split query into lowercase terms.
	Vector<String> terms;
	Vector<String> raw_terms = query.to_lower().split(" ", false);
	for (int i = 0; i < raw_terms.size(); i++) {
		String t = raw_terms[i].strip_edges();
		if (!t.is_empty()) {
			terms.push_back(t);
		}
	}
	if (terms.is_empty()) {
		return make_tool_error("Query must contain at least one search term.");
	}

	// Score all classes.
	Vector<ScoredClass> scored;

	for (const KeyValue<String, DocData::ClassDoc> &kv : docs->class_list) {
		if (!_class_matches_category(kv.value, category)) {
			continue;
		}
		int score = _score_class(kv.value, terms);
		if (score > 0) {
			ScoredClass sc;
			sc.name = kv.key;
			sc.score = score;
			scored.push_back(sc);
		}
	}

	// Sort by score descending.
	scored.sort_custom<ScoredClassCompare>();

	// Build output.
	int count = MIN(limit, scored.size());

	String text = "# Godot " + String(GODOT_VERSION_BRANCH) + " - Class Search Results";
	text += "\n# Query: \"" + query + "\"";
	text += "\n# Found " + itos(scored.size()) + " matching classes (showing top " + itos(count) + ")\n\n";

	Array structured_results;

	for (int i = 0; i < count; i++) {
		const DocData::ClassDoc *cd = docs->class_list.getptr(scored[i].name);
		if (!cd) {
			continue;
		}

		text += _build_class_summary(*cd) + "\n";

		Dictionary item;
		item["name"] = cd->name;
		item["inherits"] = cd->inherits;
		item["brief_description"] = _bbcode_to_text(cd->brief_description);
		item["score"] = scored[i].score;
		item["is_deprecated"] = cd->is_deprecated;
		item["is_experimental"] = cd->is_experimental;
		structured_results.push_back(item);
	}

	Dictionary structured;
	structured["query"] = query;
	structured["total_matches"] = scored.size();
	structured["results"] = structured_results;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool: doc/get_class
// ============================================================================

Dictionary MCPDocTools::handle_get_class(const Dictionary &p_args) {
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return make_tool_error("Documentation not loaded yet.");
	}

	String class_name = String(p_args.get("class_name", "")).strip_edges();
	if (class_name.is_empty()) {
		return make_tool_error("Missing required parameter: class_name");
	}

	// Case-sensitive lookup first, then case-insensitive fallback.
	const DocData::ClassDoc *cd = docs->class_list.getptr(class_name);
	if (!cd) {
		String lower = class_name.to_lower();
		for (const KeyValue<String, DocData::ClassDoc> &kv : docs->class_list) {
			if (kv.key.to_lower() == lower) {
				cd = &kv.value;
				class_name = kv.key;
				break;
			}
		}
	}

	if (!cd) {
		// Suggest similar names.
		Vector<String> suggestions;
		String lower = class_name.to_lower();
		for (const KeyValue<String, DocData::ClassDoc> &kv : docs->class_list) {
			if (kv.key.to_lower().find(lower) != -1 || lower.find(kv.key.to_lower()) != -1) {
				suggestions.push_back(kv.key);
				if (suggestions.size() >= 5) {
					break;
				}
			}
		}

		String msg = "Class not found: " + class_name;
		if (!suggestions.is_empty()) {
			msg += "\n\nDid you mean:";
			for (int i = 0; i < suggestions.size(); i++) {
				msg += "\n- " + suggestions[i];
			}
		}
		return make_tool_error(msg);
	}

	String sections = String(p_args.get("sections", "all")).strip_edges();
	bool include_inherited = (bool)p_args.get("include_inherited", false);

	String text = _build_class_full(*cd, sections, include_inherited);

	Dictionary structured;
	structured["class_name"] = cd->name;
	structured["inherits"] = cd->inherits;
	structured["is_deprecated"] = cd->is_deprecated;
	structured["is_experimental"] = cd->is_experimental;
	structured["method_count"] = cd->methods.size();
	structured["property_count"] = cd->properties.size();
	structured["signal_count"] = cd->signals.size();
	structured["constant_count"] = cd->constants.size();

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool: doc/search_methods
// ============================================================================

Dictionary MCPDocTools::handle_search_methods(const Dictionary &p_args) {
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return make_tool_error("Documentation not loaded yet.");
	}

	String query = String(p_args.get("query", "")).strip_edges();
	if (query.is_empty()) {
		return make_tool_error("Missing required parameter: query");
	}

	int limit = CLAMP((int)p_args.get("limit", 20), 1, 50);
	String class_filter = String(p_args.get("class_filter", "")).strip_edges();

	Vector<String> terms;
	Vector<String> raw_terms = query.to_lower().split(" ", false);
	for (int i = 0; i < raw_terms.size(); i++) {
		String t = raw_terms[i].strip_edges();
		if (!t.is_empty()) {
			terms.push_back(t);
		}
	}
	if (terms.is_empty()) {
		return make_tool_error("Query must contain at least one search term.");
	}

	// Build a set of classes to search (class + ancestors) if filter specified.
	HashSet<String> search_classes;
	if (!class_filter.is_empty()) {
		String current = class_filter;
		for (int depth = 0; depth < 20 && !current.is_empty(); depth++) {
			search_classes.insert(current);
			const DocData::ClassDoc *cd = docs->class_list.getptr(current);
			if (!cd) {
				break;
			}
			current = cd->inherits;
		}
	}

	Vector<ScoredMethod> scored;

	for (const KeyValue<String, DocData::ClassDoc> &kv : docs->class_list) {
		if (!search_classes.is_empty() && !search_classes.has(kv.key)) {
			continue;
		}

		// Search methods.
		for (int m = 0; m < kv.value.methods.size(); m++) {
			int s = _score_method(kv.value.methods[m], terms);
			if (s > 0) {
				ScoredMethod sm;
				sm.class_name = kv.key;
				sm.method_name = kv.value.methods[m].name;
				sm.score = s;
				scored.push_back(sm);
			}
		}

		// Search signals too.
		for (int s_idx = 0; s_idx < kv.value.signals.size(); s_idx++) {
			int s = _score_method(kv.value.signals[s_idx], terms);
			if (s > 0) {
				ScoredMethod sm;
				sm.class_name = kv.key;
				sm.method_name = kv.value.signals[s_idx].name + " [signal]";
				sm.score = s;
				scored.push_back(sm);
			}
		}
	}

	scored.sort_custom<ScoredMethodCompare>();

	int count = MIN(limit, scored.size());

	String text = "# Godot " + String(GODOT_VERSION_BRANCH) + " - Method Search Results";
	text += "\n# Query: \"" + query + "\"";
	text += "\n# Found " + itos(scored.size()) + " matching methods (showing top " + itos(count) + ")\n\n";

	Array structured_results;

	for (int i = 0; i < count; i++) {
		const DocData::ClassDoc *cd = docs->class_list.getptr(scored[i].class_name);
		if (!cd) {
			continue;
		}

		// Find the actual method doc.
		const DocData::MethodDoc *md = nullptr;
		bool is_signal = false;

		// Check if it's a signal (tagged with " [signal]" suffix).
		if (scored[i].method_name.ends_with(" [signal]")) {
			String clean_name = scored[i].method_name.replace(" [signal]", "");
			for (int s = 0; s < cd->signals.size(); s++) {
				if (cd->signals[s].name == clean_name) {
					md = &cd->signals[s];
					is_signal = true;
					break;
				}
			}
		} else {
			for (int m = 0; m < cd->methods.size(); m++) {
				if (cd->methods[m].name == scored[i].method_name) {
					md = &cd->methods[m];
					break;
				}
			}
		}

		if (md) {
			text += "## " + scored[i].class_name + "." + _format_method_sig(*md);
			if (is_signal) {
				text += " [signal]";
			}
			text += "\n";
			if (!md->description.is_empty()) {
				String desc = _bbcode_to_text(md->description);
				// Truncate long descriptions in search results.
				if (desc.length() > 300) {
					desc = desc.substr(0, 300) + "...";
				}
				text += desc + "\n";
			}
			text += "\n";

			Dictionary item;
			item["class_name"] = scored[i].class_name;
			item["method_name"] = md->name;
			item["signature"] = _format_method_sig(*md);
			item["is_signal"] = is_signal;
			item["score"] = scored[i].score;
			structured_results.push_back(item);
		}
	}

	Dictionary structured;
	structured["query"] = query;
	structured["total_matches"] = scored.size();
	structured["results"] = structured_results;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool: doc/get_method
// ============================================================================

Dictionary MCPDocTools::handle_get_method(const Dictionary &p_args) {
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return make_tool_error("Documentation not loaded yet.");
	}

	String class_name = String(p_args.get("class_name", "")).strip_edges();
	String member_name = String(p_args.get("member_name", "")).strip_edges();
	String member_type = String(p_args.get("member_type", "")).strip_edges().to_lower();

	if (class_name.is_empty()) {
		return make_tool_error("Missing required parameter: class_name");
	}
	if (member_name.is_empty()) {
		return make_tool_error("Missing required parameter: member_name");
	}

	// Case-insensitive class lookup.
	const DocData::ClassDoc *cd = docs->class_list.getptr(class_name);
	if (!cd) {
		String lower = class_name.to_lower();
		for (const KeyValue<String, DocData::ClassDoc> &kv : docs->class_list) {
			if (kv.key.to_lower() == lower) {
				cd = &kv.value;
				class_name = kv.key;
				break;
			}
		}
	}
	if (!cd) {
		return make_tool_error("Class not found: " + class_name);
	}

	// Walk inheritance chain to find the member.
	String text;
	Dictionary structured;

	String current_class = class_name;
	for (int depth = 0; depth < 20; depth++) {
		const DocData::ClassDoc *search_cd = docs->class_list.getptr(current_class);
		if (!search_cd) {
			break;
		}

		// Search methods.
		if (member_type.is_empty() || member_type == "method") {
			for (int i = 0; i < search_cd->methods.size(); i++) {
				if (search_cd->methods[i].name == member_name) {
					const DocData::MethodDoc &m = search_cd->methods[i];
					text = "# " + current_class + "." + _format_method_sig(m) + "\n\n";
					if (m.is_deprecated) {
						text += "**DEPRECATED**";
						if (!m.deprecated_message.is_empty()) {
							text += ": " + _bbcode_to_text(m.deprecated_message);
						}
						text += "\n\n";
					}
					if (m.is_experimental) {
						text += "**EXPERIMENTAL**";
						if (!m.experimental_message.is_empty()) {
							text += ": " + _bbcode_to_text(m.experimental_message);
						}
						text += "\n\n";
					}
					if (!m.description.is_empty()) {
						text += _bbcode_to_text(m.description) + "\n";
					}
					structured["class_name"] = current_class;
					structured["member_name"] = m.name;
					structured["member_type"] = "method";
					structured["signature"] = _format_method_sig(m);
					structured["return_type"] = m.return_type;
					return make_tool_result(text, structured);
				}
			}
		}

		// Search signals.
		if (member_type.is_empty() || member_type == "signal") {
			for (int i = 0; i < search_cd->signals.size(); i++) {
				if (search_cd->signals[i].name == member_name) {
					const DocData::MethodDoc &s = search_cd->signals[i];
					text = "# " + current_class + "." + _format_method_sig(s) + " [signal]\n\n";
					if (!s.description.is_empty()) {
						text += _bbcode_to_text(s.description) + "\n";
					}
					structured["class_name"] = current_class;
					structured["member_name"] = s.name;
					structured["member_type"] = "signal";
					structured["signature"] = _format_method_sig(s);
					return make_tool_result(text, structured);
				}
			}
		}

		// Search constants.
		if (member_type.is_empty() || member_type == "constant") {
			for (int i = 0; i < search_cd->constants.size(); i++) {
				if (search_cd->constants[i].name == member_name) {
					const DocData::ConstantDoc &c = search_cd->constants[i];
					text = "# " + current_class + "." + c.name;
					if (c.is_value_valid) {
						text += " = " + c.value;
					}
					text += "\n\n";
					if (!c.enumeration.is_empty()) {
						text += "Enum: " + c.enumeration + "\n\n";
					}
					if (!c.description.is_empty()) {
						text += _bbcode_to_text(c.description) + "\n";
					}
					structured["class_name"] = current_class;
					structured["member_name"] = c.name;
					structured["member_type"] = "constant";
					if (!c.enumeration.is_empty()) {
						structured["enum"] = c.enumeration;
					}
					return make_tool_result(text, structured);
				}
			}
		}

		// Search annotations.
		if (member_type.is_empty() || member_type == "annotation") {
			for (int i = 0; i < search_cd->annotations.size(); i++) {
				if (search_cd->annotations[i].name == member_name) {
					const DocData::MethodDoc &a = search_cd->annotations[i];
					text = "# " + current_class + "." + _format_method_sig(a) + " [annotation]\n\n";
					if (!a.description.is_empty()) {
						text += _bbcode_to_text(a.description) + "\n";
					}
					structured["class_name"] = current_class;
					structured["member_name"] = a.name;
					structured["member_type"] = "annotation";
					return make_tool_result(text, structured);
				}
			}
		}

		// Search properties.
		if (member_type.is_empty() || member_type == "property") {
			for (int i = 0; i < search_cd->properties.size(); i++) {
				if (search_cd->properties[i].name == member_name) {
					const DocData::PropertyDoc &p = search_cd->properties[i];
					text = "# " + current_class + "." + _format_property_sig(p) + "\n\n";
					if (p.is_deprecated) {
						text += "**DEPRECATED**";
						if (!p.deprecated_message.is_empty()) {
							text += ": " + _bbcode_to_text(p.deprecated_message);
						}
						text += "\n\n";
					}
					if (p.is_experimental) {
						text += "**EXPERIMENTAL**";
						if (!p.experimental_message.is_empty()) {
							text += ": " + _bbcode_to_text(p.experimental_message);
						}
						text += "\n\n";
					}
					if (!p.setter.is_empty()) {
						text += "Setter: `" + p.setter + "`\n";
					}
					if (!p.getter.is_empty()) {
						text += "Getter: `" + p.getter + "`\n";
					}
					if (!p.description.is_empty()) {
						text += "\n" + _bbcode_to_text(p.description) + "\n";
					}
					structured["class_name"] = current_class;
					structured["member_name"] = p.name;
					structured["member_type"] = "property";
					structured["type"] = p.type;
					return make_tool_result(text, structured);
				}
			}
		}

		// Move to parent class.
		if (search_cd->inherits.is_empty()) {
			break;
		}
		current_class = search_cd->inherits;
	}

	return make_tool_error("Member '" + member_name + "' not found on class '" + class_name + "' or its ancestors.");
}

// ============================================================================
// Tool: doc/get_property
// ============================================================================

Dictionary MCPDocTools::handle_get_property(const Dictionary &p_args) {
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		return make_tool_error("Documentation not loaded yet.");
	}

	String class_name = String(p_args.get("class_name", "")).strip_edges();
	String property_name = String(p_args.get("property_name", "")).strip_edges();

	if (class_name.is_empty()) {
		return make_tool_error("Missing required parameter: class_name");
	}
	if (property_name.is_empty()) {
		return make_tool_error("Missing required parameter: property_name");
	}

	// Delegate to handle_get_method with member_type=property.
	Dictionary args;
	args["class_name"] = class_name;
	args["member_name"] = property_name;
	args["member_type"] = "property";
	return handle_get_method(args);
}

// ============================================================================
// Tool Registration
// ============================================================================

void MCPDocTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- doc/search_classes ----
	{
		Dictionary props;
		props["query"] = make_prop("string",
				"Search terms (e.g., 'physics body', 'audio', 'tween animation').");
		props["limit"] = make_prop("integer",
				"Maximum number of results to return (default 20, max 50).");
		props["category"] = make_prop("string",
				"Filter by inheritance category: 'node', 'resource', 'refcounted', or empty for all.");
		Array required;
		required.push_back("query");

		p_registry->register_tool("doc/search_classes",
				"Search Godot Classes",
				"Search the Godot " GODOT_VERSION_BRANCH " class reference for classes matching a query.\n"
				"\n"
				"Use this FIRST when you need to find which class provides specific functionality.\n"
				"Returns class names, inheritance, and brief descriptions ranked by relevance.\n"
				"\n"
				"Examples:\n"
				"- 'physics body' -> RigidBody3D, CharacterBody3D, StaticBody3D...\n"
				"- 'audio' -> AudioStreamPlayer, AudioBus, AudioEffect...\n"
				"- 'tween' -> Tween, PropertyTweener, MethodTweener...\n"
				"- 'file' -> FileAccess, DirAccess, EditorFileSystem...\n"
				"- 'http request' -> HTTPRequest, HTTPClient...",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDocTools::handle_search_classes));
	}

	// ---- doc/get_class ----
	{
		Dictionary props;
		props["class_name"] = make_prop("string",
				"Exact class name (e.g., 'Node3D', 'RigidBody3D'). Case-insensitive fallback is supported.");
		props["sections"] = make_prop("string",
				"Comma-separated filter: 'description', 'methods', 'properties', 'signals', "
				"'constants', 'constructors', 'operators', 'annotations', 'theme_properties', "
				"'tutorials'. Default: 'all'.");
		props["include_inherited"] = make_prop("boolean",
				"Include inherited members from parent classes (default false).");
		Array required;
		required.push_back("class_name");

		p_registry->register_tool("doc/get_class",
				"Get Class Documentation",
				"Get complete documentation for a specific Godot " GODOT_VERSION_BRANCH " class.\n"
				"\n"
				"Returns the full class reference including description, methods, properties,\n"
				"signals, constants, and more. Use the 'sections' parameter to limit output\n"
				"for large classes.\n"
				"\n"
				"This is the AUTHORITATIVE source for Godot API -- never guess at method\n"
				"signatures or property types. Always check here before writing code that\n"
				"uses a class you're not 100%% certain about.\n"
				"\n"
				"For very large classes (100+ members), descriptions are automatically\n"
				"summarized. Request specific sections for full detail.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDocTools::handle_get_class));
	}

	// ---- doc/search_methods ----
	{
		Dictionary props;
		props["query"] = make_prop("string",
				"Search terms (e.g., 'get children', 'play animation', 'load scene').");
		props["limit"] = make_prop("integer",
				"Maximum number of results (default 20, max 50).");
		props["class_filter"] = make_prop("string",
				"Restrict search to this class and its ancestors.");
		Array required;
		required.push_back("query");

		p_registry->register_tool("doc/search_methods",
				"Search Methods",
				"Search across ALL Godot " GODOT_VERSION_BRANCH " classes for methods matching a query.\n"
				"\n"
				"Use this when you know WHAT you want to do but not WHICH class has the method.\n"
				"Also searches signals.\n"
				"\n"
				"Examples:\n"
				"- 'play animation' -> AnimationPlayer.play(), AnimationMixer.play()...\n"
				"- 'load scene' -> ResourceLoader.load(), PackedScene.instantiate()...\n"
				"- 'get mouse position' -> Viewport.get_mouse_position()...\n"
				"- 'connect signal' -> Object.connect(), Signal.connect()...",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDocTools::handle_search_methods));
	}

	// ---- doc/get_method ----
	{
		Dictionary props;
		props["class_name"] = make_prop("string", "The class name.");
		props["member_name"] = make_prop("string", "Method, signal, property, constant, or annotation name.");
		props["member_type"] = make_prop("string",
				"Hint: 'method', 'signal', 'property', 'constant', 'annotation'. "
				"Auto-detected if omitted.");
		Array required;
		required.push_back("class_name");
		required.push_back("member_name");

		p_registry->register_tool("doc/get_method",
				"Get Member Documentation",
				"Get detailed documentation for a specific method, signal, constant,\n"
				"annotation, or property on a Godot " GODOT_VERSION_BRANCH " class.\n"
				"\n"
				"Returns the full signature, parameter details, and description.\n"
				"Searches the inheritance chain if the member is defined on a parent class.\n"
				"Use this after doc/search_methods or doc/get_class for complete details.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDocTools::handle_get_method));
	}

	// ---- doc/get_property ----
	{
		Dictionary props;
		props["class_name"] = make_prop("string", "The class name.");
		props["property_name"] = make_prop("string", "The property name.");
		Array required;
		required.push_back("class_name");
		required.push_back("property_name");

		p_registry->register_tool("doc/get_property",
				"Get Property Documentation",
				"Get detailed documentation for a specific property on a Godot " GODOT_VERSION_BRANCH " class.\n"
				"\n"
				"Returns the property type, default value, setter/getter methods, and description.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDocTools::handle_get_property));
	}
}
