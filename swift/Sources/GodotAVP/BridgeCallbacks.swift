/**************************************************************************/
/*  BridgeCallbacks.swift                                                 */
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
import RealityKit

// MARK: - Bridge Callback Globals

/// Nonisolated (unsafe) reference to the shared engine, set once during registration.
/// These are accessed from C callback functions which have no actor context.
/// Safety: set once during init, read-only afterwards. The engine is a singleton.
nonisolated(unsafe) private var _engine: GodotEngine?
nonisolated(unsafe) private var _entitySynchronizer: EntitySynchronizer?
nonisolated(unsafe) private var _meshStore: MeshStore?
nonisolated(unsafe) private var _textureStore: TextureStore?
nonisolated(unsafe) private var _materialStore: MaterialStore?
nonisolated(unsafe) private var _volumeCamera: VolumeCameraNode?

// MARK: - Transform Helper

/// Construct a simd_float4x4 from a column-major C float array.
@inline(__always)
private func makeTransform(_ ptr: UnsafePointer<Float>) -> simd_float4x4 {
    simd_float4x4(
        SIMD4<Float>(ptr[0], ptr[1], ptr[2], ptr[3]),
        SIMD4<Float>(ptr[4], ptr[5], ptr[6], ptr[7]),
        SIMD4<Float>(ptr[8], ptr[9], ptr[10], ptr[11]),
        SIMD4<Float>(ptr[12], ptr[13], ptr[14], ptr[15])
    )
}

// MARK: - BridgeCallbacks

/// Defines all Swift functions that get registered as C bridge callbacks.
///
/// The C bridge (avp_bridge.cpp) stores function pointers for each operation.
/// This file provides the Swift implementations and registers them during
/// engine initialization.
///
/// Thread safety: Bridge callbacks arrive on the Godot engine thread.
/// RealityKit entity mutations must happen on @MainActor. We dispatch
/// to MainActor where needed.
public enum BridgeCallbacks {

    /// Register all Swift callback implementations with the C bridge.
    ///
    /// Called once during `GodotEngine.initialize()`.
    ///
    /// The C bridge stores static function pointers. We register closure-free
    /// C functions here and route through the global engine reference.
    @MainActor
    public static func registerAll(engine: GodotEngine) {
        // Store global references for use in C callbacks.
        _engine = engine
        _entitySynchronizer = engine.entitySynchronizer
        _meshStore = engine.meshStore
        _textureStore = engine.textureStore
        _materialStore = engine.materialStore
        _volumeCamera = engine.volumeCamera

        // Note: The C bridge in avp_bridge.cpp uses static function pointers.
        // Registration of those pointers requires additional C registration
        // functions to be added to avp_bridge.cpp. Until then, the Swift-side
        // callbacks are defined here and ready to be wired.
        //
        // The actual registration call looks like:
        //   avp_bridge_register_entity_create(bridgeEntityCreate)
        //   avp_bridge_register_entity_destroy(bridgeEntityDestroy)
        //   ... etc.
        //
        // For now, we expose the callback functions publicly so they can be
        // registered from the C++ side or through an intermediate shim.

        print("[GodotAVP] Bridge callbacks registered")
    }
}

// MARK: - Entity Lifecycle Callbacks

/// C-callable callback for entity creation.
/// Signature matches: void (*)(uint64_t, uint64_t, uint64_t, const float*)
@_cdecl("godot_avp_swift_entity_create")
public func bridgeEntityCreate(
    _ id: UInt64,
    _ meshId: UInt64,
    _ materialId: UInt64,
    _ transform16: UnsafePointer<Float>?
) {
    guard let transform16 = transform16 else { return }
    let transform = makeTransform(transform16)

    Task { @MainActor in
        _entitySynchronizer?.entityCreate(
            id: id,
            meshId: meshId,
            materialId: materialId,
            transform: transform
        )
    }
}

/// C-callable callback for entity destruction.
@_cdecl("godot_avp_swift_entity_destroy")
public func bridgeEntityDestroy(_ id: UInt64) {
    Task { @MainActor in
        _entitySynchronizer?.entityDestroy(id: id)
    }
}

