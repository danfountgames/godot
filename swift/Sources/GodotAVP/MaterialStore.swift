/**************************************************************************/
/*  MaterialStore.swift                                                   */
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

// MARK: - Alpha Mode

/// Maps bridge alpha mode values to RealityKit blending behavior.
enum GodotAlphaMode: Int32 {
    case opaque = 0
    case transparent = 1
    case alphaScissor = 2
}

// MARK: - Cull Mode

/// Maps bridge cull mode values to RealityKit face culling.
enum GodotCullMode: Int32 {
    case back = 0
    case front = 1
    case disabled = 2
}

// MARK: - MaterialCacheEntry

/// A cached RealityKit material.
struct MaterialCacheEntry {
    let material: RealityKit.Material
    var memoryEstimate: UInt64
}

// MARK: - Custom Shader Registry

/// Type alias for shader factory functions.
/// Receives JSON parameters and the texture store for texture lookups.
public typealias ShaderFactory = @Sendable (String?, TextureStore) async throws -> RealityKit.Material

// MARK: - MaterialStore

/// Actor-based store for RealityKit materials.
///
/// Maps Godot material RIDs (UInt64) to RealityKit `Material` instances.
/// Supports three material creation paths:
///   1. PBR — builds `PhysicallyBasedMaterial` from Godot StandardMaterial3D properties.
///   2. Unlit — builds `UnlitMaterial` for flat-shaded content.
///   3. Custom — loads registered `ShaderGraphMaterial` by name.
///
/// Thread-safe via Swift actor isolation.
public actor MaterialStore {

    // MARK: - Properties

    /// Cache of material ID -> MaterialCacheEntry.
    private var cache: [UInt64: MaterialCacheEntry] = [:]

    /// Total estimated memory for all cached materials.
    private var totalMemoryEstimate: UInt64 = 0

    /// Reference to the TextureStore for texture lookups.
    private let textureStore: TextureStore

    /// Registry of custom shader factories keyed by shader name.
    private var shaderFactories: [String: ShaderFactory] = [:]

    // MARK: - Initialization

    public init(textureStore: TextureStore) {
        self.textureStore = textureStore
    }

    // MARK: - Public API

    /// Retrieve a cached material, or nil if not found.
    public func get(materialId: UInt64) -> RealityKit.Material? {
        cache[materialId]?.material
    }

    /// Register a custom shader factory by name.
    ///
    /// Custom shaders are created when `godot_avp_material_create_custom` is called
    /// with the corresponding `shader_name`.
    public func registerShader(name: String, factory: @escaping ShaderFactory) {
        shaderFactories[name] = factory
    }

    // MARK: - PBR Material Creation

    /// Create a physically-based material from Godot StandardMaterial3D properties.
    ///
    /// This maps the bridge PBR descriptor to RealityKit's `PhysicallyBasedMaterial`.
    /// See RK_PLAN.md section 2.13 for the property mapping table.
    public func createPBR(
        materialId: UInt64,
        albedoTex: UInt64,
        albedoR: Float, albedoG: Float, albedoB: Float, albedoA: Float,
        roughness: Float, roughnessTex: UInt64,
        metallic: Float, metallicTex: UInt64,
        normalTex: UInt64, normalScale: Float,
        emissionTex: UInt64,
        emissionR: Float, emissionG: Float, emissionB: Float,
        emissionEnergy: Float,
        aoTex: UInt64,
        clearcoat: Float, clearcoatRoughness: Float,
        alphaMode: Int32, alphaScissorThreshold: Float,
        cullMode: Int32
    ) async {
        // Remove existing material if present.
        if cache[materialId] != nil {
            remove(materialId: materialId)
        }

        var pbr = PhysicallyBasedMaterial()

        // --- Albedo ---
        let albedoColor = PhysicallyBasedMaterial.BaseColor(
            tint: .init(
                red: CGFloat(albedoR),
                green: CGFloat(albedoG),
                blue: CGFloat(albedoB),
                alpha: CGFloat(albedoA)
            ),
            texture: await resolveTexture(albedoTex)
        )
        pbr.baseColor = albedoColor

        // --- Roughness ---
        if let roughnessTexResource = await resolveTexture(roughnessTex) {
            pbr.roughness = .init(floatLiteral: roughness)
            pbr.roughness.texture = .init(roughnessTexResource)
        } else {
            pbr.roughness = .init(floatLiteral: roughness)
        }

        // --- Metallic ---
        if let metallicTexResource = await resolveTexture(metallicTex) {
            pbr.metallic = .init(floatLiteral: metallic)
            pbr.metallic.texture = .init(metallicTexResource)
        } else {
            pbr.metallic = .init(floatLiteral: metallic)
        }

        // --- Normal Map ---
        if let normalTexResource = await resolveTexture(normalTex) {
            pbr.normal = .init(texture: .init(normalTexResource), scale: normalScale)
        }

        // --- Emission ---
        let hasEmission = emissionR > 0 || emissionG > 0 || emissionB > 0 || emissionTex != 0
        if hasEmission {
            pbr.emissiveColor = .init(
                color: .init(
                    red: CGFloat(emissionR * emissionEnergy),
                    green: CGFloat(emissionG * emissionEnergy),
                    blue: CGFloat(emissionB * emissionEnergy),
                    alpha: 1.0
                ),
                texture: await resolveTexture(emissionTex)
            )
            pbr.emissiveIntensity = emissionEnergy
        }

        // --- Ambient Occlusion ---
        if let aoTexResource = await resolveTexture(aoTex) {
            pbr.ambientOcclusion = .init(texture: .init(aoTexResource))
        }

        // --- Clearcoat ---
        if clearcoat > 0 {
            pbr.clearcoat = .init(floatLiteral: clearcoat)
            pbr.clearcoatRoughness = .init(floatLiteral: clearcoatRoughness)
        }

        // --- Alpha Mode ---
        let alpha = GodotAlphaMode(rawValue: alphaMode) ?? .opaque
        switch alpha {
        case .opaque:
            pbr.blending = .opaque
        case .transparent:
            pbr.blending = .transparent(opacity: .init(floatLiteral: albedoA))
        case .alphaScissor:
            pbr.blending = .transparent(opacity: .init(floatLiteral: albedoA))
            pbr.opacityThreshold = alphaScissorThreshold
        }

        // --- Cull Mode ---
        let cull = GodotCullMode(rawValue: cullMode) ?? .back
        switch cull {
        case .back:
            pbr.faceCulling = .back
        case .front:
            pbr.faceCulling = .front
        case .disabled:
            pbr.faceCulling = .none
        }

        let entry = MaterialCacheEntry(
            material: pbr,
            memoryEstimate: 256 // Base material overhead estimate.
        )
        cache[materialId] = entry
        totalMemoryEstimate += entry.memoryEstimate
    }

    // MARK: - Unlit Material Creation

    /// Create an unlit material.
    public func createUnlit(
        materialId: UInt64,
        albedoTex: UInt64,
        albedoR: Float, albedoG: Float, albedoB: Float, albedoA: Float,
        alphaMode: Int32, alphaScissorThreshold: Float,
        cullMode: Int32
    ) async {
        // Remove existing material if present.
        if cache[materialId] != nil {
            remove(materialId: materialId)
        }

        var unlit = UnlitMaterial()

        // --- Color ---
        let tintColor: UnlitMaterial.BaseColor
        if let texResource = await resolveTexture(albedoTex) {
            tintColor = .init(
                tint: .init(
                    red: CGFloat(albedoR),
                    green: CGFloat(albedoG),
                    blue: CGFloat(albedoB),
                    alpha: CGFloat(albedoA)
                ),
                texture: .init(texResource)
            )
        } else {
            tintColor = .init(
                tint: .init(
                    red: CGFloat(albedoR),
                    green: CGFloat(albedoG),
                    blue: CGFloat(albedoB),
                    alpha: CGFloat(albedoA)
                )
            )
        }
        unlit.color = tintColor

        // --- Alpha Mode ---
        let alpha = GodotAlphaMode(rawValue: alphaMode) ?? .opaque
        switch alpha {
        case .opaque:
            unlit.blending = .opaque
        case .transparent:
            unlit.blending = .transparent(opacity: .init(floatLiteral: albedoA))
        case .alphaScissor:
            unlit.blending = .transparent(opacity: .init(floatLiteral: albedoA))
            unlit.opacityThreshold = alphaScissorThreshold
        }

        // --- Cull Mode ---
        let cull = GodotCullMode(rawValue: cullMode) ?? .back
        switch cull {
        case .back:
            unlit.faceCulling = .back
        case .front:
            unlit.faceCulling = .front
        case .disabled:
            unlit.faceCulling = .none
        }

        let entry = MaterialCacheEntry(
            material: unlit,
            memoryEstimate: 128
        )
        cache[materialId] = entry
        totalMemoryEstimate += entry.memoryEstimate
    }

    // MARK: - Custom Material Creation

    /// Create a material using a registered replacement shader.
    ///
    /// Looks up the shader factory by name and calls it with the JSON parameters.
    /// If no factory is registered for the name, falls back to a default PBR material.
    public func createCustom(
        materialId: UInt64,
        shaderName: String,
        paramsJson: String?
    ) async {
        // Remove existing material if present.
        if cache[materialId] != nil {
            remove(materialId: materialId)
        }

        let material: RealityKit.Material
        if let factory = shaderFactories[shaderName] {
            do {
                material = try await factory(paramsJson, textureStore)
            } catch {
                print("[GodotAVP] MaterialStore: custom shader '\(shaderName)' failed: \(error). Falling back to default.")
                material = SimpleMaterial(color: .magenta, isMetallic: false)
            }
        } else {
            print("[GodotAVP] MaterialStore: no shader factory registered for '\(shaderName)'. Using fallback.")
            material = SimpleMaterial(color: .magenta, isMetallic: false)
        }

        let entry = MaterialCacheEntry(
            material: material,
            memoryEstimate: 256
        )
        cache[materialId] = entry
        totalMemoryEstimate += entry.memoryEstimate
    }

    // MARK: - Cleanup

    /// Remove a material from the cache.
    public func remove(materialId: UInt64) {
        if let entry = cache.removeValue(forKey: materialId) {
            totalMemoryEstimate -= entry.memoryEstimate
        }
    }

    /// Current total memory estimate for all cached materials.
    public var memoryEstimate: UInt64 {
        totalMemoryEstimate
    }

    /// Number of cached materials.
    public var count: Int {
        cache.count
    }

    // MARK: - Private Helpers

    /// Resolve a texture ID to a MaterialParameters.Texture, or nil if ID is 0 or not found.
    private func resolveTexture(_ textureId: UInt64) async -> MaterialParameters.Texture? {
        guard textureId != 0 else { return nil }
        guard let texResource = await textureStore.get(textureId: textureId) else { return nil }
        return .init(texResource)
    }
}
