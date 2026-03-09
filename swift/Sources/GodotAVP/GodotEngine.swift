/**************************************************************************/
/*  GodotEngine.swift                                                     */
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
import Observation
import RealityKit
import simd

// MARK: - Engine State

/// The lifecycle state of the Godot engine.
public enum EngineState: Sendable {
    case uninitialized
    case initializing
    case running
    case paused
    case stopping
    case stopped
}

// MARK: - GodotEngine

/// Wraps the Godot engine lifecycle and manages the cooperative main-thread
/// ticking model.
///
/// Architecture (Option A from RK_PLAN.md section 2.12):
///   The Godot engine tick is driven cooperatively from the main thread.
///   Each RealityKit frame, the app calls `tick()` which advances
///   Godot's simulation by one frame. Bridge callbacks fire during the
///   tick, creating/updating/destroying RealityKit entities.
///
/// This class connects all the subsystems:
///   - EntitySynchronizer — entity CRUD
///   - MeshStore — mesh resource cache
///   - TextureStore — texture resource cache
///   - MaterialStore — material cache
///   - VolumeCameraNode — volume camera state
///   - InputBridge — spatial input routing
///
/// Observable for SwiftUI reactivity (engine state, entity counts, etc.).
@Observable
@MainActor
public final class GodotEngine {

    // MARK: - Singleton

    /// Shared instance. The engine is a singleton since Godot itself is a singleton.
    public static let shared = GodotEngine()

    // MARK: - Subsystems

    /// Entity registry and CRUD dispatcher.
    public let entitySynchronizer: EntitySynchronizer

    /// Mesh resource cache.
    public let meshStore: MeshStore

    /// Texture resource cache.
    public let textureStore: TextureStore

    /// Material cache.
    public let materialStore: MaterialStore

    /// Volume camera configuration.
    public let volumeCamera: VolumeCameraNode

    /// Spatial input bridge.
    public let inputBridge: InputBridge

    // MARK: - Observable State

    /// Current engine lifecycle state.
    public private(set) var state: EngineState = .uninitialized

    /// Number of active RealityKit entities.
    public private(set) var entityCount: UInt32 = 0

    /// Estimated memory usage across all stores (bytes).
    public private(set) var memoryEstimate: UInt64 = 0

    /// Frames per second (computed from tick timing).
    public private(set) var fps: Double = 0

    /// Total frame count since engine start.
    public private(set) var frameCount: UInt64 = 0

    // MARK: - Frame Timing

    /// Timestamp of the last tick for delta time calculation.
    private var lastTickTime: CFAbsoluteTime = 0

    /// Target frame interval (1/90 for 90 Hz visionOS refresh rate).
    private let targetFrameInterval: TimeInterval = 1.0 / 90.0

    /// Smoothed frame time for FPS calculation.
    private var smoothedFrameTime: Double = 1.0 / 90.0

    // MARK: - Initialization

    private init() {
        self.entitySynchronizer = EntitySynchronizer()
        self.meshStore = MeshStore()
        self.textureStore = TextureStore()
        self.materialStore = MaterialStore(textureStore: textureStore)
        self.volumeCamera = VolumeCameraNode()
        self.inputBridge = InputBridge(entitySynchronizer: entitySynchronizer)

        // Wire subsystem references.
        entitySynchronizer.meshStore = meshStore
        entitySynchronizer.materialStore = materialStore
    }

    // MARK: - Engine Lifecycle

    /// Initialize the Godot engine and register bridge callbacks.
    ///
    /// This must be called before any ticking. It:
    /// 1. Registers all Swift callback implementations with the C bridge.
    /// 2. Initializes the Godot engine runtime.
    /// 3. Transitions state to `.running`.
    public func initialize() {
        guard state == .uninitialized else {
            print("[GodotAVP] Engine already initialized (state: \(state))")
            return
        }

        state = .initializing

        // Register Swift-side callbacks with the C bridge.
        BridgeCallbacks.registerAll(engine: self)

        // Register replacement shader factories with the material store.
        // These map @rk_shader annotations in Godot shaders to Swift factories.
        Task {
            await ToonMaterial.register(with: materialStore)
            await GlassMaterial.register(with: materialStore)
            await HologramMaterial.register(with: materialStore)
            await WaterMaterial.register(with: materialStore)
            print("[GodotAVP] Replacement shaders registered (Toon, Glass, Hologram, Water)")
        }

        state = .running
        lastTickTime = CFAbsoluteTimeGetCurrent()

        print("[GodotAVP] Engine initialized and running")
    }

    /// Perform one frame of Godot engine simulation.
    ///
    /// Called cooperatively from the RealityKit render loop (via `RealityView.update`
    /// or a `TimelineView`). Each call advances Godot by one simulation frame.
    ///
    /// The bridge callbacks fire during this tick, issuing entity create/update/destroy
    /// calls to the EntitySynchronizer.
    public func tick() {
        guard state == .running else { return }

        let now = CFAbsoluteTimeGetCurrent()
        let deltaTime = now - lastTickTime
        lastTickTime = now

        // Update FPS calculation with exponential smoothing.
        let alpha = 0.05
        smoothedFrameTime = smoothedFrameTime * (1.0 - alpha) + deltaTime * alpha
        fps = 1.0 / smoothedFrameTime

        frameCount += 1

        // Signal frame begin to the entity synchronizer.
        entitySynchronizer.frameBegin()

        // --- Godot engine tick would happen here ---
        // In the full integration, this calls into the Godot C runtime to
        // advance one frame. The Godot rendering pipeline then calls back
        // through the bridge to create/update/destroy entities.
        //
        // For now, this is a placeholder. The actual integration requires
        // linking against the Godot engine library and calling:
        //   godot_main_iteration()
        // or equivalent.

        // Signal frame end.
        entitySynchronizer.frameEnd()

        // Update diagnostics.
        entityCount = entitySynchronizer.entityCount
        Task {
            let meshMem = await meshStore.memoryEstimate
            let texMem = await textureStore.memoryEstimate
            let matMem = await materialStore.memoryEstimate
            await MainActor.run {
                memoryEstimate = meshMem + texMem + matMem
            }
        }
    }

    /// Pause the engine tick loop.
    public func pause() {
        guard state == .running else { return }
        state = .paused
        print("[GodotAVP] Engine paused")
    }

    /// Resume the engine tick loop.
    public func resume() {
        guard state == .paused else { return }
        state = .running
        lastTickTime = CFAbsoluteTimeGetCurrent()
        print("[GodotAVP] Engine resumed")
    }

    /// Stop the engine and clean up resources.
    public func stop() {
        guard state == .running || state == .paused else { return }
        state = .stopping

        // Cleanup would happen here — destroy all entities, flush stores, etc.

        state = .stopped
        print("[GodotAVP] Engine stopped")
    }

    // MARK: - Scene Root

    /// The root entity for the RealityKit scene.
    /// All Godot-managed entities are children of this entity.
    public var sceneRoot: Entity {
        entitySynchronizer.sceneRoot
    }

    // MARK: - Diagnostics

    /// Get the current entity count.
    public func getEntityCount() -> UInt32 {
        entitySynchronizer.entityCount
    }

    /// Get the estimated memory usage across all stores.
    public func getMemoryEstimate() -> UInt64 {
        memoryEstimate
    }
}
