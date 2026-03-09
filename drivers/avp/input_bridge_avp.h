/**************************************************************************/
/*  input_bridge_avp.h                                                    */
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

#ifndef INPUT_BRIDGE_AVP_H
#define INPUT_BRIDGE_AVP_H

#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

/**
 * Godot -> Apple Vision Pro input bridge.
 *
 * Handles two-way input routing:
 *   1. C++ -> Swift: Extracts collision shapes from Godot scene data and
 *      forwards them to the RealityKit bridge so entities can receive
 *      spatial input events (tap, drag, hover).
 *   2. Swift -> C++: Receives tap/drag callbacks from RealityKit and
 *      queues them for delivery as Godot input events during the next
 *      simulation frame.
 *
 * Architecture (per RK_PLAN.md §2.30):
 *   RealityKit is the sole authority for spatial input on visionOS.
 *   Eye-gaze ray is system-private — Godot cannot perform spatial picking.
 *   Input flows: user gaze/gesture → RealityKit → C bridge → Godot signals.
 *
 * Collision shape strategy (Phase 7.5):
 *   Priority order for generating CollisionComponent shapes:
 *   1. Godot CollisionShape3D child (if present) → exact shape
 *   2. Mesh bounding box → .generateBox() fallback
 *   3. Default small box → last-resort for meshless entities
 */
class InputBridgeAVP {
public:
	// --- Queued Event Types ---

	struct TapEvent {
		uint64_t entity_id = 0;
		Vector3 hit_position;
		Vector3 hit_normal;
	};

	struct DragEvent {
		uint64_t entity_id = 0;
		int phase = 0; // 0=began, 1=changed, 2=ended, 3=cancelled
		Vector3 start_position;
		Vector3 translation;
	};

	// --- Collision Shape Types ---

	enum ShapeType {
		SHAPE_BOX = 0,
		SHAPE_SPHERE = 1,
		SHAPE_CAPSULE = 2,
		SHAPE_CONVEX_HULL = 3,
		SHAPE_STATIC_MESH = 4,
	};

	static InputBridgeAVP *get_singleton() { return singleton; }

	InputBridgeAVP();
	~InputBridgeAVP();

	/**
	 * Initialize the input bridge. Registers C callbacks for
	 * receiving tap/drag events from the Swift side.
	 */
	void initialize();

	/**
	 * Mark an entity as interactive with the given collision shape.
	 * Forwards the collision shape to the Swift bridge and enables
	 * InputTargetComponent + HoverEffectComponent.
	 *
	 * @param p_entity_id   Entity (geometry instance) ID.
	 * @param p_shape_type  Shape type (ShapeType enum).
	 * @param p_data        Shape data (size for box, radius for sphere, etc.).
	 * @param p_data_count  Number of floats in data array.
	 */
	void set_entity_collision(uint64_t p_entity_id, ShapeType p_shape_type,
			const float *p_data, uint32_t p_data_count);

	/**
	 * Set entity interactive flag. Controls whether the entity
	 * has InputTargetComponent and receives gesture events.
	 */
	void set_entity_interactive(uint64_t p_entity_id, bool p_interactive);

	/**
	 * Auto-configure collision for an entity using its AABB.
	 * Called when an entity is created and marked as interactive
	 * but has no explicit collision shape.
	 */
	void auto_collision_from_aabb(uint64_t p_entity_id, const Vector3 &p_aabb_size);

	// --- Event Queue ---

	/**
	 * Process queued input events. Called during Godot's main iteration
	 * to deliver spatial input events that arrived from RealityKit
	 * since the last frame.
	 *
	 * Returns true if any events were processed.
	 */
	bool process_queued_events();

	/**
	 * Get the next tap event from the queue. Returns false if empty.
	 */
	bool dequeue_tap(TapEvent &r_event);

	/**
	 * Get the next drag event from the queue. Returns false if empty.
	 */
	bool dequeue_drag(DragEvent &r_event);

	/**
	 * Check if there are any queued tap events.
	 */
	bool has_tap_events() const { return !tap_queue.is_empty(); }

	/**
	 * Check if there are any queued drag events.
	 */
	bool has_drag_events() const { return !drag_queue.is_empty(); }

	// --- Interactivity Tracking ---

	/**
	 * Check if an entity is currently marked as interactive.
	 */
	bool is_entity_interactive(uint64_t p_entity_id) const;

	/**
	 * Get the set of all interactive entity IDs.
	 */
	const HashSet<uint64_t> &get_interactive_entities() const { return interactive_entities; }

	/**
	 * Remove an entity from the interactive set (on entity destroy).
	 */
	void remove_entity(uint64_t p_entity_id);

private:
	static InputBridgeAVP *singleton;

	// Tap and drag event queues.
	// Events arrive from Swift callbacks on arbitrary threads;
	// they are queued and consumed on the Godot main thread.
	LocalVector<TapEvent> tap_queue;
	LocalVector<DragEvent> drag_queue;

	// Set of entity IDs that are currently interactive.
	HashSet<uint64_t> interactive_entities;

	// Static C callbacks registered with the bridge.
	static void _tap_callback(uint64_t entity_id,
			float hit_pos_x, float hit_pos_y, float hit_pos_z,
			float hit_normal_x, float hit_normal_y, float hit_normal_z);

	static void _drag_callback(uint64_t entity_id, int phase,
			float hit_pos_x, float hit_pos_y, float hit_pos_z,
			float translation_x, float translation_y, float translation_z);
};

#endif // INPUT_BRIDGE_AVP_H
