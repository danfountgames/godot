/**************************************************************************/
/*  GodotImmersiveView.swift                                              */
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

/// The RealityView for immersive space presentation.
///
/// This is a separate view from `GodotSpatialView` because visionOS requires
/// immersive spaces to be declared as a separate `ImmersiveSpace` scene.
///
/// In immersive mode:
/// - Content is not clipped to a volume box
/// - The scene root is placed relative to the user's head/origin
/// - The same entity synchronizer and scene root are shared with the bounded view
/// - Only the content transform differs (no volume camera offset)
///
/// The engine tick is driven from the bounded view — this view only presents
/// the shared scene root in the immersive context.
struct GodotImmersiveView: View {

    /// The shared Godot engine instance.
    private let engine = GodotEngine.shared

    /// A separate root entity for the immersive space. The Godot scene root
    /// is reparented here when immersive mode is active.
    @State private var immersiveRoot = Entity()

    var body: some View {
        RealityView { content in
            // -- Make closure --
            immersiveRoot.name = "GodotImmersiveRoot"
            content.add(immersiveRoot)

            // In immersive mode, the scene root is added as a child.
            // The volume camera content transform handles coordinate mapping.
            let sceneRoot = engine.sceneRoot
            immersiveRoot.addChild(sceneRoot)

            // Apply the immersive content transform (scale only, no volume offset).
            let scaleFactor = engine.volumeCamera.scaleFactor
            immersiveRoot.scale = SIMD3<Float>(repeating: scaleFactor)

        } update: { content in
            // -- Update closure --
            // The engine tick is driven from GodotSpatialView.
            // Here we only update the transform if the scale factor changes.
            let scaleFactor = engine.volumeCamera.scaleFactor
            immersiveRoot.scale = SIMD3<Float>(repeating: scaleFactor)
        }
        .gesture(immersiveTapGesture)
        .gesture(immersiveDragGesture)
    }

    // MARK: - Gestures

    /// Spatial tap gesture for immersive mode.
    private var immersiveTapGesture: some Gesture {
        SpatialTapGesture()
            .targetedToAnyEntity()
            .onEnded { value in
                let entity = value.entity
                let location = value.location3D

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

    /// Drag gesture for immersive mode.
    ///
    /// Same as GodotSpatialView drag gesture — tracks start position and
    /// properly detects began/changed/ended phases.
    private var immersiveDragGesture: some Gesture {
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
