/**************************************************************************/
/*  test_mcp_deferred.h                                                   */
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

#ifndef TEST_MCP_DEFERRED_H
#define TEST_MCP_DEFERRED_H

#include "modules/godot_ai/mcp_deferred.h"

#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPDeferred {

TEST_CASE("[godot_ai] Deferred calls are answered exactly once") {
	MCPDeferred::reset();

	const MCPDeferred::Token token = MCPDeferred::begin(0.0);
	CHECK(token != MCPDeferred::INVALID_TOKEN);
	CHECK(MCPDeferred::is_pending(token));

	MCPDeferred::Completion completion;
	// Nothing to take until it completes: polling early must not consume it.
	CHECK_FALSE(MCPDeferred::take(token, completion));

	Dictionary answer;
	answer["answer"] = "yes";
	MCPDeferred::complete(token, answer);
	CHECK_FALSE(MCPDeferred::is_pending(token));

	REQUIRE(MCPDeferred::take(token, completion));
	CHECK_FALSE(completion.error.has_error());
	CHECK(String(completion.result["answer"]) == "yes");

	// Taken once and forgotten, so a second poll cannot send a duplicate response.
	CHECK_FALSE(MCPDeferred::take(token, completion));
	MCPDeferred::reset();
}

TEST_CASE("[godot_ai] A late answer is dropped rather than sent twice") {
	MCPDeferred::reset();

	const MCPDeferred::Token token = MCPDeferred::begin(0.0);
	MCPDeferred::fail(token, MCPToolError::FAILED, "first answer");

	Dictionary late;
	late["answer"] = "too late";
	MCPDeferred::complete(token, late);

	MCPDeferred::Completion completion;
	REQUIRE(MCPDeferred::take(token, completion));
	// The first answer wins: the client has already been told this outcome.
	CHECK(completion.error.has_error());
	CHECK(completion.error.message == "first answer");
	MCPDeferred::reset();
}

TEST_CASE("[godot_ai] Overdue calls fail instead of hanging the client") {
	MCPDeferred::reset();

	// The deadline clock has millisecond resolution, so give it time to actually
	// pass rather than asserting on a sub-millisecond difference.
	const MCPDeferred::Token token = MCPDeferred::begin(0.001);
	OS::get_singleton()->delay_usec(5000);
	MCPDeferred::expire_overdue();

	MCPDeferred::Completion completion;
	REQUIRE(MCPDeferred::take(token, completion));
	CHECK(completion.error.has_error());
	CHECK(completion.error.message.contains("timed out"));

	// A token with no deadline is the caller's responsibility and must survive.
	const MCPDeferred::Token forever = MCPDeferred::begin(0.0);
	MCPDeferred::expire_overdue();
	CHECK(MCPDeferred::is_pending(forever));
	MCPDeferred::reset();
}

TEST_CASE("[godot_ai] Abandoned calls leave nothing behind") {
	MCPDeferred::reset();

	const MCPDeferred::Token token = MCPDeferred::begin(0.0);
	// The client disconnected: completing afterwards must not resurrect it.
	MCPDeferred::abandon(token);
	CHECK_FALSE(MCPDeferred::is_pending(token));

	MCPDeferred::complete(token, Dictionary());
	MCPDeferred::Completion completion;
	CHECK_FALSE(MCPDeferred::take(token, completion));
	CHECK(MCPDeferred::get_pending_count() == 0);
	MCPDeferred::reset();
}

TEST_CASE("[godot_ai] The deferral marker round-trips") {
	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	CHECK_FALSE(MCPDeferred::get_deferred_token(Dictionary(), token));

	Dictionary ordinary;
	ordinary["answer"] = "no token here";
	CHECK_FALSE(MCPDeferred::get_deferred_token(ordinary, token));

	const Dictionary deferred = MCPDeferred::make_deferred_result(42);
	REQUIRE(MCPDeferred::get_deferred_token(deferred, token));
	CHECK(token == 42);
}

} // namespace TestMCPDeferred

#endif // TEST_MCP_DEFERRED_H
