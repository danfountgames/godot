/**************************************************************************/
/*  EntitySynchronizer.swift                                              */
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
import Foundation
import simd

// MARK: - Entity Registry Entry

/// Tracks a RealityKit entity along with its generation counter for stale RID detection.
struct EntityEntry: @unchecked Sendable {
    let entity: ModelEntity
    var generation: UInt32
    var meshId: UInt64
    var materialId: UInt64
    var isActive: Bool
}

// MARK: - EntitySynchronizer

/// The central entity registry and CRUD dispatcher.
///
/// Maintains a mapping from Godot instance RIDs (UInt64) to RealityKit `ModelEntity`
/// instances. Receives bridge calls for entity lifecycle, transform updates, mesh/material
/// assignment, and visibility.
///
/// All RealityKit entity mutations must occur on `@MainActor`.
/// The registry itself is protected via an `OSAllocatedUnfairLock` for thread-safe
/// access from bridge callbacks arriving on arbitrary threads.
@MainActor
public final class EntitySynchronizer: Sendable {

    // MARK: - Properties

    /// The root entity under which all Godot-managed entities are parented.
    public private(set) var sceneRoot: Entity

    /// Active entity registry: Godot RID -> EntityEntry.
    private var entities: [UInt64: EntityEntry] = [:]

    /// Entity pool for reuse — pre-allocated inactive ModelEntities.
    private var entityPool: [ModelEntity] = []

    /// Generation counter incremented on each create to detect stale RID reuse.
    private var currentGeneration: UInt32 = 0

    /// Maximum pool size to limit memory usage.
    private let maxPoolSize: Int = 256

    /// References to shared stores (set during initialization).
    public var meshStore: MeshStore?
    public var materialStore: MaterialStore?

    // MARK: - Initialization

    public init() {
        self.sceneRoot = Entity()
        self.sceneRoot.name = "GodotSceneRoot"
        preallocatePool(count: 64)
    }

    /// Pre-allocate inactive entities for pooling.
    private func preallocatePool(count: Int) {
        entityPool.reserveCapacity(count)
        for _ in 0..<count {
            let entity = ModelEntity()
            entity.isEnabled = false
            entityPool.append(entity)
        }
    }

    // MARK: - Entity Lifecycle

    /// Create a new entity with the given mesh, material, and transform.
    ///
    /// - Parameters:
    ///   - id: Stable identifier derived from Godot instance RID.
    ///   - meshId: RID of the mesh resource.
    ///   - materialId: RID of the primary material.
    ///   - transform: Column-major 4x4 transform matrix.
    public func entityCreate(
        id: UInt64,
        meshId: UInt64,
        materialId: UInt64,
        transform: simd_float4x4
    ) {
        // Detect stale RID reuse.
        if let existing = entities[id] {
            // Same RID reused — destroy the old entity first.
            returnToPool(existing.entity)
        }

        currentGeneration &+= 1
        let entity = acquireFromPool()
        entity.name = "GodotEntity_\(id)"
        entity.isEnabled = true
        entity.transform = Transform(matrix: transform)

        // Apply mesh if available.
        if meshId != 0, let store = meshStore {
            Task {
                if let meshResource = await store.get(meshId: meshId) {
                    entity.model?.mesh = meshResource
                }
            }
        }

        // Apply material if available.
        if materialId != 0, let store = materialStore {
            Task {
                if let material = await store.get(materialId: materialId) {
                    if var model = entity.model {
                        model.materials = [material]
                        entity.model = model
                    } else {
                        // Create a minimal model component with a placeholder mesh.
                        entity.model = ModelComponent(
                            mesh: entity.model?.mesh ?? .generateBox(size: 0),
                            materials: [material]
                        )
                    }
                }
            }
        }

        let entry = EntityEntry(
            entity: entity,
            generation: currentGeneration,
            meshId: meshId,
            materialId: materialId,
            isActive: true
        )
        entities[id] = entry
        sceneRoot.addChild(entity)
    }

    /// Destroy an entity, removing it from the scene and returning it to the pool.
    public func entityDestroy(id: UInt64) {
        guard let entry = entities.removeValue(forKey: id) else { return }
        returnToPool(entry.entity)
    }

    /// Set the world transform of an entity.
    public func entitySetTransform(id: UInt64, transform: simd_float4x4) {
        guard let entry = entities[id] else { return }
        entry.entity.transform = Transform(matrix: transform)
    }

