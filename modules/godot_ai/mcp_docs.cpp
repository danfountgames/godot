/**************************************************************************/
/*  mcp_docs.cpp                                                          */
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

#include "mcp_docs.h"

#include "core/variant/array.h"

#ifdef TOOLS_ENABLED
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "editor/doc/doc_tools.h"
#include "editor/doc/editor_help.h"
#endif

// ------------------------------------------------------------------ helpers ---

bool MCPDocs::matches(const String &p_haystack, const String &p_needle) {
	if (p_needle.is_empty()) {
		return true;
	}
	return p_haystack.findn(p_needle) >= 0;
}

String MCPDocs::strip_bbcode(const String &p_text) {
	String out;
	int i = 0;
	while (i < p_text.length()) {
		if (p_text[i] != '[') {
			out += String::chr(p_text[i]);
			i++;
			continue;
		}
		const int close = p_text.find_char(']', i);
		if (close < 0) {
			// An unmatched bracket is text, not markup.
			out += p_text.substr(i);
			break;
		}
		const String tag = p_text.substr(i + 1, close - i - 1);
		i = close + 1;

		if (tag.is_empty()) {
			continue;
		}
		// `[Node]`, `[method x]`, `[member y]`, `[param p]` all name something the
		// reader needs; the name survives and only the markup goes.
		const int space = tag.find_char(' ');
		if (space > 0) {
			const String head = tag.substr(0, space);
			if (head == "param" || head == "method" || head == "member" || head == "signal" ||
					head == "constant" || head == "enum" || head == "theme_item" ||
					head == "annotation" || head == "constructor" || head == "operator") {
				out += tag.substr(space + 1);
			}
			continue;
		}
		// A bare tag is either formatting (`[b]`, `[/code]`) or a class link
		// (`[Node2D]`). Formatting tags are lowercase or closing; a class name is
		// capitalised, so it is kept.
		if (tag.begins_with("/")) {
			continue;
		}
		const char32_t first = tag[0];
		if (first >= 'A' && first <= 'Z') {
			out += tag;
		}
	}
	return out;
}

String MCPDocs::brief_of(const String &p_description) {
	const String text = strip_bbcode(p_description).strip_edges();
	if (text.is_empty()) {
		return String();
	}
	// First sentence, or first line, whichever comes first.
	int end = text.length();
	const int newline = text.find_char('\n');
	if (newline >= 0) {
		end = newline;
	}
	const int period = text.find(". ");
	if (period >= 0 && period + 1 < end) {
		end = period + 1;
	}
	return text.substr(0, end).strip_edges();
}

String MCPDocs::signature(const DocData::MethodDoc &p_method) {
	String out = p_method.name + "(";
	for (int i = 0; i < p_method.arguments.size(); i++) {
		const DocData::ArgumentDoc &argument = p_method.arguments[i];
		if (i > 0) {
			out += ", ";
		}
		out += argument.name + ": " + argument.type;
		if (!argument.default_value.is_empty()) {
			out += " = " + argument.default_value;
		}
	}
	out += ")";
	if (!p_method.return_type.is_empty() && p_method.return_type != "void") {
		out += " -> " + p_method.return_type;
	}
	if (!p_method.qualifiers.is_empty()) {
		out += " " + p_method.qualifiers;
	}
	return out;
}

String MCPDocs::describe_api_type(const String &p_api_type) {
	if (p_api_type == "core") {
		return "core";
	}
	if (p_api_type == "editor") {
		return "editor";
	}
	if (p_api_type == "extension" || p_api_type == "editor_extension") {
		return "extension";
	}
	return p_api_type.is_empty() ? "unknown" : p_api_type;
}

// ------------------------------------------------------------------- lookup ---

#ifdef TOOLS_ENABLED

