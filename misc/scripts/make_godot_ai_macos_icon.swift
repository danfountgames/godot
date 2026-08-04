#!/usr/bin/env swift

/**************************************************************************/
/*  make_godot_ai_macos_icon.swift                                        */
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

import AppKit
import Foundation

private let canvas_size = NSSize(width: 1024, height: 1024)

guard CommandLine.arguments.count == 3 else {
	FileHandle.standardError.write(Data("Usage: make_godot_ai_macos_icon.swift <source.icns> <output.png>\n".utf8))
	exit(2)
}

guard let source_image = NSImage(contentsOfFile: CommandLine.arguments[1]) else {
	FileHandle.standardError.write(Data("Unable to load the source icon.\n".utf8))
	exit(1)
}

guard let bitmap = NSBitmapImageRep(
	bitmapDataPlanes: nil,
	pixelsWide: Int(canvas_size.width),
	pixelsHigh: Int(canvas_size.height),
	bitsPerSample: 8,
	samplesPerPixel: 4,
	hasAlpha: true,
	isPlanar: false,
	colorSpaceName: .deviceRGB,
	bytesPerRow: 0,
	bitsPerPixel: 0
), let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
	FileHandle.standardError.write(Data("Unable to create the icon canvas.\n".utf8))
	exit(1)
}

bitmap.size = canvas_size
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
context.imageInterpolation = .high
source_image.draw(in: NSRect(origin: .zero, size: canvas_size))

let badge_rect = NSRect(x: 654, y: 72, width: 306, height: 306)
let badge_path = NSBezierPath(ovalIn: badge_rect)

NSGraphicsContext.saveGraphicsState()
let badge_shadow = NSShadow()
badge_shadow.shadowColor = NSColor(calibratedWhite: 0.0, alpha: 0.55)
badge_shadow.shadowBlurRadius = 24
badge_shadow.shadowOffset = NSSize(width: 0, height: -12)
badge_shadow.set()
NSColor(calibratedRed: 0.12, green: 0.06, blue: 0.42, alpha: 1.0).setFill()
badge_path.fill()
NSGraphicsContext.restoreGraphicsState()

let badge_gradient = NSGradient(colors: [
	NSColor(calibratedRed: 0.36, green: 0.12, blue: 0.92, alpha: 1.0),
	NSColor(calibratedRed: 0.12, green: 0.06, blue: 0.42, alpha: 1.0),
])!
badge_gradient.draw(in: badge_path, angle: -45)

NSColor(calibratedRed: 0.19, green: 0.91, blue: 0.98, alpha: 1.0).setStroke()
badge_path.lineWidth = 14
badge_path.stroke()

let text = NSString(string: "AI")
let text_attributes: [NSAttributedString.Key: Any] = [
	.font: NSFont.boldSystemFont(ofSize: 160),
	.foregroundColor: NSColor.white,
	.kern: -5.0,
]
let text_size = text.size(withAttributes: text_attributes)
let text_origin = NSPoint(
	x: badge_rect.midX - text_size.width / 2,
	y: badge_rect.midY - text_size.height / 2 + 3
)
text.draw(at: text_origin, withAttributes: text_attributes)

NSGraphicsContext.restoreGraphicsState()

guard let png_data = bitmap.representation(using: .png, properties: [:]) else {
	FileHandle.standardError.write(Data("Unable to encode the branded icon.\n".utf8))
	exit(1)
}

do {
	try png_data.write(to: URL(fileURLWithPath: CommandLine.arguments[2]), options: .atomic)
} catch {
	FileHandle.standardError.write(Data("Unable to write the branded icon: \(error)\n".utf8))
	exit(1)
}
