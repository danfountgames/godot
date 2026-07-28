/**************************************************************************/
/*  mcp_deferred.cpp                                                      */
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

#include "mcp_deferred.h"

#include "core/os/time.h"

namespace {

struct Entry {
	bool answered = false;
	double deadline = 0.0; // 0 means no deadline.
	MCPDeferred::Completion completion;
};

MCPDeferred::Token s_next_token = 1;
HashMap<MCPDeferred::Token, Entry> s_entries;

double now_seconds() {
	return (double)Time::get_singleton()->get_ticks_msec() / 1000.0;
}

} // namespace

MCPDeferred::Token MCPDeferred::begin(double p_timeout_seconds) {
	const Token token = s_next_token++;
	Entry entry;
	entry.deadline = p_timeout_seconds > 0.0 ? now_seconds() + p_timeout_seconds : 0.0;
	s_entries[token] = entry;
	return token;
}

void MCPDeferred::complete(Token p_token, const Dictionary &p_result) {
	Entry *entry = s_entries.getptr(p_token);
	if (!entry || entry->answered) {
		// Late answers are dropped rather than queued: the client has already been
		// told the call timed out, and answering twice would break the protocol.
		return;
	}
	entry->answered = true;
	entry->completion.result = p_result;
	entry->completion.error.clear();
}

void MCPDeferred::fail(Token p_token, MCPToolError::Kind p_kind, const String &p_message) {
	Entry *entry = s_entries.getptr(p_token);
	if (!entry || entry->answered) {
		return;
	}
	entry->answered = true;
	entry->completion.result = Dictionary();
	entry->completion.error.set(p_kind, p_message);
}

bool MCPDeferred::take(Token p_token, Completion &r_completion) {
	Entry *entry = s_entries.getptr(p_token);
	if (!entry || !entry->answered) {
		return false;
	}
	r_completion = entry->completion;
	s_entries.erase(p_token);
	return true;
}

bool MCPDeferred::is_pending(Token p_token) {
	const Entry *entry = s_entries.getptr(p_token);
	return entry && !entry->answered;
}

void MCPDeferred::expire_overdue() {
	const double now = now_seconds();
	for (KeyValue<Token, Entry> &pair : s_entries) {
		if (pair.value.answered || pair.value.deadline <= 0.0 || now < pair.value.deadline) {
			continue;
		}
		pair.value.answered = true;
		pair.value.completion.result = Dictionary();
		pair.value.completion.error.set(MCPToolError::FAILED,
				"timed out waiting for the user to answer");
	}
}

void MCPDeferred::abandon(Token p_token) {
	s_entries.erase(p_token);
}

Dictionary MCPDeferred::make_deferred_result(Token p_token) {
	Dictionary result;
	result["_deferred"] = p_token;
	return result;
}

bool MCPDeferred::get_deferred_token(const Dictionary &p_result, Token &r_token) {
	if (!p_result.has("_deferred")) {
		return false;
	}
	r_token = (int64_t)p_result["_deferred"];
	return r_token != INVALID_TOKEN;
}

void MCPDeferred::reset() {
	s_entries.clear();
	s_next_token = 1;
}

int MCPDeferred::get_pending_count() {
	int count = 0;
	for (const KeyValue<Token, Entry> &pair : s_entries) {
		if (!pair.value.answered) {
			count++;
		}
	}
	return count;
}