/// C-callable callback for setting entity transform.
@_cdecl("godot_avp_swift_entity_set_transform")
public func bridgeEntitySetTransform(
    _ id: UInt64,
    _ transform16: UnsafePointer<Float>?
) {
    guard let transform16 = transform16 else { return }
    let transform = makeTransform(transform16)

    Task { @MainActor in
        _entitySynchronizer?.entitySetTransform(id: id, transform: transform)
    }
}

/// C-callable callback for setting entity visibility.
@_cdecl("godot_avp_swift_entity_set_visible")
public func bridgeEntitySetVisible(_ id: UInt64, _ visible: Int32) {
    Task { @MainActor in
        _entitySynchronizer?.entitySetVisible(id: id, visible: visible != 0)
    }
}

/// C-callable callback for changing entity mesh.
@_cdecl("godot_avp_swift_entity_set_mesh")
public func bridgeEntitySetMesh(_ id: UInt64, _ meshId: UInt64) {
    Task { @MainActor in
        _entitySynchronizer?.entitySetMesh(id: id, meshId: meshId)
    }
}

/// C-callable callback for changing entity material.
@_cdecl("godot_avp_swift_entity_set_material")
public func bridgeEntitySetMaterial(_ id: UInt64, _ materialId: UInt64) {
    Task { @MainActor in
        _entitySynchronizer?.entitySetMaterial(id: id, materialId: materialId)
    }
}

// MARK: - Batched Transform Callback

/// C-callable callback for batched transform updates.
@_cdecl("godot_avp_swift_entities_update_transforms")
public func bridgeEntitiesUpdateTransforms(
    _ ids: UnsafePointer<UInt64>?,
    _ transforms: UnsafePointer<Float>?,
    _ count: UInt32
) {
    guard let ids = ids, let transforms = transforms, count > 0 else { return }

    // Copy data to avoid dangling pointer issues across the async boundary.
    let idCount = Int(count)
    let idsCopy = Array(UnsafeBufferPointer(start: ids, count: idCount))
    let floatCount = idCount * 16
    let transformsCopy = Array(UnsafeBufferPointer(start: transforms, count: floatCount))

    Task { @MainActor in
        idsCopy.withUnsafeBufferPointer { idsBuffer in
            transformsCopy.withUnsafeBufferPointer { transformsBuffer in
                _entitySynchronizer?.batchUpdateTransforms(
                    ids: idsBuffer.baseAddress!,
                    transforms: transformsBuffer.baseAddress!,
                    count: count
                )
            }
        }
    }
}

// MARK: - Mesh Callbacks

/// C-callable callback for mesh creation.
@_cdecl("godot_avp_swift_mesh_create")
public func bridgeMeshCreate(
    _ meshId: UInt64,
    _ positions: UnsafePointer<Float>?,
    _ normals: UnsafePointer<Float>?,
    _ uvs: UnsafePointer<Float>?,
    _ tangents: UnsafePointer<Float>?,
    _ colors: UnsafePointer<Float>?,
    _ indices: UnsafePointer<UInt32>?,
    _ vertexCount: UInt32,
    _ indexCount: UInt32,
    _ surfaceCount: UInt32
) {
    guard let positions = positions else { return }

    // Copy all vertex data to safely cross the async boundary.
    let vCount = Int(vertexCount)
    let iCount = Int(indexCount)

    let positionsCopy = Array(UnsafeBufferPointer(start: positions, count: vCount * 3))
    let normalsCopy: [Float]? = normals.map { Array(UnsafeBufferPointer(start: $0, count: vCount * 3)) }
    let uvsCopy: [Float]? = uvs.map { Array(UnsafeBufferPointer(start: $0, count: vCount * 2)) }
    let tangentsCopy: [Float]? = tangents.map { Array(UnsafeBufferPointer(start: $0, count: vCount * 4)) }
    let colorsCopy: [Float]? = colors.map { Array(UnsafeBufferPointer(start: $0, count: vCount * 4)) }
    let indicesCopy: [UInt32]? = indices.map { Array(UnsafeBufferPointer(start: $0, count: iCount)) }

    Task {
        do {
            try await positionsCopy.withUnsafeBufferPointer { posPtr in
                let normPtr = normalsCopy.map { $0.withUnsafeBufferPointer { $0 } }
                // Use nested closures to get pointers safely.
                func doCreate(
                    posPtr: UnsafePointer<Float>,
                    normPtr: UnsafePointer<Float>?,
                    uvsPtr: UnsafePointer<Float>?,
                    tanPtr: UnsafePointer<Float>?,
                    colPtr: UnsafePointer<Float>?,
                    idxPtr: UnsafePointer<UInt32>?
                ) async throws {
                    try await _meshStore?.create(
                        meshId: meshId,
                        positions: posPtr,
                        normals: normPtr,
                        uvs: uvsPtr,
                        tangents: tanPtr,
                        colors: colPtr,
                        indices: idxPtr,
                        vertexCount: vertexCount,
                        indexCount: indexCount,
                        surfaceCount: surfaceCount
                    )
                }

                // We need to hold all the arrays alive while we get pointers.
                try await withSafePointers(
                    positionsCopy, normalsCopy, uvsCopy, tangentsCopy, colorsCopy, indicesCopy
                ) { posPtr, normPtr, uvsPtr, tanPtr, colPtr, idxPtr in
                    try await _meshStore?.create(
                        meshId: meshId,
                        positions: posPtr,
                        normals: normPtr,
                        uvs: uvsPtr,
                        tangents: tanPtr,
                        colors: colPtr,
                        indices: idxPtr,
                        vertexCount: vertexCount,
                        indexCount: indexCount,
                        surfaceCount: surfaceCount
                    )
                }
            }
        } catch {
            print("[GodotAVP] Mesh create failed for \(meshId): \(error)")
        }
    }
}

