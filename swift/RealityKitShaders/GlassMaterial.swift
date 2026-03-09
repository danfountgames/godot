/**************************************************************************/
/*  GlassMaterial.swift                                                   */
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

// MARK: - Glass Material Parameters

/// Parameters for a glass/transparent replacement material.
///
/// Parsed from JSON when the Godot shader annotation `@rk_shader("GlassMaterial")`
/// is used. Creates a glass-like appearance using RealityKit's PBR with
/// transparency, low roughness, and optional tinting.
struct GlassMaterialParams: Codable, Sendable {
    /// Tint color (linear RGBA). Alpha controls base opacity.
    var color: [Float] = [0.95, 0.97, 1.0, 0.15]

    /// Surface roughness. 0 = perfectly smooth (mirror-like), 1 = frosted glass.
    var roughness: Float = 0.0

    /// Index of refraction influence. Higher = more reflective at grazing angles.
    /// Maps to metallic in PBR approximation (0.0 = dielectric glass).
    var ior: Float = 1.5

    /// Opacity multiplier. Combined with color alpha.
    var opacity: Float = 0.2

    /// Whether the glass has a colored tint (colored glass).
    var tinted: Bool = false

    /// Tint absorption color for colored glass.
    var tintColor: [Float] = [0.8, 0.9, 1.0]

    enum CodingKeys: String, CodingKey {
        case color, roughness, ior, opacity, tinted
        case tintColor = "tint_color"
    }
}

// MARK: - Glass Material Factory

/// Creates a glass material using RealityKit's `PhysicallyBasedMaterial`
/// with transparency.
///
/// RealityKit's PBR pipeline handles glass well: low roughness + transparency
/// gives realistic reflections and see-through appearance. On visionOS, this
/// integrates naturally with passthrough compositing.
///
/// This is a Tier 3 replacement shader per RK_PLAN.md §2.13.
public enum GlassMaterial {

    public static func create(
        paramsJson: String?,
        textureStore: TextureStore
    ) async throws -> RealityKit.Material {
        let params: GlassMaterialParams

        if let json = paramsJson, let data = json.data(using: .utf8) {
            params = try JSONDecoder().decode(GlassMaterialParams.self, from: data)
        } else {
            params = GlassMaterialParams()
        }

        var pbr = PhysicallyBasedMaterial()

        // -- Base Color / Tint --
        let r = CGFloat(params.color.count > 0 ? params.color[0] : 0.95)
        let g = CGFloat(params.color.count > 1 ? params.color[1] : 0.97)
        let b = CGFloat(params.color.count > 2 ? params.color[2] : 1.0)

        pbr.baseColor = .init(tint: .init(red: r, green: g, blue: b, alpha: 1.0))

        // -- Glass Surface --
        pbr.roughness = .init(floatLiteral: params.roughness)

        // Glass is dielectric — low metallic. A slight metallic value
        // increases Fresnel reflections at grazing angles.
        let metallicFromIOR = max(0.0, min(0.1, (params.ior - 1.0) * 0.05))
        pbr.metallic = .init(floatLiteral: metallicFromIOR)

        // -- Transparency --
        let finalOpacity = params.opacity * (params.color.count > 3 ? params.color[3] : 0.15)
        pbr.blending = .transparent(opacity: .init(floatLiteral: finalOpacity))

        // -- Clearcoat for Surface Reflection --
        // Adds a sharp surface reflection layer typical of glass.
        pbr.clearcoat = .init(floatLiteral: 1.0)
        pbr.clearcoatRoughness = .init(floatLiteral: params.roughness * 0.5)

        // -- Tinted Glass Emission --
        if params.tinted {
            let tR = CGFloat(params.tintColor.count > 0 ? params.tintColor[0] * 0.05 : 0)
            let tG = CGFloat(params.tintColor.count > 1 ? params.tintColor[1] * 0.05 : 0)
            let tB = CGFloat(params.tintColor.count > 2 ? params.tintColor[2] * 0.05 : 0)
            pbr.emissiveColor = .init(
                color: .init(red: tR, green: tG, blue: tB, alpha: 1.0)
            )
            pbr.emissiveIntensity = 0.1
        }

        // Glass is visible from both sides.
        pbr.faceCulling = .none

        return pbr
    }

    @MainActor
    public static func register(with materialStore: MaterialStore) async {
        await materialStore.registerShader(name: "GlassMaterial", factory: create)
    }
}
