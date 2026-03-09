/**************************************************************************/
/*  GodotSpatialView.swift                                                */
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

import SwiftUI
import RealityKit
import GodotAVP

/// The primary RealityView for bounded volume presentation.
///
/// This view:
/// - Hosts the Godot scene root entity in a `RealityView`
/// - Drives the engine tick from the `update` closure
/// - Handles spatial gestures (tap, drag) and routes them to the InputBridge
/// - Applies the volume camera content transform to the scene root
struct GodotSpatialView: View {

    /// The shared Godot engine instance.
    private let engine = GodotEngine.shared

    var body: some View {
        RealityView { content in
            // -- Make closure --
            // Add the Godot scene root entity to the RealityView content.
            let sceneRoot = engine.sceneRoot
            content.add(sceneRoot)

            // Apply initial content transform from volume camera.
            sceneRoot.transform = Transform(matrix: engine.volumeCamera.contentTransform)

        } update: { content in
            // -- Update closure --
            // Called each frame by RealityKit. Drive the Godot engine tick.
            engine.tick()

            // Update the scene root transform if the volume camera changed.
            engine.sceneRoot.transform = Transform(
                matrix: engine.volumeCamera.contentTransform
            )
        }
        .gesture(tapGesture)
        .gesture(dragGesture)
    }

    // MARK: - Gestures

    /// Spatial tap gesture — detect taps on Godot entities.
    private var tapGesture: some Gesture {
        SpatialTapGesture()
            .targetedToAnyEntity()
            .onEnded { value in
                let entity = value.entity
                let location = value.location3D

                // Convert to SIMD3<Float> for the bridge.
                let hitPos = SIMD3<Float>(
                    Float(location.x),
                    Float(location.y),
                    Float(location.z)
                )

                engine.inputBridge.handleTap(
                    entity: entity,
                    location3D: hitPos
                )
            }
    }

    /// Drag gesture — detect drags on Godot entities.
    ///
    /// Properly tracks drag phases (began/changed/ended) and stores the
    /// entity's initial position at gesture start for accurate hit reporting.
    private var dragGesture: some Gesture {
        DragGesture()
            .targetedToAnyEntity()
            .onChanged { value in
                let entity = value.entity
                let translation = value.translation3D

                let trans = SIMD3<Float>(
                    Float(translation.x),
                    Float(translation.y),
                    Float(translation.z)
                )

                // Use the entity's position at drag start via startLocation3D.
                let startPos: SIMD3<Float>
                if let startLoc = value.startLocation3D {
                    startPos = SIMD3<Float>(
                        Float(startLoc.x),
                        Float(startLoc.y),
                        Float(startLoc.z)
                    )
                } else {
                    startPos = SIMD3<Float>(
                        entity.position.x,
                        entity.position.y,
                        entity.position.z
                    )
                }

                // Determine phase: first onChanged is effectively "began",
                // subsequent ones are "changed". We detect first by checking
                // if translation is near zero.
                let isNearStart = (abs(translation.x) + abs(translation.y) + abs(translation.z)) < 0.001
                let phase: DragPhase = isNearStart ? .began : .changed

                engine.inputBridge.handleDrag(
                    entity: entity,
                    phase: phase,
                    startLocation3D: startPos,
                    translation: trans
                )
            }
            .onEnded { value in
                let entity = value.entity
                let translation = value.translation3D

                let trans = SIMD3<Float>(
                    Float(translation.x),
                    Float(translation.y),
                    Float(translation.z)
                )

                let startPos: SIMD3<Float>
                if let startLoc = value.startLocation3D {
                    startPos = SIMD3<Float>(
                        Float(startLoc.x),
                        Float(startLoc.y),
                        Float(startLoc.z)
                    )
                } else {
                    startPos = SIMD3<Float>(
                        entity.position.x,
                        entity.position.y,
                        entity.position.z
                    )
                }

                engine.inputBridge.handleDrag(
                    entity: entity,
                    phase: .ended,
                    startLocation3D: startPos,
                    translation: trans
                )
            }
    }
}
