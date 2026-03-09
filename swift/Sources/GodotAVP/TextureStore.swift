/**************************************************************************/
/*  TextureStore.swift                                                    */
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

// MARK: - TextureFormat

/// Maps the integer format codes from the C bridge to Metal pixel formats.
///
/// Bridge format values:
///   0 = RGBA8 Unorm
///   1 = RGBA8 sRGB
///   2 = RGBA16 Float
///   3 = R8 Unorm
///   4 = RG8 Unorm
///   5 = BC7 Unorm (compressed)
enum GodotTextureFormat: UInt32 {
    case rgba8Unorm = 0
    case rgba8Srgb = 1
    case rgba16Float = 2
    case r8Unorm = 3
    case rg8Unorm = 4
    case bc7Unorm = 5

    /// Convert to the corresponding `MTLPixelFormat`.
    var metalPixelFormat: MTLPixelFormat {
        switch self {
        case .rgba8Unorm: return .rgba8Unorm
        case .rgba8Srgb: return .rgba8Unorm_srgb
        case .rgba16Float: return .rgba16Float
        case .r8Unorm: return .r8Unorm
        case .rg8Unorm: return .rg8Unorm
        case .bc7Unorm: return .bc7_rgbaUnorm
        }
    }

    /// Convert to the corresponding `LowLevelTexture.PixelFormat`.
    var lowLevelFormat: LowLevelTexture.PixelFormat {
        switch self {
        case .rgba8Unorm: return .rgba8Unorm
        case .rgba8Srgb: return .rgba8Unorm_srgb
        case .rgba16Float: return .rgba16Float
        case .r8Unorm: return .r8Unorm
        case .rg8Unorm: return .rg8Unorm
        case .bc7Unorm: return .bc7_rgbaUnorm
        }
    }

    /// Bytes per pixel for uncompressed formats.
    var bytesPerPixel: Int {
        switch self {
        case .rgba8Unorm, .rgba8Srgb: return 4
        case .rgba16Float: return 8
        case .r8Unorm: return 1
        case .rg8Unorm: return 2
        case .bc7Unorm: return 1 // compressed; actual size depends on block count
        }
    }

    /// Whether this is a block-compressed format.
    var isCompressed: Bool {
        self == .bc7Unorm
    }
}

// MARK: - TextureCacheEntry

/// A cached texture resource along with its LowLevelTexture for GPU-side updates.
struct TextureCacheEntry {
    let textureResource: TextureResource
    let lowLevelTexture: LowLevelTexture
    let width: Int
    let height: Int
    let format: GodotTextureFormat
    let mipmapCount: Int
    var memoryEstimate: UInt64
}

// MARK: - TextureStore

