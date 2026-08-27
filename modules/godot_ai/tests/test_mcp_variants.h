/**************************************************************************/
/*  test_mcp_variants.h                                                   */
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

#ifndef TEST_MCP_VARIANTS_H
#define TEST_MCP_VARIANTS_H

#include "modules/godot_ai/mcp_variants.h"

#include "tests/test_macros.h"

namespace TestMCPVariants {

static Array three_heights() {
	Array values;
	values.push_back(180.0);
	values.push_back(220.0);
	values.push_back(260.0);
	return values;
}

// Opens a set on /root/Main/Player.jump_height, starting from 120.
static bool open_default(MCPVariants &r_set, String &r_error) {
	return r_set.open_set("/root/Main/Player", "jump_height", 120.0, "120", three_heights(),
			r_error);
}

// --- opening --------------------------------------------------------------------

TEST_CASE("[godot_ai] A set always holds the original as a candidate") {
	// Comparing against what the game already had is the comparison that matters most,
	// and it is the one a designer forgets to make.
	MCPVariants set;
	String error;
	REQUIRE_MESSAGE(open_default(set, error), error);

	CHECK(set.is_open());
	CHECK(set.get_candidate_count() == 4);
	CHECK(set.get_current() == MCPVariants::ORIGINAL_NAME);
	CHECK(set.text_for(MCPVariants::ORIGINAL_NAME) == "120");

	bool found = false;
	CHECK((double)set.value_for(MCPVariants::ORIGINAL_NAME, found) == doctest::Approx(120.0));
	CHECK(found);
}

TEST_CASE("[godot_ai] One value is not a choice") {
	MCPVariants set;
	Array one;
	one.push_back(220.0);
	String error;
	CHECK_FALSE(set.open_set("/root/Main/Player", "jump_height", 120.0, "120", one, error));
	CHECK(error.contains("at least two"));
	// And it points at the tool that does set one value, rather than leaving the caller
	// to work out that this is the wrong tool for the job.
	CHECK(error.contains("Godot_SetRuntimeProperty"));
	CHECK_FALSE(set.is_open());
}

TEST_CASE("[godot_ai] An unnamed candidate is named after its own value") {
	// A name nobody chose is a name nobody has to remember, and "220.0" is a perfectly good
	// name for 220.
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	CHECK(set.text_for("220.0") == "220.0");
	bool found = false;
	set.value_for("260.0", found);
	CHECK(found);
}

TEST_CASE("[godot_ai] A named candidate keeps its name") {
	MCPVariants set;
	Array values;
	Dictionary floaty;
	floaty["name"] = "floaty";
	floaty["value"] = 300.0;
	values.push_back(floaty);
	Dictionary heavy;
	heavy["name"] = "heavy";
	heavy["value"] = 140.0;
	values.push_back(heavy);

	String error;
	REQUIRE_MESSAGE(
			set.open_set("/root/Main/Player", "jump_height", 120.0, "120", values, error), error);
	CHECK(set.text_for("floaty") == "300.0");
	CHECK(set.text_for("heavy") == "140.0");
}

TEST_CASE("[godot_ai] Two candidates cannot share a name, and none may be called 'original'") {
	MCPVariants set;
	String error;

	Array duplicated;
	Dictionary first;
	first["name"] = "floaty";
	first["value"] = 300.0;
	Dictionary second;
	second["name"] = "floaty";
	second["value"] = 320.0;
	duplicated.push_back(first);
	duplicated.push_back(second);
	// Two candidates under one name cannot be told apart afterwards, which defeats the
	// point of naming them at all.
	CHECK_FALSE(set.open_set("/root/Main/Player", "jump_height", 120.0, "120", duplicated, error));
	CHECK(error.contains("floaty"));

	Array reserved;
	Dictionary shadow;
	shadow["name"] = MCPVariants::ORIGINAL_NAME;
	shadow["value"] = 300.0;
	reserved.push_back(shadow);
	reserved.push_back(320.0);
	CHECK_FALSE(set.open_set("/root/Main/Player", "jump_height", 120.0, "120", reserved, error));
	CHECK(error.contains("reserved"));
	// A refused open leaves nothing half-built behind.
	CHECK_FALSE(set.is_open());
	CHECK(set.get_candidate_count() == 0);
}

TEST_CASE("[godot_ai] A structured value survives into the set") {
	// A position is exactly the kind of thing worth tuning live, and it arrives as a JSON
	// array because JSON has no Vector2. The set hands it straight to the runtime, which
	// already knows how to build the real type from it.
	MCPVariants set;
	Array values;
	Array left;
	left.push_back(0);
	left.push_back(64);
	Array right;
	right.push_back(128);
	right.push_back(64);
	values.push_back(left);
	values.push_back(right);

	String error;
	REQUIRE_MESSAGE(set.open_set("/root/Main/Spawn", "position", Vector2(0, 0), "(0, 0)", values,
							error),
			error);
	// The candidate arrives, and comes back out unchanged: nothing here tries to be clever
	// about the type, because `set_property` in the running game already is.
	const Dictionary summary = set.summary();
	const Array candidates = summary["candidates"];
	REQUIRE(candidates.size() == 3);
	const Dictionary second = candidates[2];
	const Variant kept = second["value"];
	CHECK(kept.get_type() == Variant::ARRAY);
	CHECK((int)Array(kept)[0] == 128);
	// And the original's own text, which is a Vector2, survived as text rather than being
	// flattened through JSON on the way in.
	CHECK(String(summary["original"]) == "(0, 0)");
}

// --- switching and the clock -----------------------------------------------------

TEST_CASE("[godot_ai] Time is banked against the candidate that was live") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));

	REQUIRE(set.switch_to("180.0", 1000, error));
	REQUIRE(set.switch_to("220.0", 4000, error));
	set.close(9000);

	const Dictionary summary = set.summary();
	const Array candidates = summary["candidates"];
	for (int i = 0; i < candidates.size(); i++) {
		const Dictionary entry = candidates[i];
		const String name = entry["name"];
		if (name == "180.0") {
			CHECK((int64_t)entry["msec_live"] == 3000);
		} else if (name == "220.0") {
			CHECK((int64_t)entry["msec_live"] == 5000);
		} else {
			// The original was current before the first switch, but the clock only starts
			// when something is applied - nothing was, so it has no time against it.
			CHECK((int64_t)entry["msec_live"] == 0);
		}
	}
	CHECK(String(summary["current"]) == "220.0");
}