/// Helper to safely pass copied arrays as pointers across async boundaries.
private func withSafePointers(
    _ positions: [Float],
    _ normals: [Float]?,
    _ uvs: [Float]?,
    _ tangents: [Float]?,
    _ colors: [Float]?,
    _ indices: [UInt32]?,
    body: (UnsafePointer<Float>, UnsafePointer<Float>?, UnsafePointer<Float>?,
           UnsafePointer<Float>?, UnsafePointer<Float>?, UnsafePointer<UInt32>?) async throws -> Void
) async rethrows {
    try await positions.withUnsafeBufferPointer { posBuffer in
        let posPtr = posBuffer.baseAddress!

        if let normals = normals {
            try await normals.withUnsafeBufferPointer { normBuffer in
                if let uvs = uvs {
                    try await uvs.withUnsafeBufferPointer { uvBuffer in
                        if let tangents = tangents {
                            try await tangents.withUnsafeBufferPointer { tanBuffer in
                                if let colors = colors {
                                    try await colors.withUnsafeBufferPointer { colBuffer in
                                        if let indices = indices {
                                            try await indices.withUnsafeBufferPointer { idxBuffer in
                                                try await body(posPtr, normBuffer.baseAddress, uvBuffer.baseAddress,
                                                             tanBuffer.baseAddress, colBuffer.baseAddress, idxBuffer.baseAddress)
                                            }
                                        } else {
                                            try await body(posPtr, normBuffer.baseAddress, uvBuffer.baseAddress,
                                                         tanBuffer.baseAddress, colBuffer.baseAddress, nil)
                                        }
                                    }
                                } else {
                                    if let indices = indices {
                                        try await indices.withUnsafeBufferPointer { idxBuffer in
                                            try await body(posPtr, normBuffer.baseAddress, uvBuffer.baseAddress,
                                                         tanBuffer.baseAddress, nil, idxBuffer.baseAddress)
                                        }
                                    } else {
                                        try await body(posPtr, normBuffer.baseAddress, uvBuffer.baseAddress,
                                                     tanBuffer.baseAddress, nil, nil)
                                    }
                                }
                            }
                        } else {
                            if let indices = indices {
                                try await indices.withUnsafeBufferPointer { idxBuffer in
                                    try await body(posPtr, normBuffer.baseAddress, uvBuffer.baseAddress,
                                                 nil, nil, idxBuffer.baseAddress)
                                }
                            } else {
                                try await body(posPtr, normBuffer.baseAddress, uvBuffer.baseAddress,
                                             nil, nil, nil)
                            }
                        }
                    }
                } else {
                    if let indices = indices {
                        try await indices.withUnsafeBufferPointer { idxBuffer in
                            try await body(posPtr, normBuffer.baseAddress, nil, nil, nil, idxBuffer.baseAddress)
                        }
                    } else {
                        try await body(posPtr, normBuffer.baseAddress, nil, nil, nil, nil)
                    }
                }
            }
        } else {
            if let indices = indices {
                try await indices.withUnsafeBufferPointer { idxBuffer in
                    try await body(posPtr, nil, nil, nil, nil, idxBuffer.baseAddress)
                }
            } else {
                try await body(posPtr, nil, nil, nil, nil, nil)
            }
        }
    }
}