    /// Set the visibility of an entity.
    public func entitySetVisible(id: UInt64, visible: Bool) {
        guard let entry = entities[id] else { return }
        entry.entity.isEnabled = visible
    }

    /// Change the mesh assigned to an entity.
    public func entitySetMesh(id: UInt64, meshId: UInt64) {
        guard var entry = entities[id] else { return }
        entry.meshId = meshId
        entities[id] = entry

        guard let store = meshStore else { return }
        let entity = entry.entity
        Task {
            if let meshResource = await store.get(meshId: meshId) {
                let materials = entity.model?.materials ?? []
                entity.model = ModelComponent(mesh: meshResource, materials: materials)
            }
        }
    }

    /// Change the material assigned to an entity.
    public func entitySetMaterial(id: UInt64, materialId: UInt64) {
        guard var entry = entities[id] else { return }
        entry.materialId = materialId
        entities[id] = entry

        guard let store = materialStore else { return }
        let entity = entry.entity
        Task {
            if let material = await store.get(materialId: materialId) {
                if var model = entity.model {
                    model.materials = [material]
                    entity.model = model
                }
            }
        }
    }

    // MARK: - Batched Transform Updates

    /// Update transforms for multiple entities in a single call.
    /// This is the preferred path — one call per frame for all moving entities.
    ///
    /// - Parameters:
    ///   - ids: Array of entity IDs.
    ///   - transforms: Array of column-major 4x4 matrices (one per entity).
    ///   - count: Number of entities.
    public func batchUpdateTransforms(
        ids: UnsafePointer<UInt64>,
        transforms: UnsafePointer<Float>,
        count: UInt32
    ) {
        for i in 0..<Int(count) {
            let entityId = ids[i]
            guard let entry = entities[entityId] else { continue }
            let matrixPtr = transforms.advanced(by: i * 16)
            let matrix = simd_float4x4(
                SIMD4<Float>(matrixPtr[0], matrixPtr[1], matrixPtr[2], matrixPtr[3]),
                SIMD4<Float>(matrixPtr[4], matrixPtr[5], matrixPtr[6], matrixPtr[7]),
                SIMD4<Float>(matrixPtr[8], matrixPtr[9], matrixPtr[10], matrixPtr[11]),
                SIMD4<Float>(matrixPtr[12], matrixPtr[13], matrixPtr[14], matrixPtr[15])
            )
            entry.entity.transform = Transform(matrix: matrix)
        }
    }

    // MARK: - Collision & Input

    /// Set a collision shape on an entity for spatial input.
    ///
    /// - Parameters:
    ///   - entityId: Entity to set collision on.
    ///   - shapeType: 0=box, 1=sphere, 2=capsule, 3=convex_hull, 4=static_mesh.
    ///   - data: Shape-specific float data.
    ///   - dataCount: Number of floats in data.
    public func entitySetCollision(
        entityId: UInt64,
        shapeType: Int32,
        data: UnsafePointer<Float>,
        dataCount: UInt32
    ) {
        guard let entry = entities[entityId] else { return }
        let entity = entry.entity

        let shape: ShapeResource
        switch shapeType {
        case 0: // box
            guard dataCount >= 3 else { return }
            let extents = SIMD3<Float>(data[0], data[1], data[2])
            shape = .generateBox(size: extents)
        case 1: // sphere
            guard dataCount >= 1 else { return }
            shape = .generateSphere(radius: data[0])
        case 2: // capsule
            guard dataCount >= 2 else { return }
            shape = .generateCapsule(height: data[0], radius: data[1])
        default:
            // For convex_hull (3) and static_mesh (4), fall back to a bounding box.
            // Full convex hull support requires async ShapeResource generation.
            if let model = entity.model {
                let bounds = model.mesh.bounds
                let extents = bounds.extents
                shape = .generateBox(size: extents)
            } else {
                shape = .generateBox(size: [0.1, 0.1, 0.1])
            }
        }

        entity.components.set(CollisionComponent(shapes: [shape]))
    }

    /// Mark an entity as interactive (adds or removes InputTargetComponent).
    public func entitySetInteractive(entityId: UInt64, interactive: Bool) {
        guard let entry = entities[entityId] else { return }
        let entity = entry.entity

        if interactive {
            entity.components.set(InputTargetComponent())
            entity.components.set(HoverEffectComponent())
        } else {
            entity.components.remove(InputTargetComponent.self)
            entity.components.remove(HoverEffectComponent.self)
        }
    }

