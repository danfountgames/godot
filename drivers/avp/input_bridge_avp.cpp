/**************************************************************************/
/*  input_bridge_avp.cpp                                                  */
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

#include "input_bridge_avp.h"

#include "avp_bridge.h"
#include "core/string/print_string.h"

InputBridgeAVP *InputBridgeAVP::singleton = nullptr;

InputBridgeAVP::InputBridgeAVP() {
	singleton = this;
}

InputBridgeAVP::~InputBridgeAVP() {
	singleton = nullptr;
}

void InputBridgeAVP::initialize() {
	// Register C callbacks with the bridge so Swift can route
	// tap/drag events back to the C++ side.
	godot_avp_set_tap_callback(_tap_callback);
	godot_avp_set_drag_callback(_drag_callback);
	print_verbose("AVP InputBridge: Tap/drag callbacks registered.");
}

// --- Collision Shape Management ---

void InputBridgeAVP::set_entity_collision(uint64_t p_entity_id, ShapeType p_shape_type,
		const float *p_data, uint32_t p_data_count) {
	godot_avp_entity_set_collision(p_entity_id, static_cast<int>(p_shape_type),
			p_data, p_data_count);
}

void InputBridgeAVP::set_entity_interactive(uint64_t p_entity_id, bool p_interactive) {
	if (p_interactive) {
		interactive_entities.insert(p_entity_id);
	} else {
		interactive_entities.erase(p_entity_id);
	}
	godot_avp_entity_set_interactive(p_entity_id, p_interactive ? 1 : 0);
}

void InputBridgeAVP::auto_collision_from_aabb(uint64_t p_entity_id, const Vector3 &p_aabb_size) {
	// Generate a box collision shape from the AABB extents.
	float box_data[3] = {
		p_aabb_size.x,
		p_aabb_size.y,
		p_aabb_size.z,
	};

	// Clamp to a minimum size so very small/degenerate meshes still have
	// a pickable collision region.
	for (int i = 0; i < 3; i++) {
		if (box_data[i] < 0.01f) {
			box_data[i] = 0.01f;
		}
	}

	set_entity_collision(p_entity_id, SHAPE_BOX, box_data, 3);
}

// --- Event Queue ---

bool InputBridgeAVP::process_queued_events() {
	// This method is called from the main thread during Godot's iteration.
	// Events were enqueued by the static callbacks which may arrive
	// from arbitrary threads (via Swift async dispatch).
	return has_tap_events() || has_drag_events();
}

bool InputBridgeAVP::dequeue_tap(TapEvent &r_event) {
	if (tap_queue.is_empty()) {
		return false;
	}
	r_event = tap_queue[0];
	tap_queue.remove_at(0);
	return true;
}

bool InputBridgeAVP::dequeue_drag(DragEvent &r_event) {
	if (drag_queue.is_empty()) {
		return false;
	}
	r_event = drag_queue[0];
	drag_queue.remove_at(0);
	return true;
}

bool InputBridgeAVP::is_entity_interactive(uint64_t p_entity_id) const {
	return interactive_entities.has(p_entity_id);
}

void InputBridgeAVP::remove_entity(uint64_t p_entity_id) {
	interactive_entities.erase(p_entity_id);
}

// --- Static C Callbacks ---

void InputBridgeAVP::_tap_callback(uint64_t entity_id,
		float hit_pos_x, float hit_pos_y, float hit_pos_z,
		float hit_normal_x, float hit_normal_y, float hit_normal_z) {
	InputBridgeAVP *bridge = get_singleton();
	if (!bridge) {
		return;
	}

	TapEvent event;
	event.entity_id = entity_id;
	event.hit_position = Vector3(hit_pos_x, hit_pos_y, hit_pos_z);
	event.hit_normal = Vector3(hit_normal_x, hit_normal_y, hit_normal_z);

	// Queue for delivery on the main thread.
	bridge->tap_queue.push_back(event);
}

void InputBridgeAVP::_drag_callback(uint64_t entity_id, int phase,
		float hit_pos_x, float hit_pos_y, float hit_pos_z,
		float translation_x, float translation_y, float translation_z) {
	InputBridgeAVP *bridge = get_singleton();
	if (!bridge) {
		return;
	}

	DragEvent event;
	event.entity_id = entity_id;
	event.phase = phase;
	event.start_position = Vector3(hit_pos_x, hit_pos_y, hit_pos_z);
	event.translation = Vector3(translation_x, translation_y, translation_z);

	// Queue for delivery on the main thread.
	bridge->drag_queue.push_back(event);
}