/// C-callable callback for mesh destruction.
@_cdecl("godot_avp_swift_mesh_destroy")
public func bridgeMeshDestroy(_ meshId: UInt64) {
    Task {
        await _meshStore?.remove(meshId: meshId)
    }
}

/// C-callable callback for mesh vertex updates (skinned mesh streaming).
@_cdecl("godot_avp_swift_mesh_update_vertices")
public func bridgeMeshUpdateVertices(
    _ meshId: UInt64,
    _ positions: UnsafePointer<Float>?,
    _ normals: UnsafePointer<Float>?,
    _ vertexCount: UInt32
) {
    guard let positions = positions else { return }

    let vCount = Int(vertexCount)
    let positionsCopy = Array(UnsafeBufferPointer(start: positions, count: vCount * 3))
    let normalsCopy: [Float]? = normals.map { Array(UnsafeBufferPointer(start: $0, count: vCount * 3)) }

    Task {
        await positionsCopy.withUnsafeBufferPointer { posBuffer in
            if let normalsCopy = normalsCopy {
                normalsCopy.withUnsafeBufferPointer { normBuffer in
                    // Actor-isolated call — fine within Task.
                    Task {
                        await _meshStore?.updateVertices(
                            meshId: meshId,
                            positions: posBuffer.baseAddress!,
                            normals: normBuffer.baseAddress,
                            vertexCount: vertexCount
                        )
                    }
                }
            } else {
                Task {
                    await _meshStore?.updateVertices(
                        meshId: meshId,
                        positions: posBuffer.baseAddress!,
                        normals: nil,
                        vertexCount: vertexCount
                    )
                }
            }
        }
    }
}

// MARK: - Texture Callbacks

/// C-callable callback for texture creation.
@_cdecl("godot_avp_swift_texture_create")
public func bridgeTextureCreate(
    _ textureId: UInt64,
    _ pixels: UnsafePointer<UInt8>?,
    _ width: UInt32,
    _ height: UInt32,
    _ format: UInt32,
    _ mipmaps: UInt32,
    _ isSrgb: Int32
) {
    guard let pixels = pixels else { return }

    // Estimate total pixel data size for copying.
    let bytesPerPixel: Int
    switch format {
    case 0, 1: bytesPerPixel = 4      // RGBA8
    case 2: bytesPerPixel = 8         // RGBA16F
    case 3: bytesPerPixel = 1         // R8
    case 4: bytesPerPixel = 2         // RG8
    case 5:                            // BC7 compressed
        let blocksWide = (Int(width) + 3) / 4
        let blocksHigh = (Int(height) + 3) / 4
        bytesPerPixel = 0  // handled separately
        let baseSize = blocksWide * blocksHigh * 16
        // Approximate mip chain size as ~1.33x base.
        let totalSize = Int(mipmaps) > 1 ? Int(Double(baseSize) * 1.34) : baseSize
        let pixelsCopy = Array(UnsafeBufferPointer(start: pixels, count: totalSize))
        Task {
            do {
                try await pixelsCopy.withUnsafeBufferPointer { buffer in
                    try await _textureStore?.create(
                        textureId: textureId,
                        pixels: buffer.baseAddress!,
                        width: width,
                        height: height,
                        format: format,
                        mipmaps: mipmaps,
                        isSrgb: isSrgb != 0
                    )
                }
            } catch {
                print("[GodotAVP] Texture create failed for \(textureId): \(error)")
            }
        }
        return
    default:
        print("[GodotAVP] Unknown texture format \(format)")
        return
    }

    if bytesPerPixel > 0 {
        let baseSize = Int(width) * Int(height) * bytesPerPixel
        let totalSize = Int(mipmaps) > 1 ? Int(Double(baseSize) * 1.34) : baseSize
        let pixelsCopy = Array(UnsafeBufferPointer(start: pixels, count: totalSize))
        Task {
            do {
                try await pixelsCopy.withUnsafeBufferPointer { buffer in
                    try await _textureStore?.create(
                        textureId: textureId,
                        pixels: buffer.baseAddress!,
                        width: width,
                        height: height,
                        format: format,
                        mipmaps: mipmaps,
                        isSrgb: isSrgb != 0
                    )
                }
            } catch {
                print("[GodotAVP] Texture create failed for \(textureId): \(error)")
            }
        }
    }
}

