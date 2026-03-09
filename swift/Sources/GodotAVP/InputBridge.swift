/**************************************************************************/
/*  InputBridge.swift                                                     */
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

import RealityKit
import simd
import CGodotAVPBridge

// MARK: - Drag Phase

/// Maps drag gesture phases to the C bridge integer values.
enum DragPhase: Int32 {
    case began = 0
    case changed = 1
    case ended = 2
    case cancelled = 3
}

// MARK: - InputBridge

/// Processes RealityKit spatial input events and routes them to the
/// Godot engine via registered C callbacks.
///
/// RealityKit is the sole authority for spatial input on visionOS.
/// Eye-gaze ray is system-private — Godot cannot perform spatial picking.
/// All input must flow from RealityKit -> C bridge -> Godot.
///
/// This class:
/// - Processes `SpatialTapGesture` events from RealityKit
/// - Processes `DragGesture` events from RealityKit
/// - Maps RealityKit entity references back to Godot entity IDs
/// - Calls the registered C callbacks
///
/// Must operate on `@MainActor` as it accesses the EntitySynchronizer
/// for entity ID resolution.
@MainActor
public final class InputBridge {

    // MARK: - Properties

    /// Reference to the entity synchronizer for entity ID lookups.
    private let entitySynchronizer: EntitySynchronizer

    /// Registered tap callback (Swift -> C++).
    private var tapCallback: godot_avp_tap_callback?

    /// Registered drag callback (Swift -> C++).
    private var dragCallback: godot_avp_drag_callback?

    // MARK: - Initialization

    public init(entitySynchronizer: EntitySynchronizer) {
        self.entitySynchronizer = entitySynchronizer
    }

    // MARK: - Callback Registration

    /// Register the tap gesture callback. Called during bridge initialization.
    public func setTapCallback(_ callback: godot_avp_tap_callback?) {
        tapCallback = callback
    }

    /// Register the drag gesture callback. Called during bridge initialization.
    public func setDragCallback(_ callback: godot_avp_drag_callback?) {
        dragCallback = callback
    }

    // MARK: - Tap Gesture Processing

    /// Process a spatial tap gesture event from RealityKit.
    ///
    /// Called from the `SpatialTapGesture` handler in `GodotSpatialView`.
    ///
    /// - Parameters:
    ///   - entity: The RealityKit entity that was tapped.
    ///   - location3D: The hit position in entity-local coordinates.
    public func handleTap(entity: Entity, location3D: SIMD3<Float>) {
        guard let callback = tapCallback else { return }

        // Resolve the RealityKit entity to a Godot RID.
        guard let godotId = resolveGodotId(for: entity) else { return }

        // Surface normal is unproven in public API (see avp_bridge.h comment).
        // Pass zeros for now.
        callback(
            godotId,
            location3D.x, location3D.y, location3D.z,
            0, 0, 0  // hit_normal — unproven, zeroed
        )
    }

    // MARK: - Drag Gesture Processing

    /// Process a drag gesture event from RealityKit.
    ///
    /// Called from the `DragGesture` handler in `GodotSpatialView`.
    ///
    /// - Parameters:
    ///   - entity: The RealityKit entity being dragged.
    ///   - phase: The drag gesture phase.
    ///   - startLocation3D: The initial hit position in entity-local coordinates.
    ///   - translation: Cumulative drag translation in world-space.
    public func handleDrag(
        entity: Entity,
        phase: DragPhase,
        startLocation3D: SIMD3<Float>,
        translation: SIMD3<Float>
    ) {
        guard let callback = dragCallback else { return }

        // Resolve the RealityKit entity to a Godot RID.
        guard let godotId = resolveGodotId(for: entity) else { return }

        callback(
            godotId,
            phase.rawValue,
            startLocation3D.x, startLocation3D.y, startLocation3D.z,
            translation.x, translation.y, translation.z
        )
    }

    // MARK: - Collision Shape Management

    /// Configure collision and input components on an entity for spatial interaction.
    ///
    /// Adds `InputTargetComponent` and `CollisionComponent` so the entity can
    /// receive spatial tap and drag gestures. Also adds `HoverEffectComponent`
    /// for system-standard visual hover feedback.
    ///
    /// - Parameters:
    ///   - entity: The RealityKit entity to make interactive.
    ///   - shape: The collision shape to use for hit testing.
    public func makeInteractive(_ entity: ModelEntity, shape: ShapeResource) {
        entity.components.set(InputTargetComponent())
        entity.components.set(CollisionComponent(shapes: [shape]))
        entity.components.set(HoverEffectComponent())
    }

    /// Remove interactivity from an entity.
    public func removeInteractive(_ entity: ModelEntity) {
        entity.components.remove(InputTargetComponent.self)
        entity.components.remove(CollisionComponent.self)
        entity.components.remove(HoverEffectComponent.self)
    }

    // MARK: - Private

    /// Walk up the entity hierarchy to find a Godot-managed entity.
    ///
    /// When the user taps a child entity (e.g., a collision shape child),
    /// we walk up to find the parent that has a Godot RID.
    private func resolveGodotId(for entity: Entity) -> UInt64? {
        var current: Entity? = entity
        while let e = current {
            if let id = entitySynchronizer.godotId(for: e) {
                return id
            }
            current = e.parent
        }
        return nil
    }
}
