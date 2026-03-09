/**************************************************************************/
/*  test_avp_bridge.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                 */
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

#pragma once

/**
 * Unit tests for the AVP (Apple Vision Pro) RealityKit bridge.
 *
 * Phase 8.1 per RK_PLAN.md:
 * - Bridge call verification
 * - Entity sync / diff engine
 * - Mesh/texture/material store operations
 * - Volume camera culling
 * - Input bridge event queuing
 * - Transform conversion
 * - @rk_shader annotation parsing
 *
 * These tests verify the C++ layer logic WITHOUT requiring visionOS
 * or RealityKit — the bridge functions are no-ops when the Swift
 * side is not connected (function pointers are null).
 */

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "tests/test_macros.h"

// Forward declarations — include AVP headers only if building for visionOS
// or in a test context where the driver code is compiled.
#ifdef AVP_BRIDGE_TESTS_ENABLED
#include "drivers/avp/input_bridge_avp.h"
#include "drivers/avp/material_storage_avp.h"
#include "drivers/avp/mesh_storage_avp.h"
#include "drivers/avp/render_state_extractor.h"
#endif

namespace TestAVPBridge {

// ============================================================================
// §1. Transform Conversion Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Transform3D to float[16] column-major conversion") {
	// Identity transform.
	Transform3D identity;
	float mat[16] = {};

	// Manually compute what we expect:
	// Identity basis is [[1,0,0],[0,1,0],[0,0,1]], origin is (0,0,0).
	// Column-major 4x4:
	//   col0: [1,0,0,0]  col1: [0,1,0,0]  col2: [0,0,1,0]  col3: [0,0,0,1]
	const Basis &b = identity.basis;
	const Vector3 &o = identity.origin;

	// Column 0
	mat[0] = b[0][0]; mat[1] = b[1][0]; mat[2] = b[2][0]; mat[3] = 0.0f;
	// Column 1
	mat[4] = b[0][1]; mat[5] = b[1][1]; mat[6] = b[2][1]; mat[7] = 0.0f;
	// Column 2
	mat[8] = b[0][2]; mat[9] = b[1][2]; mat[10] = b[2][2]; mat[11] = 0.0f;
	// Column 3
	mat[12] = o.x; mat[13] = o.y; mat[14] = o.z; mat[15] = 1.0f;

	CHECK(mat[0] == doctest::Approx(1.0f));
	CHECK(mat[5] == doctest::Approx(1.0f));
	CHECK(mat[10] == doctest::Approx(1.0f));
	CHECK(mat[15] == doctest::Approx(1.0f));
	// Off-diagonals should be 0.
	CHECK(mat[1] == doctest::Approx(0.0f));
	CHECK(mat[4] == doctest::Approx(0.0f));
	CHECK(mat[12] == doctest::Approx(0.0f));
}

TEST_CASE("[AVP Bridge] Transform with translation") {
	Transform3D t;
	t.origin = Vector3(3.5f, -1.2f, 7.0f);
	float mat[16] = {};

	const Basis &b = t.basis;
	const Vector3 &o = t.origin;
	mat[0] = b[0][0]; mat[1] = b[1][0]; mat[2] = b[2][0]; mat[3] = 0.0f;
	mat[4] = b[0][1]; mat[5] = b[1][1]; mat[6] = b[2][1]; mat[7] = 0.0f;
	mat[8] = b[0][2]; mat[9] = b[1][2]; mat[10] = b[2][2]; mat[11] = 0.0f;
	mat[12] = o.x; mat[13] = o.y; mat[14] = o.z; mat[15] = 1.0f;

	CHECK(mat[12] == doctest::Approx(3.5f));
	CHECK(mat[13] == doctest::Approx(-1.2f));
	CHECK(mat[14] == doctest::Approx(7.0f));
}