namespace {

Dictionary member_entry(const String &p_kind, const String &p_name, const String &p_signature,
		const String &p_type, const String &p_description, bool p_full,
		bool p_deprecated, bool p_experimental) {
	Dictionary out;
	out["kind"] = p_kind;
	out["name"] = p_name;
	if (!p_signature.is_empty()) {
		out["signature"] = p_signature;
	}
	if (!p_type.is_empty()) {
		out["type"] = p_type;
	}
	if (p_full) {
		out["description"] = MCPDocs::strip_bbcode(p_description).strip_edges();
	} else {
		const String brief = MCPDocs::brief_of(p_description);
		if (!brief.is_empty()) {
			out["brief"] = brief;
		}
	}
	// Worth carrying even in a listing: an agent that picks a deprecated method
	// writes code that works today and warns forever.
	if (p_deprecated) {
		out["deprecated"] = true;
	}
	if (p_experimental) {
		out["experimental"] = true;
	}
	return out;
}

void collect_members(const DocData::ClassDoc &p_class, const String &p_search, const String &p_member,
		bool p_full, Array &r_out) {
	const bool exact = !p_member.is_empty();

	for (const DocData::MethodDoc &method : p_class.methods) {
		if (exact ? method.name == p_member : MCPDocs::matches(method.name, p_search)) {
			r_out.push_back(member_entry("method", method.name, MCPDocs::signature(method),
					method.return_type, method.description, p_full || exact,
					method.is_deprecated, method.is_experimental));
		}
	}
	for (const DocData::PropertyDoc &property : p_class.properties) {
		if (exact ? property.name == p_member : MCPDocs::matches(property.name, p_search)) {
			Dictionary entry = member_entry("property", property.name, String(), property.type,
					property.description, p_full || exact,
					property.is_deprecated, property.is_experimental);
			if (!property.default_value.is_empty()) {
				entry["default"] = property.default_value;
			}
			// The setter and getter are how a property is reached from code that
			// cannot use property syntax, which includes every tool call here.
			if (!property.setter.is_empty()) {
				entry["setter"] = property.setter;
			}
			if (!property.getter.is_empty()) {
				entry["getter"] = property.getter;
			}
			r_out.push_back(entry);
		}
	}
	for (const DocData::MethodDoc &signal : p_class.signals) {
		if (exact ? signal.name == p_member : MCPDocs::matches(signal.name, p_search)) {
			r_out.push_back(member_entry("signal", signal.name, MCPDocs::signature(signal),
					String(), signal.description, p_full || exact,
					signal.is_deprecated, signal.is_experimental));
		}
	}
	for (const DocData::ConstantDoc &constant : p_class.constants) {
		if (exact ? constant.name == p_member : MCPDocs::matches(constant.name, p_search)) {
			Dictionary entry = member_entry("constant", constant.name, String(),
					constant.enumeration.is_empty() ? constant.type : constant.enumeration,
					constant.description, p_full || exact,
					constant.is_deprecated, constant.is_experimental);
			if (!constant.value.is_empty()) {
				entry["value"] = constant.value;
			}
			r_out.push_back(entry);
		}
	}
}

// Documentation for one of the project's own script classes, read from the script
// itself rather than from the editor's cache.
//
// The cache cannot be relied on here. Godot skips loading script documentation
// entirely when the editor is in command-line mode - which is decided by the display
// server being headless - so in exactly the run an agent is most likely to be driving,
// the project's own classes are absent from DocTools and every doc added on top of it
// is queued behind a flag that never gets set. Going to the script is the same data by
// a shorter route, it needs no engine change, and it also picks up a class written
// seconds ago in an interactive session before any rescan has caught up.
bool script_class_doc(const String &p_class_name, DocData::ClassDoc &r_doc) {
	if (!ScriptServer::is_global_class(p_class_name)) {
		return false;
	}
	const String path = ScriptServer::get_global_class_path(p_class_name);
	if (path.is_empty()) {
		return false;
	}
	Ref<Script> script = ResourceLoader::load(path, "Script");
	if (script.is_null()) {
		return false;
	}
	for (const DocData::ClassDoc &candidate : script->get_documentation()) {
		if (candidate.name == p_class_name) {
			r_doc = candidate;
			r_doc.is_script_doc = true;
			if (r_doc.script_path.is_empty()) {
				r_doc.script_path = path;
			}
			if (r_doc.inherits.is_empty()) {
				r_doc.inherits = ScriptServer::get_global_class_native_base(p_class_name);
			}
			return true;
		}
	}
	// A class with no doc comments still exists and still has a base, and saying so
	// beats reporting that the class does not exist.
	r_doc = DocData::ClassDoc();
	r_doc.name = p_class_name;
	r_doc.is_script_doc = true;
	r_doc.script_path = path;
	r_doc.inherits = ScriptServer::get_global_class_native_base(p_class_name);
	return true;
}

} // namespace

