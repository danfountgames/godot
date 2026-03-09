/**************************************************************************/
/*  HologramMaterial.swift                                                */
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

// MARK: - Hologram Material Parameters

/// Parameters for a holographic/sci-fi replacement material.
///
/// Parsed from JSON when the Godot shader annotation `@rk_shader("HologramMaterial")`
/// is used. Creates a holographic look using emission, transparency, and
/// optional scanline effects approximated through PBR parameters.
struct HologramMaterialParams: Codable, Sendable {
    /// Primary hologram color (linear RGB).
    var color: [Float] = [0.0, 0.7, 1.0, 1.0]

    /// Emission intensity. Higher = brighter hologram glow.
    var intensity: Float = 2.0

    /// Base opacity of the hologram. Lower = more transparent/ghostly.
    var opacity: Float = 0.35

    /// Rim glow intensity. Enhances edges for a holographic fringe effect.
    /// Approximated via clearcoat in PBR.
    var rimIntensity: Float = 0.8

    /// Scanline density (visual hint only — actual scanlines not possible
    /// without custom shaders). 0 = no scanline influence.
    var scanlineDensity: Float = 0.0

    /// Whether the hologram flickers (future: time-based opacity modulation).
    var flicker: Bool = false

    enum CodingKeys: String, CodingKey {
        case color, intensity, opacity
        case rimIntensity = "rim_intensity"
        case scanlineDensity = "scanline_density"
        case flicker
    }
}

// MARK: - Hologram Material Factory

/// Creates a holographic material using RealityKit's `PhysicallyBasedMaterial`
/// with strong emission and transparency.
///
/// The hologram look is achieved through:
/// - Strong emission in the hologram color (self-illuminated)
/// - High transparency (see-through)
/// - Clearcoat for rim/edge glow approximation
/// - Zero metallic, high roughness (emission-dominant, not reflective)
///
/// This is a Tier 3 replacement shader per RK_PLAN.md §2.13.
public enum HologramMaterial {

    public static func create(
        paramsJson: String?,
        textureStore: TextureStore
    ) async throws -> RealityKit.Material {
        let params: HologramMaterialParams

        if let json = paramsJson, let data = json.data(using: .utf8) {
            params = try JSONDecoder().decode(HologramMaterialParams.self, from: data)
        } else {
            params = HologramMaterialParams()
        }

        var pbr = PhysicallyBasedMaterial()

        // -- Base Color (dark, emission does the work) --
        pbr.baseColor = .init(
            tint: .init(red: 0.01, green: 0.01, blue: 0.02, alpha: 1.0)
        )

        // -- Emission is the core of the hologram look --
        let r = CGFloat(params.color.count > 0 ? params.color[0] : 0.0)
        let g = CGFloat(params.color.count > 1 ? params.color[1] : 0.7)
        let b = CGFloat(params.color.count > 2 ? params.color[2] : 1.0)
        pbr.emissiveColor = .init(
            color: .init(red: r, green: g, blue: b, alpha: 1.0)
        )
        pbr.emissiveIntensity = params.intensity

        // -- Non-reflective surface --
        pbr.roughness = .init(floatLiteral: 0.9)
        pbr.metallic = .init(floatLiteral: 0.0)

        // -- Rim Glow via Clearcoat --
        // Clearcoat adds a secondary specular highlight that can approximate
        // Fresnel rim glow when combined with low base reflections.
        if params.rimIntensity > 0 {
            pbr.clearcoat = .init(floatLiteral: params.rimIntensity)
            pbr.clearcoatRoughness = .init(floatLiteral: 0.1)
        }

        // -- Transparency --
        pbr.blending = .transparent(opacity: .init(floatLiteral: params.opacity))

        // Holograms are visible from all angles.
        pbr.faceCulling = .none

        return pbr
    }

    @MainActor
    public static func register(with materialStore: MaterialStore) async {
        await materialStore.registerShader(name: "HologramMaterial", factory: create)
    }
}