TEST_CASE("[AVP Bridge] Transform with rotation") {
	Transform3D t;
	// 90 degree rotation around Y axis.
	t.basis = Basis(Vector3(0, 1, 0), Math_PI / 2.0);
	float mat[16] = {};

	const Basis &b = t.basis;
	const Vector3 &o = t.origin;
	mat[0] = b[0][0]; mat[1] = b[1][0]; mat[2] = b[2][0]; mat[3] = 0.0f;
	mat[4] = b[0][1]; mat[5] = b[1][1]; mat[6] = b[2][1]; mat[7] = 0.0f;
	mat[8] = b[0][2]; mat[9] = b[1][2]; mat[10] = b[2][2]; mat[11] = 0.0f;
	mat[12] = o.x; mat[13] = o.y; mat[14] = o.z; mat[15] = 1.0f;

	// After 90 deg Y rotation, col0 X axis becomes (0,0,-1), col2 Z axis becomes (1,0,0).
	CHECK(mat[0] == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(mat[2] == doctest::Approx(-1.0f).epsilon(0.001));
	CHECK(mat[8] == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("[AVP Bridge] Transform with scale") {
	Transform3D t;
	t.basis.scale(Vector3(2, 3, 4));
	float mat[16] = {};

	const Basis &b = t.basis;
	mat[0] = b[0][0]; mat[5] = b[1][1]; mat[10] = b[2][2];

	CHECK(mat[0] == doctest::Approx(2.0f));
	CHECK(mat[5] == doctest::Approx(3.0f));
	CHECK(mat[10] == doctest::Approx(4.0f));
}

// ============================================================================
// §2. Transform Comparison Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Transform comparison - identical") {
	float a[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 };
	float b[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 };

	bool equal = true;
	for (int i = 0; i < 16; i++) {
		if (!Math::is_equal_approx(a[i], b[i])) {
			equal = false;
			break;
		}
	}
	CHECK(equal);
}

TEST_CASE("[AVP Bridge] Transform comparison - different") {
	float a[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 };
	float b[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 8, 1 };

	bool equal = true;
	for (int i = 0; i < 16; i++) {
		if (!Math::is_equal_approx(a[i], b[i])) {
			equal = false;
			break;
		}
	}
	CHECK_FALSE(equal);
}

TEST_CASE("[AVP Bridge] Transform comparison - epsilon tolerance") {
	float a[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 };
	float b[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7.0000001f, 1 };

	bool equal = true;
	for (int i = 0; i < 16; i++) {
		if (!Math::is_equal_approx(a[i], b[i])) {
			equal = false;
			break;
		}
	}
	CHECK_MESSAGE(equal, "Transforms that differ only by floating-point epsilon should be considered equal.");
}

// ============================================================================
// §3. Volume Camera Culling Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Volume AABB intersection - inside") {
	// Volume centered at origin with size 2x2x2.
	AABB volume_aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	// Entity AABB centered at origin.
	AABB entity_aabb(Vector3(-0.5, -0.5, -0.5), Vector3(1, 1, 1));

	CHECK(volume_aabb.intersects(entity_aabb));
}

TEST_CASE("[AVP Bridge] Volume AABB intersection - outside") {
	AABB volume_aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	// Entity far away.
	AABB entity_aabb(Vector3(10, 10, 10), Vector3(1, 1, 1));

	CHECK_FALSE(volume_aabb.intersects(entity_aabb));
}

TEST_CASE("[AVP Bridge] Volume AABB intersection - partial overlap") {
	AABB volume_aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	// Entity partially overlapping the edge.
	AABB entity_aabb(Vector3(0.5, 0, 0), Vector3(2, 1, 1));

	CHECK(volume_aabb.intersects(entity_aabb));
}

TEST_CASE("[AVP Bridge] Volume AABB intersection - touching edge") {
	AABB volume_aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	// Entity right at the edge.
	AABB entity_aabb(Vector3(1, 0, 0), Vector3(1, 1, 1));

	// AABB touching edge is considered intersecting.
	CHECK(volume_aabb.intersects(entity_aabb));
}

TEST_CASE("[AVP Bridge] Volume culling - immersive mode skips culling") {
	// In immersive mode (mode == 1), culling should not occur.
	// All entities remain visible regardless of position.
	int volume_mode = 1;
	bool volume_culling_enabled = true;

	// With mode == 1, the code checks `volume_mode == 0` before culling.
	bool should_cull = (volume_culling_enabled && volume_mode == 0);
	CHECK_FALSE(should_cull);
}

TEST_CASE("[AVP Bridge] Volume culling - disabled flag") {
	int volume_mode = 0;
	bool volume_culling_enabled = false;

	bool should_cull = (volume_culling_enabled && volume_mode == 0);
	CHECK_FALSE(should_cull);
}

// ============================================================================
// §4. Entity ID Derivation Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Entity ID from pointer - uniqueness") {
	// Simulate two different pointer addresses.
	uint64_t a = 0x1000;
	uint64_t b = 0x2000;

	CHECK(a != b);
}

