/**************************************************************************/
/*  render_godot_ai_logo.swift                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
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

// Rasterises the GodotAI logo SVGs. macOS renders SVG natively through NSImage since
// Big Sur, so this needs no third-party rasteriser - which is the reason the raster
// assets are produced here rather than by a Linux toolchain.
//
// Usage: render_godot_ai_logo.swift <source.svg> <width> <height> <output.png>
// The SVG is drawn centred and aspect-fit into the requested canvas, on transparency.

import AppKit
import Foundation

guard CommandLine.arguments.count == 5,
		let width = Int(CommandLine.arguments[2]), let height = Int(CommandLine.arguments[3]),
		width > 0, height > 0 else {
	FileHandle.standardError.write(Data("Usage: render_godot_ai_logo.swift <source.svg> <width> <height> <output.png>\n".utf8))
	exit(2)
}

guard let source = NSImage(contentsOfFile: CommandLine.arguments[1]) else {
	FileHandle.standardError.write(Data("Unable to load the source SVG.\n".utf8))
	exit(1)
}

let canvas = NSSize(width: width, height: height)
guard let bitmap = NSBitmapImageRep(
	bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
	bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
	colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
), let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
	FileHandle.standardError.write(Data("Unable to create the canvas.\n".utf8))
	exit(1)
}

bitmap.size = canvas
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
context.imageInterpolation = .high

// Aspect-fit: the icon fills a square canvas edge to edge; the wordmark centres in a
// wider one without distortion.
let source_size = source.size
let scale = min(canvas.width / source_size.width, canvas.height / source_size.height)
let drawn = NSSize(width: source_size.width * scale, height: source_size.height * scale)
let origin = NSPoint(x: (canvas.width - drawn.width) / 2, y: (canvas.height - drawn.height) / 2)
source.draw(in: NSRect(origin: origin, size: drawn))

NSGraphicsContext.restoreGraphicsState()

guard let png = bitmap.representation(using: .png, properties: [:]) else {
	FileHandle.standardError.write(Data("Unable to encode the PNG.\n".utf8))
	exit(1)
}
do {
	try png.write(to: URL(fileURLWithPath: CommandLine.arguments[4]), options: .atomic)
} catch {
	FileHandle.standardError.write(Data("Unable to write: \(error)\n".utf8))
	exit(1)
}