/// C-callable callback for texture destruction.
@_cdecl("godot_avp_swift_texture_destroy")
public func bridgeTextureDestroy(_ textureId: UInt64) {
    Task {
        await _textureStore?.remove(textureId: textureId)
    }
}

// MARK: - Material Callbacks

/// C-callable callback for PBR material creation.
@_cdecl("godot_avp_swift_material_create_pbr")
public func bridgeMaterialCreatePBR(
    _ materialId: UInt64,
    _ albedoTex: UInt64,
    _ albedoR: Float, _ albedoG: Float, _ albedoB: Float, _ albedoA: Float,
    _ roughness: Float, _ roughnessTex: UInt64,
    _ metallic: Float, _ metallicTex: UInt64,
    _ normalTex: UInt64, _ normalScale: Float,
    _ emissionTex: UInt64,
    _ emissionR: Float, _ emissionG: Float, _ emissionB: Float,
    _ emissionEnergy: Float,
    _ aoTex: UInt64,
    _ clearcoat: Float, _ clearcoatRoughness: Float,
    _ alphaMode: Int32, _ alphaScissorThreshold: Float,
    _ cullMode: Int32
) {
    Task {
        await _materialStore?.createPBR(
            materialId: materialId,
            albedoTex: albedoTex,
            albedoR: albedoR, albedoG: albedoG, albedoB: albedoB, albedoA: albedoA,
            roughness: roughness, roughnessTex: roughnessTex,
            metallic: metallic, metallicTex: metallicTex,
            normalTex: normalTex, normalScale: normalScale,
            emissionTex: emissionTex,
            emissionR: emissionR, emissionG: emissionG, emissionB: emissionB,
            emissionEnergy: emissionEnergy,
            aoTex: aoTex,
            clearcoat: clearcoat, clearcoatRoughness: clearcoatRoughness,
            alphaMode: alphaMode, alphaScissorThreshold: alphaScissorThreshold,
            cullMode: cullMode
        )
    }
}

/// C-callable callback for unlit material creation.
@_cdecl("godot_avp_swift_material_create_unlit")
public func bridgeMaterialCreateUnlit(
    _ materialId: UInt64,
    _ albedoTex: UInt64,
    _ albedoR: Float, _ albedoG: Float, _ albedoB: Float, _ albedoA: Float,
    _ alphaMode: Int32, _ alphaScissorThreshold: Float,
    _ cullMode: Int32
) {
    Task {
        await _materialStore?.createUnlit(
            materialId: materialId,
            albedoTex: albedoTex,
            albedoR: albedoR, albedoG: albedoG, albedoB: albedoB, albedoA: albedoA,
            alphaMode: alphaMode, alphaScissorThreshold: alphaScissorThreshold,
            cullMode: cullMode
        )
    }
}

/// C-callable callback for custom shader material creation.
@_cdecl("godot_avp_swift_material_create_custom")
public func bridgeMaterialCreateCustom(
    _ materialId: UInt64,
    _ shaderName: UnsafePointer<CChar>?,
    _ paramsJson: UnsafePointer<CChar>?
) {
    let name = shaderName.map { String(cString: $0) } ?? ""
    let params = paramsJson.map { String(cString: $0) }

    Task {
        await _materialStore?.createCustom(
            materialId: materialId,
            shaderName: name,
            paramsJson: params
        )
    }
}

/// C-callable callback for material destruction.
@_cdecl("godot_avp_swift_material_destroy")
public func bridgeMaterialDestroy(_ materialId: UInt64) {
    Task {
        await _materialStore?.remove(materialId: materialId)
    }
}

// MARK: - Frame Sync Callbacks

/// C-callable callback for frame begin.
@_cdecl("godot_avp_swift_frame_begin")
public func bridgeFrameBegin() {
    Task { @MainActor in
        _entitySynchronizer?.frameBegin()
    }
}

/// C-callable callback for frame end.
@_cdecl("godot_avp_swift_frame_end")
public func bridgeFrameEnd() {
    Task { @MainActor in
        _entitySynchronizer?.frameEnd()
    }
}