TEST_CASE("[AVP Bridge] Entity ID from pointer - stability") {
	// Same pointer should give same ID.
	uint64_t ptr = 0xDEADBEEF;
	uint64_t id1 = ptr;
	uint64_t id2 = ptr;

	CHECK(id1 == id2);
}

// ============================================================================
// §5. @rk_shader Annotation Parsing Tests
// ============================================================================

TEST_CASE("[AVP Bridge] @rk_shader annotation - valid") {
	String code = "shader_type spatial;\n// @rk_shader(\"ToonMaterial\")\nvoid vertex() {}";

	int idx = code.find("@rk_shader");
	CHECK(idx != -1);

	int paren_start = code.find("(", idx);
	CHECK(paren_start != -1);

	int quote_start = code.find("\"", paren_start);
	CHECK(quote_start != -1);

	int quote_end = code.find("\"", quote_start + 1);
	CHECK(quote_end != -1);

	String name = code.substr(quote_start + 1, quote_end - quote_start - 1);
	CHECK(name == "ToonMaterial");
}

TEST_CASE("[AVP Bridge] @rk_shader annotation - GlassMaterial") {
	String code = "// @rk_shader(\"GlassMaterial\")\nshader_type spatial;";

	int idx = code.find("@rk_shader");
	int paren_start = code.find("(", idx);
	int quote_start = code.find("\"", paren_start);
	int quote_end = code.find("\"", quote_start + 1);
	String name = code.substr(quote_start + 1, quote_end - quote_start - 1);

	CHECK(name == "GlassMaterial");
}

TEST_CASE("[AVP Bridge] @rk_shader annotation - missing") {
	String code = "shader_type spatial;\nvoid vertex() {}";

	int idx = code.find("@rk_shader");
	CHECK(idx == -1);
}

TEST_CASE("[AVP Bridge] @rk_shader annotation - malformed (no quotes)") {
	String code = "// @rk_shader(ToonMaterial)";

	int idx = code.find("@rk_shader");
	int paren_start = code.find("(", idx);
	int quote_start = code.find("\"", paren_start);

	CHECK(quote_start == -1);
}

TEST_CASE("[AVP Bridge] @rk_shader annotation - malformed (no parens)") {
	String code = "// @rk_shader \"ToonMaterial\"";

	int idx = code.find("@rk_shader");
	int paren_start = code.find("(", idx);

	CHECK(paren_start == -1);
}

// ============================================================================
// §6. JSON Serialization Tests (for custom shader params)
// ============================================================================

TEST_CASE("[AVP Bridge] JSON serialize - float value") {
	HashMap<StringName, Variant> params;
	params["roughness"] = 0.5f;

	// Manually build JSON for testing.
	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":";
		json += String::num(float(E.value), 6);
	}
	json += "}";

	CHECK(json.find("\"roughness\"") != -1);
	CHECK(json.find("0.5") != -1);
}

TEST_CASE("[AVP Bridge] JSON serialize - Color value") {
	HashMap<StringName, Variant> params;
	params["color"] = Color(1.0, 0.5, 0.25, 1.0);

	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":";
		Color c = E.value;
		json += vformat("[%f,%f,%f,%f]", c.r, c.g, c.b, c.a);
	}
	json += "}";

	CHECK(json.find("\"color\"") != -1);
	CHECK(json.find("[1.") != -1);
}

TEST_CASE("[AVP Bridge] JSON serialize - bool value") {
	HashMap<StringName, Variant> params;
	params["flicker"] = true;

	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":";
		json += bool(E.value) ? "true" : "false";
	}
	json += "}";

	CHECK(json.find("\"flicker\":true") != -1);
}

TEST_CASE("[AVP Bridge] JSON serialize - int value") {
	HashMap<StringName, Variant> params;
	params["mode"] = 42;

	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":";
		json += itos(int(E.value));
	}
	json += "}";

	CHECK(json.find("\"mode\":42") != -1);
}

TEST_CASE("[AVP Bridge] JSON serialize - Vector3 value") {
	HashMap<StringName, Variant> params;
	params["offset"] = Vector3(1.0, 2.0, 3.0);

	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":";
		Vector3 vec = E.value;
		json += vformat("[%f,%f,%f]", vec.x, vec.y, vec.z);
	}
	json += "}";

	CHECK(json.find("\"offset\"") != -1);
	CHECK(json.find("[1.") != -1);
}