TEST_CASE("[godot_ai] Switching back to a candidate adds to its time rather than replacing it") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("220.0", 0, error));
	REQUIRE(set.switch_to("260.0", 2000, error));
	REQUIRE(set.switch_to("220.0", 3000, error));
	set.close(4000);

	const Array candidates = Dictionary(set.summary())["candidates"];
	for (int i = 0; i < candidates.size(); i++) {
		const Dictionary entry = candidates[i];
		if (String(entry["name"]) == "220.0") {
			CHECK((int64_t)entry["msec_live"] == 3000);
			CHECK((int)entry["times_applied"] == 2);
		}
	}
}

TEST_CASE("[godot_ai] Switching to something that is not in the set names what is") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	CHECK_FALSE(set.switch_to("400", 0, error));
	CHECK(error.contains("400"));
	CHECK(error.contains("220.0"));
	// Including the original, which is the one a caller is least likely to know is there.
	CHECK(error.contains(MCPVariants::ORIGINAL_NAME));
}

// --- notes ------------------------------------------------------------------------

TEST_CASE("[godot_ai] A note without a name lands on whatever is live") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("260.0", 0, error));
	REQUIRE(set.add_note(String(), "overshoots the platform", error));

	const Array candidates = Dictionary(set.summary())["candidates"];
	for (int i = 0; i < candidates.size(); i++) {
		const Dictionary entry = candidates[i];
		if (String(entry["name"]) == "260.0") {
			CHECK(String(entry["note"]) == "overshoots the platform");
		} else {
			CHECK_FALSE(entry.has("note"));
		}
	}
}

TEST_CASE("[godot_ai] A second look appends rather than overwrites") {
	// The change of mind is the most interesting thing in the record, and replacing the
	// first note would lose it.
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("220.0", 0, error));
	REQUIRE(set.add_note(String(), "feels right", error));
	REQUIRE(set.add_note("220.0", "still right after the second gap", error));

	const Array candidates = Dictionary(set.summary())["candidates"];
	for (int i = 0; i < candidates.size(); i++) {
		const Dictionary entry = candidates[i];
		if (String(entry["name"]) == "220.0") {
			CHECK(String(entry["note"]).contains("feels right"));
			CHECK(String(entry["note"]).contains("second gap"));
		}
	}
}

// --- what may be kept ---------------------------------------------------------------

