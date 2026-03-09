/**************************************************************************/
/*  ToonMaterial.swift                                                    */
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
import GodotAVP

// MARK: - Toon Material Parameters

/// Parameters for the toon/cel-shading replacement material.
///
/// Parsed from JSON when the Godot shader annotation `@rk_shader("ToonMaterial")`
/// is used. These map common toon shader parameters to a RealityKit
/// `PhysicallyBasedMaterial` approximation.
struct ToonMaterialParams: Codable, Sendable {
    /// Base albedo color (linear RGB).
    var color: [Float] = [1.0, 1.0, 1.0, 1.0]

    /// Number of discrete shading steps (2-5 typical).
    var shadingSteps: Int = 3

    /// Shadow color tint.
    var shadowColor: [Float] = [0.3, 0.3, 0.4]

    /// Outline width (0 = no outline).
    var outlineWidth: Float = 0.0

    /// Outline color.
    var outlineColor: [Float] = [0.0, 0.0, 0.0, 1.0]

    /// Specular highlight size.
    var specularSize: Float = 0.3

    /// Whether to use hard specular (cel-shaded highlight).
    var hardSpecular: Bool = true

    /// Albedo texture ID (0 = none).
    var albedoTexture: UInt64 = 0

    enum CodingKeys: String, CodingKey {
        case color
        case shadingSteps = "shading_steps"
        case shadowColor = "shadow_color"
        case outlineWidth = "outline_width"
        case outlineColor = "outline_color"
        case specularSize = "specular_size"
        case hardSpecular = "hard_specular"
        case albedoTexture = "albedo_texture"
    }
}

// MARK: - Toon Material Factory

/// Creates a toon/cel-shading material approximation using RealityKit's
/// `PhysicallyBasedMaterial`.
///
/// Since RealityKit does not expose a programmable surface shader pipeline
/// (ShaderGraphMaterial requires Reality Composer Pro .usda assets), this
/// approximation uses PBR material parameters tuned for a toon look:
///
/// - High roughness (flat shading appearance)
/// - Low metallic (diffuse-dominant)
/// - Emission used for shadow color tinting
/// - Clearcoat for specular highlights
///
/// This is a Tier 3 replacement shader per RK_PLAN.md section 2.13 — a
/// best-effort approximation, not a pixel-accurate reproduction of the
/// Godot toon shader.
///
/// Register with:
/// ```swift
/// materialStore.registerShader(name: "ToonMaterial", factory: ToonMaterial.create)
/// ```
public enum ToonMaterial {

    /// Factory function matching the `ShaderFactory` type alias.
    ///
    /// - Parameters:
    ///   - paramsJson: JSON-encoded `ToonMaterialParams`, or nil for defaults.
    ///   - textureStore: Texture store for resolving texture IDs.
    /// - Returns: A configured RealityKit material.
    public static func create(
        paramsJson: String?,
        textureStore: TextureStore
    ) async throws -> RealityKit.Material {
        let params: ToonMaterialParams

        if let json = paramsJson, let data = json.data(using: .utf8) {
            params = try JSONDecoder().decode(ToonMaterialParams.self, from: data)
        } else {
            params = ToonMaterialParams()
        }

        var pbr = PhysicallyBasedMaterial()

        // -- Base Color --
        let r = CGFloat(params.color.count > 0 ? params.color[0] : 1.0)
        let g = CGFloat(params.color.count > 1 ? params.color[1] : 1.0)
        let b = CGFloat(params.color.count > 2 ? params.color[2] : 1.0)
        let a = CGFloat(params.color.count > 3 ? params.color[3] : 1.0)

        if params.albedoTexture != 0,
           let texResource = await textureStore.get(textureId: params.albedoTexture) {
            pbr.baseColor = .init(
                tint: .init(red: r, green: g, blue: b, alpha: a),
                texture: .init(texResource)
            )
        } else {
            pbr.baseColor = .init(
                tint: .init(red: r, green: g, blue: b, alpha: a)
            )
        }

        // -- Toon Approximation via PBR --
        // High roughness gives a flat, non-reflective look.
        pbr.roughness = .init(floatLiteral: 0.95)

        // Low metallic for diffuse-dominant shading.
        pbr.metallic = .init(floatLiteral: 0.0)

        // -- Specular via Clearcoat --
        // Clearcoat provides a secondary specular lobe that can approximate
        // a toon specular highlight when combined with high roughness base.
        if params.hardSpecular {
            // Small clearcoat for a controlled highlight.
            pbr.clearcoat = .init(floatLiteral: 0.5)
            pbr.clearcoatRoughness = .init(floatLiteral: 1.0 - params.specularSize)
        } else {
            pbr.clearcoat = .init(floatLiteral: params.specularSize)
            pbr.clearcoatRoughness = .init(floatLiteral: 0.3)
        }

        // -- Shadow Color Hint via Emission --
        // A subtle emission in the shadow color direction can help tint
        // shadowed areas, though this is a rough approximation.
        let shadowR = CGFloat(params.shadowColor.count > 0 ? params.shadowColor[0] * 0.1 : 0)
        let shadowG = CGFloat(params.shadowColor.count > 1 ? params.shadowColor[1] * 0.1 : 0)
        let shadowB = CGFloat(params.shadowColor.count > 2 ? params.shadowColor[2] * 0.1 : 0)
        if shadowR > 0 || shadowG > 0 || shadowB > 0 {
            pbr.emissiveColor = .init(
                color: .init(red: shadowR, green: shadowG, blue: shadowB, alpha: 1.0)
            )
            pbr.emissiveIntensity = 0.15
        }

        // -- Alpha --
        if a < 1.0 {
            pbr.blending = .transparent(opacity: .init(floatLiteral: Float(a)))
        } else {
            pbr.blending = .opaque
        }

        // Double-sided rendering is common for toon materials.
        pbr.faceCulling = .none

        return pbr
    }

    /// Register this shader factory with a `MaterialStore`.
    @MainActor
    public static func register(with materialStore: MaterialStore) async {
        await materialStore.registerShader(name: "ToonMaterial", factory: create)
    }
}