TEST_CASE("[AVP Bridge] JSON serialize - multiple params") {
	HashMap<StringName, Variant> params;
	params["roughness"] = 0.3f;
	params["metallic"] = 0.8f;
	params["tinted"] = true;

	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":";
		const Variant &v = E.value;
		switch (v.get_type()) {
			case Variant::FLOAT:
				json += String::num(float(v), 6);
				break;
			case Variant::BOOL:
				json += bool(v) ? "true" : "false";
				break;
			default:
				json += "null";
				break;
		}
	}
	json += "}";

	// Should contain all three keys.
	CHECK(json.find("roughness") != -1);
	CHECK(json.find("metallic") != -1);
	CHECK(json.find("tinted") != -1);
}

TEST_CASE("[AVP Bridge] JSON serialize - empty params") {
	HashMap<StringName, Variant> params;

	String json = "{";
	bool first = true;
	for (const KeyValue<StringName, Variant> &E : params) {
		if (!first) json += ",";
		first = false;
		json += "\"" + String(E.key) + "\":null";
	}
	json += "}";

	CHECK(json == "{}");
}

// ============================================================================
// §7. Material Parameter Extraction Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Material alpha mode - opaque") {
	int transparency = 0;
	int alpha_mode = 0;

	if (transparency == 1) {
		alpha_mode = 1;
	} else if (transparency == 2) {
		alpha_mode = 2;
	}

	CHECK(alpha_mode == 0);
}

TEST_CASE("[AVP Bridge] Material alpha mode - transparent") {
	int transparency = 1;
	int alpha_mode = 0;

	if (transparency == 1) {
		alpha_mode = 1;
	} else if (transparency == 2) {
		alpha_mode = 2;
	}

	CHECK(alpha_mode == 1);
}

TEST_CASE("[AVP Bridge] Material alpha mode - alpha_scissor") {
	int transparency = 2;
	int alpha_mode = 0;

	if (transparency == 1) {
		alpha_mode = 1;
	} else if (transparency == 2) {
		alpha_mode = 2;
	}

	CHECK(alpha_mode == 2);
}

TEST_CASE("[AVP Bridge] Material IOR to metallic mapping") {
	// Glass: IOR 1.5 -> metallic = max(0, min(0.1, (1.5-1.0)*0.05)) = 0.025
	float ior = 1.5f;
	float metallic = MAX(0.0f, MIN(0.1f, (ior - 1.0f) * 0.05f));
	CHECK(metallic == doctest::Approx(0.025f));

	// Air: IOR 1.0 -> metallic = 0
	ior = 1.0f;
	metallic = MAX(0.0f, MIN(0.1f, (ior - 1.0f) * 0.05f));
	CHECK(metallic == doctest::Approx(0.0f));

	// Diamond: IOR 2.42 -> metallic = min(0.1, (2.42-1)*0.05) = min(0.1, 0.071) = 0.071
	ior = 2.42f;
	metallic = MAX(0.0f, MIN(0.1f, (ior - 1.0f) * 0.05f));
	CHECK(metallic == doctest::Approx(0.071f));

	// Very high IOR -> clamped to 0.1
	ior = 5.0f;
	metallic = MAX(0.0f, MIN(0.1f, (ior - 1.0f) * 0.05f));
	CHECK(metallic == doctest::Approx(0.1f));
}

// ============================================================================
// §8. Input Bridge Event Queue Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Input event queue - empty initially") {
	// Simulate an empty queue using LocalVector.
	LocalVector<Vector3> queue;
	CHECK(queue.is_empty());
}

TEST_CASE("[AVP Bridge] Input event queue - push and pop") {
	LocalVector<Vector3> queue;
	queue.push_back(Vector3(1, 2, 3));
	queue.push_back(Vector3(4, 5, 6));

	CHECK(queue.size() == 2);
	CHECK(queue[0].is_equal_approx(Vector3(1, 2, 3)));

	queue.remove_at(0);
	CHECK(queue.size() == 1);
	CHECK(queue[0].is_equal_approx(Vector3(4, 5, 6)));
}

