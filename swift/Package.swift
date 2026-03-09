// swift-tools-version: 5.9
/**************************************************************************/
/*  Package.swift                                                         */
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

import PackageDescription

let package = Package(
    name: "GodotAVP",
    platforms: [
        .visionOS(.v2)
    ],
    products: [
        .library(
            name: "GodotAVP",
            targets: ["GodotAVP"]
        ),
        .library(
            name: "GodotAVPApp",
            targets: ["GodotAVPApp"]
        ),
    ],
    targets: [
        // C bridge header — system library wrapping the Godot avp_bridge.h header.
        .systemLibrary(
            name: "CGodotAVPBridge",
            path: "Sources/CGodotAVPBridge"
        ),

        // Core bridge library — entity synchronization, mesh/texture/material stores.
        .target(
            name: "GodotAVP",
            dependencies: ["CGodotAVPBridge"],
            path: "Sources/GodotAVP",
            swiftSettings: [
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),

        // SwiftUI application layer — RealityView integration, app entry point.
        .target(
            name: "GodotAVPApp",
            dependencies: ["GodotAVP"],
            path: "Sources/GodotAVPApp",
            swiftSettings: [
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),

        // Replacement shaders using ShaderGraphMaterial / programmatic materials.
        .target(
            name: "RealityKitShaders",
            dependencies: ["GodotAVP"],
            path: "RealityKitShaders",
            swiftSettings: [
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),
    ]
)