TEST_CASE("[godot_ai] A value nobody played cannot be kept") {
	// Keeping a value that was never live is editing the scene by a longer route, and
	// calling it the result of a comparison would be a claim about something that did not
	// happen.
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("220.0", 0, error));

	CHECK(set.can_keep("220.0", error));
	CHECK_FALSE(set.can_keep("260.0", error));
	CHECK(error.contains("never applied"));
	// And it says what to do instead, rather than only refusing.
	CHECK(error.contains("Godot_SetSceneProperty"));
}

TEST_CASE("[godot_ai] The original may be kept, once it has been played against") {
	// "I tried three and the one it already had was best" is a real outcome, and a
	// perfectly good result for a tuning session.
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	CHECK_FALSE(set.can_keep(MCPVariants::ORIGINAL_NAME, error));
	REQUIRE(set.switch_to("220.0", 0, error));
	REQUIRE(set.switch_to(MCPVariants::ORIGINAL_NAME, 1000, error));
	CHECK(set.can_keep(MCPVariants::ORIGINAL_NAME, error));
}

TEST_CASE("[godot_ai] Keeping from a set that is not open is refused, not silently allowed") {
	MCPVariants set;
	String error;
	CHECK_FALSE(set.can_keep("220.0", error));
	CHECK(error.contains("no tuning set"));
	CHECK_FALSE(set.switch_to("220.0", 0, error));
	CHECK_FALSE(set.add_note(String(), "anything", error));
}

// --- what the set says about itself ---------------------------------------------------

TEST_CASE("[godot_ai] A set that flipped through its values says it recorded a choice") {
	// Five numbers in a second is not tuning. The tool holds the clock, so it is the one
	// thing here that can say so, and it says it rather than refusing - overruling the
	// person tuning is not its job.
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("180.0", 0, error));
	REQUIRE(set.switch_to("220.0", 100, error));
	REQUIRE(set.switch_to("260.0", 200, error));
	set.close(300);

	const String comparison = set.describe_comparison();
	CHECK(comparison.contains("choice rather than a comparison"));
	CHECK(comparison.contains("500"));

	const Array candidates = Dictionary(set.summary())["candidates"];
	int flagged = 0;
	for (int i = 0; i < candidates.size(); i++) {
		flagged += (bool)Dictionary(candidates[i]).get("only_briefly", false) ? 1 : 0;
	}
	CHECK(flagged == 3);
}

TEST_CASE("[godot_ai] A set that was actually played says so plainly") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("180.0", 0, error));
	REQUIRE(set.switch_to("220.0", 20000, error));
	set.close(45000);

	const String comparison = set.describe_comparison();
	CHECK(comparison.contains("2 of 4"));
	CHECK(comparison.contains("long enough to judge"));
	CHECK_FALSE(comparison.contains("rather than a comparison"));
}

TEST_CASE("[godot_ai] One value played is not a comparison, and neither is none") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	CHECK(set.describe_comparison().contains("Nothing has been tried"));

	REQUIRE(set.switch_to("220.0", 0, error));
	set.close(30000);
	// Thirty seconds on one value is a good long look at one value. It is still not a
	// comparison, and the sentence has to say which of the two it is.
	CHECK(set.describe_comparison().contains("nothing to compare"));
}

TEST_CASE("[godot_ai] Resetting a set forgets it entirely") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	REQUIRE(set.switch_to("220.0", 0, error));
	set.reset();

	CHECK_FALSE(set.is_open());
	CHECK(set.get_candidate_count() == 0);
	CHECK(set.get_runtime_path().is_empty());
	// And a second set opens cleanly on top of it, carrying nothing across.
	REQUIRE(set.open_set("/root/Main/Enemy", "speed", 40.0, "40", three_heights(), error));
	CHECK(set.get_runtime_path() == "/root/Main/Enemy");
	CHECK(set.get_current() == MCPVariants::ORIGINAL_NAME);
	CHECK(set.get_candidate_count() == 4);
}

TEST_CASE("[godot_ai] A second set cannot be opened over an open one") {
	MCPVariants set;
	String error;
	REQUIRE(open_default(set, error));
	// There is one running game, so there is one thing being tuned in it.
	CHECK_FALSE(set.open_set("/root/Main/Enemy", "speed", 40.0, "40", three_heights(), error));
	CHECK(error.contains("jump_height"));
}

} // namespace TestMCPVariants

#endif // TEST_MCP_VARIANTS_H