MCPDocs::Result MCPDocs::lookup(const Query &p_query, Dictionary &r_out, String &r_error) {
	// The member case is checked first because it is the more specific mistake: a
	// caller who named one wants to be told what is missing from *their* query, not
	// the generic "name a class or a search".
	if (!p_query.member.is_empty() && p_query.class_name.is_empty()) {
		r_error = vformat("'%s' is a member of something; name the class it belongs to", p_query.member);
		return RESULT_INVALID;
	}
	if (p_query.class_name.is_empty() && p_query.search.is_empty()) {
		r_error = "name a class, or give something to search for";
		return RESULT_INVALID;
	}

	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		r_error = "this build has no class reference loaded";
		return RESULT_UNAVAILABLE;
	}

	// --- Searching for classes. ---
	if (p_query.class_name.is_empty()) {
		Array found;
		int total = 0;
		// Two passes rather than one merged map: the class list holds 800-odd entries
		// and copying it to add a handful of script classes would cost more than the
		// search itself.
		auto consider = [&](const DocData::ClassDoc &doc) {
			if (!matches(doc.name, p_query.search) &&
					!matches(doc.brief_description, p_query.search) &&
					!matches(doc.keywords, p_query.search)) {
				return;
			}
			total++;
			if (found.size() >= p_query.limit) {
				return;
			}
			Dictionary summary;
			summary["class_name"] = doc.name;
			summary["inherits"] = doc.inherits;
			summary["api_type"] = doc.is_script_doc ? "project script" : describe_api_type(doc.api_type);
			if (!doc.script_path.is_empty()) {
				summary["script_path"] = doc.script_path;
			}
			const String brief = brief_of(doc.brief_description.is_empty() ? doc.description : doc.brief_description);
			if (!brief.is_empty()) {
				summary["brief"] = brief;
			}
			found.push_back(summary);
		};

		for (const KeyValue<String, DocData::ClassDoc> &entry : docs->class_list) {
			consider(entry.value);
		}
		// The project's own classes, for the runs where the cache does not carry them.
		LocalVector<StringName> global_classes;
		ScriptServer::get_global_class_list(global_classes);
		for (const StringName &global : global_classes) {
			if (docs->class_list.has(global)) {
				continue;
			}
			DocData::ClassDoc from_script;
			if (script_class_doc(global, from_script)) {
				consider(from_script);
			}
		}
		if (total == 0) {
			r_error = vformat("no class matches '%s'", p_query.search);
			return RESULT_NOT_FOUND;
		}
		r_out["classes"] = found;
		r_out["total"] = total;
		r_out["truncated"] = total > found.size();
		if (total > found.size()) {
			r_out["note"] = vformat("%d classes matched and %d are listed; narrow the search or raise the limit.",
					total, found.size());
		}
		return RESULT_OK;
	}

	// --- Looking up one class. ---
	const DocData::ClassDoc *doc = docs->class_list.getptr(p_query.class_name);
	DocData::ClassDoc from_script;
	if (!doc && script_class_doc(p_query.class_name, from_script)) {
		doc = &from_script;
	}
	if (!doc) {
		// A near miss is the common case - wrong case, or a remembered name from
		// another engine version - so the refusal offers candidates rather than just
		// saying no.
		Array suggestions;
		for (const KeyValue<String, DocData::ClassDoc> &entry : docs->class_list) {
			if (suggestions.size() < 8 && matches(entry.key, p_query.class_name)) {
				suggestions.push_back(entry.key);
			}
		}
		LocalVector<StringName> global_classes;
		ScriptServer::get_global_class_list(global_classes);
		for (const StringName &global : global_classes) {
			if (suggestions.size() < 8 && matches(global, p_query.class_name) &&
					!suggestions.has(String(global))) {
				suggestions.push_back(String(global));
			}
		}
		r_error = vformat("there is no class named '%s'", p_query.class_name);
		if (!suggestions.is_empty()) {
			String listed;
			for (int i = 0; i < suggestions.size(); i++) {
				listed += (i ? ", " : "") + String(suggestions[i]);
			}
			r_error += vformat(". Did you mean: %s", listed);
		}
		return RESULT_NOT_FOUND;
	}

	r_out["class_name"] = doc->name;
	r_out["inherits"] = doc->inherits;
	r_out["api_type"] = doc->is_script_doc ? "project script" : describe_api_type(doc->api_type);
	if (!doc->script_path.is_empty()) {
		// For a class the user wrote, the file is more useful than any description.
		r_out["script_path"] = doc->script_path;
	}
	r_out["brief"] = brief_of(doc->brief_description.is_empty() ? doc->description : doc->brief_description);
	if (p_query.member.is_empty() && p_query.search.is_empty()) {
		r_out["description"] = strip_bbcode(doc->description).strip_edges();
	}

	Array chain;
	{
		String base = doc->inherits;
		// Bounded by the map, not by trust: a malformed doc set with a cycle in it
		// must not spin here.
		int guard = 0;
		while (!base.is_empty() && guard++ < 32) {
			chain.push_back(base);
			const DocData::ClassDoc *parent = docs->class_list.getptr(base);
			if (!parent) {
				break;
			}
			base = parent->inherits;
		}
	}
	r_out["inheritance_chain"] = chain;

	Array members;
	collect_members(*doc, p_query.search, p_query.member, false, members);

	if (p_query.include_inherited) {
		for (int i = 0; i < chain.size(); i++) {
			const DocData::ClassDoc *parent = docs->class_list.getptr(String(chain[i]));
			if (!parent) {
				continue;
			}
			Array inherited;
			collect_members(*parent, p_query.search, p_query.member, false, inherited);
			for (int j = 0; j < inherited.size(); j++) {
				Dictionary entry = inherited[j];
				entry["inherited_from"] = parent->name;
				members.push_back(entry);
			}
		}
	}

	if (!p_query.member.is_empty() && members.is_empty()) {
		r_error = vformat("'%s' has no member named '%s'%s", doc->name, p_query.member,
				p_query.include_inherited ? "" : " (pass include_inherited to search its base classes)");
		return RESULT_NOT_FOUND;
	}

	const int total = members.size();
	if (total > p_query.limit) {
		members.resize(p_query.limit);
		r_out["note"] = vformat(
				"%s has %d matching members and %d are listed. Pass 'search' to narrow them, or "
				"'member' for one in full.",
				doc->name, total, p_query.limit);
	}
	r_out["members"] = members;
	r_out["total"] = total;
	r_out["truncated"] = total > members.size();

	if (total == 0 && p_query.search.is_empty() && !p_query.include_inherited && !chain.is_empty()) {
		// A leaf class with nothing of its own is confusing without this: everything
		// it can do came from somewhere up the chain.
		r_out["note"] = vformat("%s declares no members of its own; pass include_inherited to see "
								"what it gets from %s.",
				doc->name, String(chain[0]));
	}
	return RESULT_OK;
}

#else // !TOOLS_ENABLED

MCPDocs::Result MCPDocs::lookup(const Query &p_query, Dictionary &r_out, String &r_error) {
	r_error = "the class reference is only available in an editor build";
	return RESULT_UNAVAILABLE;
}

#endif // TOOLS_ENABLED
