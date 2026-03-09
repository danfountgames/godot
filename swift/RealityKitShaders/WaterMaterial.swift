/**************************************************************************/
/*  WaterMaterial.swift                                                   */
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

// MARK: - Water Material Parameters

/// Parameters for a water surface replacement material.
///
/// Parsed from JSON when the Godot shader annotation `@rk_shader("WaterMaterial")`
/// is used. Creates a water-like appearance using RealityKit's PBR with
/// partial transparency, low roughness, and color-tinted reflections.
///
/// Note: Actual wave vertex displacement and caustics are NOT possible through
/// PBR parameters alone. This material provides the surface appearance (color,
/// reflections, transparency) but NOT animated waves. Vertex animation must be
/// handled on the Godot side via CPU-driven mesh updates if needed.
struct WaterMaterialParams: Codable, Sendable {
    /// Water surface color (linear RGBA).
    var color: [Float] = [0.05, 0.3, 0.5, 1.0]

    /// Deep water color (used as emission tint for depth).
    var deepColor: [Float] = [0.02, 0.08, 0.2]

    /// Surface roughness. 0 = perfectly calm (mirror-like), 1 = choppy.
    var roughness: Float = 0.05

    /// Water opacity. 0 = fully transparent, 1 = opaque.
    var opacity: Float = 0.65

    /// Metallic value. Higher gives stronger surface reflections.
    var metallic: Float = 0.1

    /// Foam color (edge highlights via emission).
    var foamColor: [Float] = [0.9, 0.95, 1.0]

    /// Foam intensity (0 = no foam).
    var foamIntensity: Float = 0.0

    /// Normal map texture ID for surface detail (0 = none).
    var normalTexture: UInt64 = 0

    /// Normal map strength.
    var normalScale: Float = 1.0

    enum CodingKeys: String, CodingKey {
        case color, roughness, opacity, metallic
        case deepColor = "deep_color"
        case foamColor = "foam_color"
        case foamIntensity = "foam_intensity"
        case normalTexture = "normal_texture"
        case normalScale = "normal_scale"
    }
}

// MARK: - Water Material Factory

/// Creates a water surface material using RealityKit's `PhysicallyBasedMaterial`.
///
/// The water look is achieved through:
/// - Blue-tinted base color with partial transparency
/// - Low roughness for reflective surface
/// - Emission for depth color tinting
/// - Optional normal map for surface detail
/// - Clearcoat for surface specular highlights
///
/// This is a Tier 3 replacement shader per RK_PLAN.md §2.13.
public enum WaterMaterial {

    public static func create(
        paramsJson: String?,
        textureStore: TextureStore
    ) async throws -> RealityKit.Material {
        let params: WaterMaterialParams

        if let json = paramsJson, let data = json.data(using: .utf8) {
            params = try JSONDecoder().decode(WaterMaterialParams.self, from: data)
        } else {
            params = WaterMaterialParams()
        }

        var pbr = PhysicallyBasedMaterial()

        // -- Water Surface Color --
        let r = CGFloat(params.color.count > 0 ? params.color[0] : 0.05)
        let g = CGFloat(params.color.count > 1 ? params.color[1] : 0.3)
        let b = CGFloat(params.color.count > 2 ? params.color[2] : 0.5)

        pbr.baseColor = .init(tint: .init(red: r, green: g, blue: b, alpha: 1.0))

        // -- Surface Properties --
        pbr.roughness = .init(floatLiteral: params.roughness)
        pbr.metallic = .init(floatLiteral: params.metallic)

        // -- Depth Color via Emission --
        // A subtle emission in the deep color adds visual depth.
        let dR = CGFloat(params.deepColor.count > 0 ? params.deepColor[0] : 0.02)
        let dG = CGFloat(params.deepColor.count > 1 ? params.deepColor[1] : 0.08)
        let dB = CGFloat(params.deepColor.count > 2 ? params.deepColor[2] : 0.2)
        pbr.emissiveColor = .init(
            color: .init(red: dR, green: dG, blue: dB, alpha: 1.0)
        )
        pbr.emissiveIntensity = 0.3

        // -- Foam via Emission Boost --
        if params.foamIntensity > 0 {
            let fR = CGFloat(params.foamColor.count > 0 ? params.foamColor[0] : 0.9)
            let fG = CGFloat(params.foamColor.count > 1 ? params.foamColor[1] : 0.95)
            let fB = CGFloat(params.foamColor.count > 2 ? params.foamColor[2] : 1.0)
            // Blend foam color into emission.
            let blend = CGFloat(params.foamIntensity)
            pbr.emissiveColor = .init(
                color: .init(
                    red: dR * (1 - blend) + fR * blend,
                    green: dG * (1 - blend) + fG * blend,
                    blue: dB * (1 - blend) + fB * blend,
                    alpha: 1.0
                )
            )
            pbr.emissiveIntensity = 0.3 + params.foamIntensity * 0.5
        }

        // -- Normal Map --
        if params.normalTexture != 0,
           let normalTex = await textureStore.get(textureId: params.normalTexture) {
            pbr.normal = .init(texture: .init(normalTex), scale: params.normalScale)
        }

        // -- Surface Reflections via Clearcoat --
        // Adds a sharp reflection layer typical of water surfaces.
        pbr.clearcoat = .init(floatLiteral: 0.8)
        pbr.clearcoatRoughness = .init(floatLiteral: params.roughness * 0.3)

        // -- Transparency --
        pbr.blending = .transparent(opacity: .init(floatLiteral: params.opacity))

        // Water surface is typically single-sided (viewed from above).
        pbr.faceCulling = .back

        return pbr
    }

    @MainActor
    public static func register(with materialStore: MaterialStore) async {
        await materialStore.registerShader(name: "WaterMaterial", factory: create)
    }
}
