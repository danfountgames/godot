/**************************************************************************/
/*  VolumeCameraNode.swift                                                */
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

import Foundation
import simd

// MARK: - VolumeMode

/// The display mode for the volume.
///
/// - bounded: Content is rendered in a windowed volume (clipped to bounds).
/// - immersive: Content is rendered in an immersive space (no clipping).
public enum VolumeMode: Int, Sendable {
    case bounded = 0
    case immersive = 1
}

// MARK: - VolumeCameraNode

/// Manages the volume camera configuration for spatial presentation.
///
/// The volume camera defines the bounding volume within which Godot content
/// is visible. In bounded mode, content is clipped to a box. In immersive
/// mode, content fills the immersive space.
///
/// Receives volume camera updates from the C bridge and applies coordinate
/// transform mapping from Godot's coordinate system (Y-up, right-handed)
/// to RealityKit's coordinate system (also Y-up, right-handed, but meters).
///
/// Observable for SwiftUI reactivity — the app layer watches for mode
/// changes to switch between volumetric window and immersive space.
@Observable
@MainActor
public final class VolumeCameraNode {

    // MARK: - Properties

    /// Volume dimensions in meters (width, height, depth).
    public private(set) var bounds: SIMD3<Float> = SIMD3<Float>(1.0, 1.0, 1.0)

    /// World transform of the volume camera.
    public private(set) var transform: simd_float4x4 = matrix_identity_float4x4

    /// Current display mode.
    public private(set) var mode: VolumeMode = .bounded

    /// Scale factor applied to convert Godot units to RealityKit meters.
    /// Default: 1.0 (1 Godot unit = 1 meter). Adjust if your Godot project
    /// uses a different scale convention.
    public var scaleFactor: Float = 1.0

    // MARK: - Computed Properties

    /// The volume bounds scaled to RealityKit meters.
    public var scaledBounds: SIMD3<Float> {
        bounds * scaleFactor
    }

    /// Whether the volume is in immersive mode.
    public var isImmersive: Bool {
        mode == .immersive
    }

    /// The content scale transform that maps Godot scene coordinates to
    /// the volume. Applied to the scene root entity.
    ///
    /// For bounded mode, this centers content in the volume and applies
    /// the scale factor. For immersive mode, only the scale factor is applied.
    public var contentTransform: simd_float4x4 {
        var result = matrix_identity_float4x4

        // Apply scale factor.
        result.columns.0 *= scaleFactor
        result.columns.1 *= scaleFactor
        result.columns.2 *= scaleFactor

        if mode == .bounded {
            // In bounded mode, apply the volume camera transform to
            // offset content relative to the volume center.
            // The volume camera transform defines where the "camera"
            // is positioned in Godot world space — we invert it to
            // move the scene content accordingly.
            let invTransform = transform.inverse
            result = invTransform * result
        }

        return result
    }

    // MARK: - Bridge Callbacks

    /// Set the bounding dimensions of the spatial volume.
    /// Called from `godot_avp_volume_camera_set_bounds`.
    ///
    /// - Parameters:
    ///   - sizeX: Volume width in Godot units.
    ///   - sizeY: Volume height in Godot units.
    ///   - sizeZ: Volume depth in Godot units.
    public func setBounds(sizeX: Float, sizeY: Float, sizeZ: Float) {
        bounds = SIMD3<Float>(sizeX, sizeY, sizeZ)
    }

    /// Set the world transform of the volume camera.
    /// Called from `godot_avp_volume_camera_set_transform`.
    ///
    /// - Parameter transform16: Column-major 4x4 matrix (16 floats).
    public func setTransform(_ transform16: UnsafePointer<Float>) {
        transform = simd_float4x4(
            SIMD4<Float>(transform16[0], transform16[1], transform16[2], transform16[3]),
            SIMD4<Float>(transform16[4], transform16[5], transform16[6], transform16[7]),
            SIMD4<Float>(transform16[8], transform16[9], transform16[10], transform16[11]),
            SIMD4<Float>(transform16[12], transform16[13], transform16[14], transform16[15])
        )
    }

    /// Set the volume display mode.
    /// Called from `godot_avp_volume_camera_set_mode`.
    ///
    /// - Parameter modeValue: 0 = bounded, 1 = immersive.
    public func setMode(_ modeValue: Int32) {
        mode = VolumeMode(rawValue: Int(modeValue)) ?? .bounded
    }
}