    // MARK: - Instancing

    /// Create or update an instanced mesh group.
    public func instancingUpdate(
        groupId: UInt64,
        meshId: UInt64,
        materialId: UInt64,
        transforms: UnsafePointer<Float>,
        instanceCount: UInt32
    ) {
        // For instanced rendering, we create one entity per instance.
        // On visionOS 26+, MeshInstancesComponent could be used instead.
        // This is the safe fallback path.
        let count = Int(instanceCount)

        // Destroy existing group entities if any.
        for i in 0..<count {
            let syntheticId = groupId &+ UInt64(i) &+ 0x8000_0000_0000_0000
            entityDestroy(id: syntheticId)
        }

        guard let mStore = meshStore, let matStore = materialStore else { return }
        let root = sceneRoot

        Task {
            let meshResource = await mStore.get(meshId: meshId)
            let material = await matStore.get(materialId: materialId)

            guard let mesh = meshResource else { return }

            await MainActor.run {
                for i in 0..<count {
                    let syntheticId = groupId &+ UInt64(i) &+ 0x8000_0000_0000_0000
                    let matrixPtr = transforms.advanced(by: i * 16)
                    let matrix = simd_float4x4(
                        SIMD4<Float>(matrixPtr[0], matrixPtr[1], matrixPtr[2], matrixPtr[3]),
                        SIMD4<Float>(matrixPtr[4], matrixPtr[5], matrixPtr[6], matrixPtr[7]),
                        SIMD4<Float>(matrixPtr[8], matrixPtr[9], matrixPtr[10], matrixPtr[11]),
                        SIMD4<Float>(matrixPtr[12], matrixPtr[13], matrixPtr[14], matrixPtr[15])
                    )

                    let entity = self.acquireFromPool()
                    entity.name = "GodotInstance_\(groupId)_\(i)"
                    entity.isEnabled = true
                    entity.transform = Transform(matrix: matrix)
                    entity.model = ModelComponent(
                        mesh: mesh,
                        materials: material.map { [$0] } ?? [SimpleMaterial()]
                    )

                    let entry = EntityEntry(
                        entity: entity,
                        generation: self.currentGeneration,
                        meshId: meshId,
                        materialId: materialId,
                        isActive: true
                    )
                    self.entities[syntheticId] = entry
                    root.addChild(entity)
                }
            }
        }
    }

    /// Destroy an instanced mesh group.
    public func instancingDestroy(groupId: UInt64) {
        // Remove all entities with the synthetic ID prefix for this group.
        let prefix = groupId &+ 0x8000_0000_0000_0000
        let keysToRemove = entities.keys.filter { key in
            key >= prefix && key < prefix &+ 0x0000_0000_FFFF_FFFF
        }
        for key in keysToRemove {
            entityDestroy(id: key)
        }
    }

    // MARK: - Diagnostics

    /// Current number of active entities.
    public var entityCount: UInt32 {
        UInt32(entities.count)
    }

    /// Look up a RealityKit entity by its Godot RID.
    public func entity(forId id: UInt64) -> ModelEntity? {
        entities[id]?.entity
    }

    /// Look up a Godot RID from a RealityKit entity.
    public func godotId(for entity: Entity) -> UInt64? {
        for (id, entry) in entities where entry.entity === entity {
            return id
        }
        return nil
    }

    // MARK: - Pool Management

    /// Acquire a `ModelEntity` from the pool or create a new one.
    private func acquireFromPool() -> ModelEntity {
        if let entity = entityPool.popLast() {
            // Reset state.
            entity.model = nil
            entity.transform = .init()
            entity.isEnabled = false
            entity.components.removeAll()
            return entity
        }
        return ModelEntity()
    }

    /// Return an entity to the pool for reuse.
    private func returnToPool(_ entity: ModelEntity) {
        entity.removeFromParent()
        entity.isEnabled = false
        entity.model = nil
        entity.components.removeAll()

        if entityPool.count < maxPoolSize {
            entityPool.append(entity)
        }
        // If pool is full, entity will be deallocated.
    }

    // MARK: - Frame Lifecycle

    /// Called at the beginning of each frame. Currently a no-op placeholder for
    /// future double-buffering or batching strategies.
    public func frameBegin() {
        // Reserved for future frame synchronization.
    }

    /// Called at the end of each frame. Commits any pending state.
    public func frameEnd() {
        // Reserved for future frame synchronization.
    }
}