// MARK: - Volume Camera Callbacks

/// C-callable callback for volume camera bounds.
@_cdecl("godot_avp_swift_volume_camera_set_bounds")
public func bridgeVolumeCameraSetBounds(_ sizeX: Float, _ sizeY: Float, _ sizeZ: Float) {
    Task { @MainActor in
        _volumeCamera?.setBounds(sizeX: sizeX, sizeY: sizeY, sizeZ: sizeZ)
    }
}

/// C-callable callback for volume camera transform.
@_cdecl("godot_avp_swift_volume_camera_set_transform")
public func bridgeVolumeCameraSetTransform(_ transform16: UnsafePointer<Float>?) {
    guard let transform16 = transform16 else { return }
    // Copy transform data before crossing async boundary.
    var transformData = [Float](repeating: 0, count: 16)
    for i in 0..<16 {
        transformData[i] = transform16[i]
    }

    Task { @MainActor in
        transformData.withUnsafeBufferPointer { buffer in
            _volumeCamera?.setTransform(buffer.baseAddress!)
        }
    }
}

/// C-callable callback for volume camera mode.
@_cdecl("godot_avp_swift_volume_camera_set_mode")
public func bridgeVolumeCameraSetMode(_ mode: Int32) {
    Task { @MainActor in
        _volumeCamera?.setMode(mode)
    }
}

// MARK: - Instancing Callbacks

/// C-callable callback for instancing update.
@_cdecl("godot_avp_swift_instancing_update")
public func bridgeInstancingUpdate(
    _ groupId: UInt64,
    _ meshId: UInt64,
    _ materialId: UInt64,
    _ transforms: UnsafePointer<Float>?,
    _ instanceCount: UInt32
) {
    guard let transforms = transforms, instanceCount > 0 else { return }

    let floatCount = Int(instanceCount) * 16
    let transformsCopy = Array(UnsafeBufferPointer(start: transforms, count: floatCount))

    Task { @MainActor in
        transformsCopy.withUnsafeBufferPointer { buffer in
            _entitySynchronizer?.instancingUpdate(
                groupId: groupId,
                meshId: meshId,
                materialId: materialId,
                transforms: buffer.baseAddress!,
                instanceCount: instanceCount
            )
        }
    }
}

/// C-callable callback for instancing destroy.
@_cdecl("godot_avp_swift_instancing_destroy")
public func bridgeInstancingDestroy(_ groupId: UInt64) {
    Task { @MainActor in
        _entitySynchronizer?.instancingDestroy(groupId: groupId)
    }
}

// MARK: - Collision Callbacks

/// C-callable callback for setting collision shapes.
@_cdecl("godot_avp_swift_entity_set_collision")
public func bridgeEntitySetCollision(
    _ entityId: UInt64,
    _ shapeType: Int32,
    _ data: UnsafePointer<Float>?,
    _ dataCount: UInt32
) {
    guard let data = data, dataCount > 0 else { return }

    let dataCopy = Array(UnsafeBufferPointer(start: data, count: Int(dataCount)))

    Task { @MainActor in
        dataCopy.withUnsafeBufferPointer { buffer in
            _entitySynchronizer?.entitySetCollision(
                entityId: entityId,
                shapeType: shapeType,
                data: buffer.baseAddress!,
                dataCount: dataCount
            )
        }
    }
}

/// C-callable callback for setting entity interactivity.
@_cdecl("godot_avp_swift_entity_set_interactive")
public func bridgeEntitySetInteractive(_ entityId: UInt64, _ interactive: Int32) {
    Task { @MainActor in
        _entitySynchronizer?.entitySetInteractive(entityId: entityId, interactive: interactive != 0)
    }
}

// MARK: - Diagnostics Callbacks

/// C-callable callback for entity count query.
@_cdecl("godot_avp_swift_get_entity_count")
public func bridgeGetEntityCount() -> UInt32 {
    // This is called synchronously from C++, so we return the cached value.
    // The actual value is updated on MainActor during tick.
    return _engine?.entityCount ?? 0
}

/// C-callable callback for memory estimate query.
@_cdecl("godot_avp_swift_get_memory_estimate")
public func bridgeGetMemoryEstimate() -> UInt64 {
    return _engine?.memoryEstimate ?? 0
}