/// Actor-based store for `LowLevelTexture` / `TextureResource` objects.
///
/// Maps Godot texture RIDs (UInt64) to RealityKit `TextureResource` instances.
/// Creates textures via `LowLevelTexture` with format mapping from bridge format
/// codes to Metal pixel formats.
///
/// Thread-safe via Swift actor isolation.
public actor TextureStore {

    // MARK: - Properties

    /// Cache of texture ID -> TextureCacheEntry.
    private var cache: [UInt64: TextureCacheEntry] = [:]

    /// Total estimated GPU memory for all cached textures.
    private var totalMemoryEstimate: UInt64 = 0

    /// Shared Metal device for GPU operations.
    private let metalDevice: MTLDevice?

    /// Command queue for GPU-side texture updates.
    private let commandQueue: MTLCommandQueue?

    // MARK: - Initialization

    public init() {
        self.metalDevice = MTLCreateSystemDefaultDevice()
        self.commandQueue = metalDevice?.makeCommandQueue()
    }

    // MARK: - Public API

    /// Retrieve a cached texture resource, or nil if not found.
    public func get(textureId: UInt64) -> TextureResource? {
        cache[textureId]?.textureResource
    }

    /// Create a texture from raw pixel data streamed from the C bridge.
    ///
    /// - Parameters:
    ///   - textureId: Unique texture identifier from Godot RID.
    ///   - pixels: Raw pixel data.
    ///   - width: Texture width in pixels.
    ///   - height: Texture height in pixels.
    ///   - format: Bridge format code (0-5, see GodotTextureFormat).
    ///   - mipmaps: Number of mipmap levels (1 = no mipmaps).
    ///   - isSrgb: Whether the texture uses sRGB color space.
    public func create(
        textureId: UInt64,
        pixels: UnsafePointer<UInt8>,
        width: UInt32,
        height: UInt32,
        format: UInt32,
        mipmaps: UInt32,
        isSrgb: Bool
    ) throws {
        // Remove existing texture if present.
        if cache[textureId] != nil {
            remove(textureId: textureId)
        }

        let w = Int(width)
        let h = Int(height)
        let mipCount = max(Int(mipmaps), 1)

        guard let texFormat = GodotTextureFormat(rawValue: format) else {
            throw TextureStoreError.unsupportedFormat(format)
        }

        // Override format to sRGB variant if requested.
        let effectiveFormat: GodotTextureFormat
        if isSrgb && texFormat == .rgba8Unorm {
            effectiveFormat = .rgba8Srgb
        } else {
            effectiveFormat = texFormat
        }

        // Create the LowLevelTexture.
        let descriptor = LowLevelTexture.Descriptor(
            textureType: .type2D,
            pixelFormat: effectiveFormat.lowLevelFormat,
            width: w,
            height: h,
            depth: 1,
            mipmapLevelCount: mipCount,
            textureUsage: [.shaderRead]
        )

        let lowLevelTexture = try LowLevelTexture(descriptor: descriptor)

        // Upload pixel data to the texture.
        // Calculate total data size for the base mip level.
        let bytesPerRow: Int
        let totalBytes: Int

        if effectiveFormat.isCompressed {
            // BC7: 4x4 block compressed, 16 bytes per block.
            let blocksWide = (w + 3) / 4
            let blocksHigh = (h + 3) / 4
            bytesPerRow = blocksWide * 16
            totalBytes = bytesPerRow * blocksHigh
        } else {
            bytesPerRow = w * effectiveFormat.bytesPerPixel
            totalBytes = bytesPerRow * h
        }

        // Upload base mip level via replace.
        lowLevelTexture.replace(
            using: pixels,
            bytesPerRow: bytesPerRow,
            region: MTLRegion(
                origin: MTLOrigin(x: 0, y: 0, z: 0),
                size: MTLSize(width: w, height: h, depth: 1)
            ),
            mipmapLevel: 0
        )

        // Upload additional mip levels if provided.
        if mipCount > 1 {
            var offset = totalBytes
            var mipWidth = w / 2
            var mipHeight = h / 2
            for mip in 1..<mipCount {
                if mipWidth < 1 { mipWidth = 1 }
                if mipHeight < 1 { mipHeight = 1 }

                let mipBytesPerRow: Int
                let mipTotalBytes: Int

                if effectiveFormat.isCompressed {
                    let blocksWide = (mipWidth + 3) / 4
                    let blocksHigh = (mipHeight + 3) / 4
                    mipBytesPerRow = blocksWide * 16
                    mipTotalBytes = mipBytesPerRow * blocksHigh
                } else {
                    mipBytesPerRow = mipWidth * effectiveFormat.bytesPerPixel
                    mipTotalBytes = mipBytesPerRow * mipHeight
                }

                let mipPixels = pixels.advanced(by: offset)
                lowLevelTexture.replace(
                    using: mipPixels,
                    bytesPerRow: mipBytesPerRow,
                    region: MTLRegion(
                        origin: MTLOrigin(x: 0, y: 0, z: 0),
                        size: MTLSize(width: mipWidth, height: mipHeight, depth: 1)
                    ),
                    mipmapLevel: mip
                )

                offset += mipTotalBytes
                mipWidth /= 2
                mipHeight /= 2
            }
        }

        let textureResource = try TextureResource(from: lowLevelTexture)

        // Memory estimate — sum of all mip levels.
        let memEstimate = estimateMemory(
            width: w, height: h,
            format: effectiveFormat,
            mipmapCount: mipCount
        )

        let entry = TextureCacheEntry(
            textureResource: textureResource,
            lowLevelTexture: lowLevelTexture,
            width: w,
            height: h,
            format: effectiveFormat,
            mipmapCount: mipCount,
            memoryEstimate: memEstimate
        )

        cache[textureId] = entry
        totalMemoryEstimate += memEstimate
    }

    /// Remove a texture from the cache and free its resources.
    public func remove(textureId: UInt64) {
        if let entry = cache.removeValue(forKey: textureId) {
            totalMemoryEstimate -= entry.memoryEstimate
        }
    }

    /// Current total memory estimate for all cached textures.
    public var memoryEstimate: UInt64 {
        totalMemoryEstimate
    }

    /// Number of cached textures.
    public var count: Int {
        cache.count
    }

    // MARK: - Private Helpers

    /// Estimate GPU memory usage for a texture including all mip levels.
    private func estimateMemory(
        width: Int,
        height: Int,
        format: GodotTextureFormat,
        mipmapCount: Int
    ) -> UInt64 {
        var total: UInt64 = 0
        var w = width
        var h = height
        for _ in 0..<mipmapCount {
            if format.isCompressed {
                let blocksWide = (w + 3) / 4
                let blocksHigh = (h + 3) / 4
                total += UInt64(blocksWide * blocksHigh * 16)
            } else {
                total += UInt64(w * h * format.bytesPerPixel)
            }
            w = max(w / 2, 1)
            h = max(h / 2, 1)
        }
        return total
    }
}

// MARK: - Errors

enum TextureStoreError: Error, CustomStringConvertible {
    case unsupportedFormat(UInt32)
    case metalDeviceUnavailable

    var description: String {
        switch self {
        case .unsupportedFormat(let code):
            return "TextureStore: unsupported texture format code \(code)"
        case .metalDeviceUnavailable:
            return "TextureStore: Metal device unavailable"
        }
    }
}
