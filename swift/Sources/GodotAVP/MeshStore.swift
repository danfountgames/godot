/**************************************************************************/
/*  MeshStore.swift                                                       */
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
import Metal
import simd

// MARK: - Vertex Layout

/// Interleaved vertex data matching the bridge vertex attribute layout.
/// position (float3), normal (float3), uv (float2), tangent (float4), color (float4).
struct GodotVertex {
    var position: SIMD3<Float>
    var normal: SIMD3<Float>
    var uv: SIMD2<Float>
    var tangent: SIMD4<Float>
    var color: SIMD4<Float>

    static let zero = GodotVertex(
        position: .zero,
        normal: SIMD3<Float>(0, 1, 0),
        uv: .zero,
        tangent: SIMD4<Float>(1, 0, 0, 1),
        color: SIMD4<Float>(1, 1, 1, 1)
    )
}

// MARK: - MeshCacheEntry

/// A cached mesh resource along with its LowLevelMesh for GPU-side updates.
struct MeshCacheEntry {
    let meshResource: MeshResource
    let lowLevelMesh: LowLevelMesh
    var vertexCount: Int
    var indexCount: Int
    /// Estimated GPU memory in bytes.
    var memoryEstimate: UInt64
}

// MARK: - MeshStore

/// Actor-based store for `LowLevelMesh` / `MeshResource` objects.
///
/// Maps Godot mesh RIDs (UInt64) to RealityKit `MeshResource` instances.
/// Creates meshes via `LowLevelMesh` with a custom interleaved vertex layout
/// that matches the data streamed from the C bridge.
///
/// Thread-safe via Swift actor isolation.
public actor MeshStore {

    // MARK: - Properties

    /// Cache of mesh ID -> MeshCacheEntry.
    private var cache: [UInt64: MeshCacheEntry] = [:]

    /// Total estimated GPU memory for all cached meshes.
    private var totalMemoryEstimate: UInt64 = 0

    // MARK: - Vertex Attributes

    /// The fixed vertex attribute layout used for all Godot meshes.
    private static let vertexAttributes: [LowLevelMesh.Attribute] = [
        .init(
            semantic: .position,
            format: .float3,
            offset: 0
        ),
        .init(
            semantic: .normal,
            format: .float3,
            offset: MemoryLayout<SIMD3<Float>>.stride
        ),
        .init(
            semantic: .uv0,
            format: .float2,
            offset: MemoryLayout<SIMD3<Float>>.stride * 2
        ),
        .init(
            semantic: .tangent,
            format: .float4,
            offset: MemoryLayout<SIMD3<Float>>.stride * 2 + MemoryLayout<SIMD2<Float>>.stride
        ),
        .init(
            semantic: .color,
            format: .float4,
            offset: MemoryLayout<SIMD3<Float>>.stride * 2 + MemoryLayout<SIMD2<Float>>.stride + MemoryLayout<SIMD4<Float>>.stride
        ),
    ]

    private static let vertexLayouts: [LowLevelMesh.Layout] = [
        .init(bufferIndex: 0, bufferStride: MemoryLayout<GodotVertex>.stride)
    ]

    // MARK: - Public API

    /// Retrieve a cached mesh resource, or nil if not found.
    public func get(meshId: UInt64) -> MeshResource? {
        cache[meshId]?.meshResource
    }

    /// Create a mesh from raw vertex/index data streamed from the C bridge.
    ///
    /// - Parameters:
    ///   - meshId: Unique mesh identifier from Godot RID.
    ///   - positions: Vertex positions (3 floats per vertex: x, y, z).
    ///   - normals: Vertex normals (3 floats per vertex, may be nil).
    ///   - uvs: Texture coordinates (2 floats per vertex, may be nil).
    ///   - tangents: Tangent vectors (4 floats per vertex: x, y, z, w; may be nil).
    ///   - colors: Vertex colors (4 floats per vertex: r, g, b, a; may be nil).
    ///   - indices: Triangle indices.
    ///   - vertexCount: Number of vertices.
    ///   - indexCount: Number of indices.
    ///   - surfaceCount: Number of surfaces (currently treated as 1 part).
    public func create(
        meshId: UInt64,
        positions: UnsafePointer<Float>,
        normals: UnsafePointer<Float>?,
        uvs: UnsafePointer<Float>?,
        tangents: UnsafePointer<Float>?,
        colors: UnsafePointer<Float>?,
        indices: UnsafePointer<UInt32>?,
        vertexCount: UInt32,
        indexCount: UInt32,
        surfaceCount: UInt32
    ) throws {
        // Remove existing mesh if present.
        if cache[meshId] != nil {
            remove(meshId: meshId)
        }

        let vCount = Int(vertexCount)
        let iCount = Int(indexCount)

        // Create the LowLevelMesh descriptor.
        var descriptor = LowLevelMesh.Descriptor()
        descriptor.vertexCapacity = vCount
        descriptor.vertexAttributes = MeshStore.vertexAttributes
        descriptor.vertexLayouts = MeshStore.vertexLayouts
        descriptor.indexCapacity = iCount > 0 ? iCount : vCount * 3
        descriptor.indexType = .uint32

        let lowLevelMesh = try LowLevelMesh(descriptor: descriptor)

        // Fill vertex buffer.
        lowLevelMesh.withUnsafeMutableBytes(bufferIndex: 0) { rawBuffer in
            let vertexBuffer = rawBuffer.bindMemory(to: GodotVertex.self)
            for i in 0..<vCount {
                var vertex = GodotVertex.zero

                // Position (required).
                vertex.position = SIMD3<Float>(
                    positions[i * 3],
                    positions[i * 3 + 1],
                    positions[i * 3 + 2]
                )

                // Normal (optional).
                if let normals = normals {
                    vertex.normal = SIMD3<Float>(
                        normals[i * 3],
                        normals[i * 3 + 1],
                        normals[i * 3 + 2]
                    )
                }

                // UV (optional).
                if let uvs = uvs {
                    vertex.uv = SIMD2<Float>(
                        uvs[i * 2],
                        uvs[i * 2 + 1]
                    )
                }

                // Tangent (optional).
                if let tangents = tangents {
                    vertex.tangent = SIMD4<Float>(
                        tangents[i * 4],
                        tangents[i * 4 + 1],
                        tangents[i * 4 + 2],
                        tangents[i * 4 + 3]
                    )
                }

                // Color (optional).
                if let colors = colors {
                    vertex.color = SIMD4<Float>(
                        colors[i * 4],
                        colors[i * 4 + 1],
                        colors[i * 4 + 2],
                        colors[i * 4 + 3]
                    )
                }

                vertexBuffer[i] = vertex
            }
        }

        // Fill index buffer.
        if iCount > 0, let indices = indices {
            lowLevelMesh.withUnsafeMutableIndices { rawBuffer in
                let indexBuffer = rawBuffer.bindMemory(to: UInt32.self)
                for i in 0..<iCount {
                    indexBuffer[i] = indices[i]
                }
            }
        } else {
            // Generate sequential indices if none provided.
            lowLevelMesh.withUnsafeMutableIndices { rawBuffer in
                let indexBuffer = rawBuffer.bindMemory(to: UInt32.self)
                for i in 0..<vCount {
                    indexBuffer[i] = UInt32(i)
                }
            }
        }

        // Compute bounding box for the mesh part.
        var minBound = SIMD3<Float>(Float.greatestFiniteMagnitude,
                                     Float.greatestFiniteMagnitude,
                                     Float.greatestFiniteMagnitude)
        var maxBound = SIMD3<Float>(-Float.greatestFiniteMagnitude,
                                     -Float.greatestFiniteMagnitude,
                                     -Float.greatestFiniteMagnitude)
        for i in 0..<vCount {
            let pos = SIMD3<Float>(
                positions[i * 3],
                positions[i * 3 + 1],
                positions[i * 3 + 2]
            )
            minBound = min(minBound, pos)
            maxBound = max(maxBound, pos)
        }

        let bounds = BoundingBox(min: minBound, max: maxBound)
        let effectiveIndexCount = iCount > 0 ? iCount : vCount

        // Add a single mesh part covering all geometry.
        let part = LowLevelMesh.Part(
            indexCount: effectiveIndexCount,
            topology: .triangle,
            bounds: bounds
        )
        lowLevelMesh.parts.replaceAll([part])

        let meshResource = try MeshResource(from: lowLevelMesh)

        // Calculate memory estimate.
        let vertexMemory = UInt64(vCount * MemoryLayout<GodotVertex>.stride)
        let indexMemory = UInt64(effectiveIndexCount * MemoryLayout<UInt32>.stride)
        let memEstimate = vertexMemory + indexMemory

        let entry = MeshCacheEntry(
            meshResource: meshResource,
            lowLevelMesh: lowLevelMesh,
            vertexCount: vCount,
            indexCount: effectiveIndexCount,
            memoryEstimate: memEstimate
        )

        cache[meshId] = entry
        totalMemoryEstimate += memEstimate
    }

    /// Update vertex positions and normals for an existing mesh (e.g., skinned mesh streaming).
    ///
    /// - Parameters:
    ///   - meshId: Mesh to update.
    ///   - positions: New vertex positions (3 floats per vertex).
    ///   - normals: New vertex normals (3 floats per vertex, may be nil to keep existing).
    ///   - vertexCount: Number of vertices.
    public func updateVertices(
        meshId: UInt64,
        positions: UnsafePointer<Float>,
        normals: UnsafePointer<Float>?,
        vertexCount: UInt32
    ) {
        guard let entry = cache[meshId] else { return }
        let vCount = min(Int(vertexCount), entry.vertexCount)

        entry.lowLevelMesh.withUnsafeMutableBytes(bufferIndex: 0) { rawBuffer in
            let vertexBuffer = rawBuffer.bindMemory(to: GodotVertex.self)
            for i in 0..<vCount {
                vertexBuffer[i].position = SIMD3<Float>(
                    positions[i * 3],
                    positions[i * 3 + 1],
                    positions[i * 3 + 2]
                )

                if let normals = normals {
                    vertexBuffer[i].normal = SIMD3<Float>(
                        normals[i * 3],
                        normals[i * 3 + 1],
                        normals[i * 3 + 2]
                    )
                }
            }
        }

        // Recompute bounds.
        var minBound = SIMD3<Float>(Float.greatestFiniteMagnitude,
                                     Float.greatestFiniteMagnitude,
                                     Float.greatestFiniteMagnitude)
        var maxBound = SIMD3<Float>(-Float.greatestFiniteMagnitude,
                                     -Float.greatestFiniteMagnitude,
                                     -Float.greatestFiniteMagnitude)
        for i in 0..<vCount {
            let pos = SIMD3<Float>(
                positions[i * 3],
                positions[i * 3 + 1],
                positions[i * 3 + 2]
            )
            minBound = min(minBound, pos)
            maxBound = max(maxBound, pos)
        }

        let bounds = BoundingBox(min: minBound, max: maxBound)
        let part = LowLevelMesh.Part(
            indexCount: entry.indexCount,
            topology: .triangle,
            bounds: bounds
        )
        entry.lowLevelMesh.parts.replaceAll([part])
    }

    /// Remove a mesh from the cache and free its resources.
    public func remove(meshId: UInt64) {
        if let entry = cache.removeValue(forKey: meshId) {
            totalMemoryEstimate -= entry.memoryEstimate
        }
    }

    /// Current total memory estimate for all cached meshes.
    public var memoryEstimate: UInt64 {
        totalMemoryEstimate
    }

    /// Number of cached meshes.
    public var count: Int {
        cache.count
    }
}