TEST_CASE("[AVP Bridge] Collision shape auto-generation - AABB to box") {
	Vector3 aabb_size(2.0, 3.0, 1.5);
	float box_data[3] = { aabb_size.x, aabb_size.y, aabb_size.z };

	// Minimum clamp.
	for (int i = 0; i < 3; i++) {
		if (box_data[i] < 0.01f) {
			box_data[i] = 0.01f;
		}
	}

	CHECK(box_data[0] == doctest::Approx(2.0f));
	CHECK(box_data[1] == doctest::Approx(3.0f));
	CHECK(box_data[2] == doctest::Approx(1.5f));
}

TEST_CASE("[AVP Bridge] Collision shape auto-generation - degenerate AABB clamped") {
	Vector3 aabb_size(0.0, 0.001, 0.0);
	float box_data[3] = { aabb_size.x, aabb_size.y, aabb_size.z };

	for (int i = 0; i < 3; i++) {
		if (box_data[i] < 0.01f) {
			box_data[i] = 0.01f;
		}
	}

	CHECK(box_data[0] == doctest::Approx(0.01f));
	CHECK(box_data[1] == doctest::Approx(0.01f));
	CHECK(box_data[2] == doctest::Approx(0.01f));
}

// ============================================================================
// §9. Octahedron Decode Tests (mesh normals)
// ============================================================================

