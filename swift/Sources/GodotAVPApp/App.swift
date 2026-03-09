/**************************************************************************/
/*  App.swift                                                             */
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

/// The main application entry point for the Godot-RealityKit bridge.
///
/// Provides two presentation modes:
///   - **Bounded volume**: A windowed 3D volume (the default). Content is
///     clipped to the volume bounds set by `VolumeCameraNode`.
///   - **Immersive space**: Full immersive environment, opened when
///     `VolumeCameraNode.mode` transitions to `.immersive`.
@main
struct GodotAVPApp: App {

    /// The shared Godot engine instance.
    @State private var engine = GodotEngine.shared

    /// Tracks whether the immersive space is currently open.
    @State private var isImmersiveSpaceOpen = false

    /// Environment action to open the immersive space.
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace

    /// Environment action to dismiss the immersive space.
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some Scene {
        // -- Bounded Volume Window --
        WindowGroup(id: "GodotVolume") {
            GodotSpatialView()
                .task {
                    // Initialize the engine on first appearance.
                    engine.initialize()
                }
                .onChange(of: engine.volumeCamera.isImmersive) { _, isImmersive in
                    Task {
                        if isImmersive && !isImmersiveSpaceOpen {
                            let result = await openImmersiveSpace(id: "GodotImmersive")
                            isImmersiveSpaceOpen = (result == .opened)
                        } else if !isImmersive && isImmersiveSpaceOpen {
                            await dismissImmersiveSpace()
                            isImmersiveSpaceOpen = false
                        }
                    }
                }
        }
        .windowStyle(.volumetric)
        .defaultSize(
            width: engine.volumeCamera.scaledBounds.x,
            height: engine.volumeCamera.scaledBounds.y,
            depth: engine.volumeCamera.scaledBounds.z,
            in: .meters
        )

        // -- Immersive Space --
        ImmersiveSpace(id: "GodotImmersive") {
            GodotImmersiveView()
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
