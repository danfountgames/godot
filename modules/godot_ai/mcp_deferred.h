/**************************************************************************/
/*  mcp_deferred.h                                                        */
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

#ifndef MCP_DEFERRED_H
#define MCP_DEFERRED_H

#include "mcp_types.h"

#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

// Support for tools that cannot answer immediately.
//
// Everything else in this module runs to completion inside one call on the editor's
// main thread. A tool that has to wait for the *user* cannot do that: a modal needs
// the main loop to keep running, so blocking in the tool would freeze the editor and
// the dialog it just opened.
//
// Such a tool instead claims a token, starts its work, and returns
// `{ "_deferred": <token> }`. The service holds the client's request id, keeps
// polling, and sends the response when the token completes or its deadline passes.
class MCPDeferred {
public:
	typedef int64_t Token;
	// constexpr, not const: doctest takes it by reference, which would otherwise
	// need an out-of-line definition.
	static constexpr Token INVALID_TOKEN = 0;

	struct Completion {
		Dictionary result;
		MCPToolError error;
	};

	// Claims a token. p_timeout_seconds bounds how long the client will wait; 0 means
	// the caller takes responsibility for completing it.
	static Token begin(double p_timeout_seconds);

	// Completes a token. Ignored when the token is unknown, which happens when a
	// client disconnected or the deadline already passed.
	static void complete(Token p_token, const Dictionary &p_result);
	static void fail(Token p_token, MCPToolError::Kind p_kind, const String &p_message);

	// True when the token has an answer waiting. Fills r_completion and forgets it.
	static bool take(Token p_token, Completion &r_completion);

	// True when the token is still waiting for an answer.
	static bool is_pending(Token p_token);

	// Fails every token whose deadline has passed. Called from the service's poll.
	static void expire_overdue();

	// Drops a token without answering, for a client that has gone away.
	static void abandon(Token p_token);

	// The dictionary a tool returns to defer. Kept here so the marker cannot drift
	// between the tool side and the protocol side.
	static Dictionary make_deferred_result(Token p_token);
	static bool get_deferred_token(const Dictionary &p_result, Token &r_token);

	// Test seam.
	static void reset();
	static int get_pending_count();
};

#endif // MCP_DEFERRED_H