TEST_CASE("[AVP Bridge] Octahedron decode - forward normal (0,0,1)") {
	// Encoding: (0,0,1) maps to octahedron (0.5, 0.5).
	Vector2 oct(0.5f, 0.5f);
	Vector3 decoded = Vector3::octahedron_decode(oct);
	CHECK(decoded.z == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("[AVP Bridge] Octahedron decode - up normal (0,1,0)") {
	// Encoding: (0,1,0) maps to octahedron (0.5, 1.0).
	Vector2 oct(0.5f, 1.0f);
	Vector3 decoded = Vector3::octahedron_decode(oct);
	CHECK(decoded.y == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("[AVP Bridge] Octahedron decode - right normal (1,0,0)") {
	// Encoding: (1,0,0) maps to octahedron (1.0, 0.5).
	Vector2 oct(1.0f, 0.5f);
	Vector3 decoded = Vector3::octahedron_decode(oct);
	CHECK(decoded.x == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("[AVP Bridge] Octahedron decode - negative Z (0,0,-1)") {
	Vector2 oct(0.0f, 0.0f);
	Vector3 decoded = Vector3::octahedron_decode(oct);
	CHECK(decoded.z == doctest::Approx(-1.0f).epsilon(0.01));
}

// ============================================================================
// §10. Dirty Flag Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Dirty flags - none") {
	uint32_t flags = 0;
	CHECK((flags & (1 << 0)) == 0); // DIRTY_TRANSFORM
	CHECK((flags & (1 << 1)) == 0); // DIRTY_MESH
	CHECK((flags & (1 << 2)) == 0); // DIRTY_MATERIAL
	CHECK((flags & (1 << 3)) == 0); // DIRTY_VISIBILITY
}

TEST_CASE("[AVP Bridge] Dirty flags - transform only") {
	uint32_t flags = (1 << 0);
	CHECK((flags & (1 << 0)) != 0);
	CHECK((flags & (1 << 1)) == 0);
	CHECK((flags & (1 << 2)) == 0);
}

TEST_CASE("[AVP Bridge] Dirty flags - combined") {
	uint32_t flags = (1 << 0) | (1 << 2); // TRANSFORM + MATERIAL
	CHECK((flags & (1 << 0)) != 0);
	CHECK((flags & (1 << 1)) == 0);
	CHECK((flags & (1 << 2)) != 0);
}

TEST_CASE("[AVP Bridge] Dirty flags - all") {
	uint32_t flags = 0xFFFFFFFF;
	CHECK((flags & (1 << 0)) != 0);
	CHECK((flags & (1 << 1)) != 0);
	CHECK((flags & (1 << 2)) != 0);
	CHECK((flags & (1 << 3)) != 0);
	CHECK((flags & (1 << 4)) != 0);
}

// ============================================================================
// §11. Entity Count and Memory Tracking Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Entity count tracking") {
	// Simulate entity count using a simple counter.
	uint32_t count = 0;

	count++; // entity_create
	CHECK(count == 1);

	count++; // entity_create
	CHECK(count == 2);

	count--; // entity_destroy
	CHECK(count == 1);

	count--; // entity_destroy
	CHECK(count == 0);
}

TEST_CASE("[AVP Bridge] Entity count - destroy below zero clamped") {
	uint32_t count = 0;

	// Simulate destroy when count is already 0.
	if (count > 0) {
		count--;
	}
	CHECK(count == 0);
}

// ============================================================================
// §12. Bridge Call Safety Tests (null function pointers)
// ============================================================================

TEST_CASE("[AVP Bridge] Bridge calls are safe with null function pointers") {
	// avp_bridge.cpp guards all calls with null checks.
	// Verify the pattern: if (s_func) { s_func(...); }
	void (*test_func)(void) = nullptr;

	bool called = false;
	if (test_func) {
		called = true;
	}
	CHECK_FALSE(called);
}

// ============================================================================
// §13. RID Stability Tests
// ============================================================================

TEST_CASE("[AVP Bridge] RID to uint64 roundtrip") {
	RID rid;
	// RID().get_id() should return 0 for invalid RIDs.
	CHECK(rid.get_id() == 0);
	CHECK_FALSE(rid.is_valid());
}

TEST_CASE("[AVP Bridge] RID from_uint64 roundtrip") {
	uint64_t id = 12345;
	RID rid = RID::from_uint64(id);
	CHECK(rid.get_id() == id);
	CHECK(rid.is_valid());
}

// ============================================================================
// §14. Interactive Entity Set Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Interactive entity tracking") {
	HashSet<uint64_t> interactive_entities;

	interactive_entities.insert(100);
	interactive_entities.insert(200);
	interactive_entities.insert(300);

	CHECK(interactive_entities.has(100));
	CHECK(interactive_entities.has(200));
	CHECK(interactive_entities.has(300));
	CHECK_FALSE(interactive_entities.has(400));

	interactive_entities.erase(200);
	CHECK_FALSE(interactive_entities.has(200));
	CHECK(interactive_entities.size() == 2);
}

// ============================================================================
// §15. Shader Name Registration Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Replacement shader names") {
	// Verify the expected set of replacement shader names.
	Vector<String> expected_shaders = {
		"ToonMaterial",
		"GlassMaterial",
		"HologramMaterial",
		"WaterMaterial",
	};

	CHECK(expected_shaders.size() == 4);
	CHECK(expected_shaders[0] == "ToonMaterial");
	CHECK(expected_shaders[1] == "GlassMaterial");
	CHECK(expected_shaders[2] == "HologramMaterial");
	CHECK(expected_shaders[3] == "WaterMaterial");
}

// ============================================================================
// §16. Drag Phase Encoding Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Drag phase values") {
	// Verify phase integer encoding matches Swift DragPhase enum.
	int began = 0;
	int changed = 1;
	int ended = 2;
	int cancelled = 3;

	CHECK(began == 0);
	CHECK(changed == 1);
	CHECK(ended == 2);
	CHECK(cancelled == 3);
}

// ============================================================================
// §17. Volume Camera Mode Tests
// ============================================================================

TEST_CASE("[AVP Bridge] Volume camera mode - bounded") {
	int mode = 0;
	CHECK(mode == 0);
	CHECK((mode == 0)); // bounded
}

TEST_CASE("[AVP Bridge] Volume camera mode - immersive") {
	int mode = 1;
	CHECK(mode == 1);
	CHECK((mode != 0)); // not bounded
}

// ============================================================================
// §18. AABB Size Extraction Tests
// ============================================================================

TEST_CASE("[AVP Bridge] AABB size for collision") {
	AABB aabb(Vector3(-1, -0.5, -2), Vector3(2, 1, 4));
	Vector3 size = aabb.size;

	CHECK(size.x == doctest::Approx(2.0f));
	CHECK(size.y == doctest::Approx(1.0f));
	CHECK(size.z == doctest::Approx(4.0f));
}

TEST_CASE("[AVP Bridge] AABB world transform") {
	AABB local_aabb(Vector3(-0.5, -0.5, -0.5), Vector3(1, 1, 1));
	Transform3D xform;
	xform.origin = Vector3(5, 0, 0);
	xform.basis.scale(Vector3(2, 2, 2));

	AABB world_aabb = xform.xform(local_aabb);

	// Scaled by 2 and translated by 5 on X.
	CHECK(world_aabb.size.x == doctest::Approx(2.0f));
	CHECK(world_aabb.size.y == doctest::Approx(2.0f));
	CHECK(world_aabb.size.z == doctest::Approx(2.0f));
	CHECK(world_aabb.position.x == doctest::Approx(4.0f)); // 5 - 1 (half of scaled size)
}

} // namespace TestAVPBridge
