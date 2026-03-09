// livemount_overlay.swift — Overlay registration for external apps.
// Part of Godot LiveMount Module.
//
// Apps linking libgodot.a can register a SwiftUI overlay view via this
// singleton. The engine's app.swift observes changes and renders the
// registered overlay on top of the Godot rendering view.

import SwiftUI

@MainActor
public final class LiveMountOverlay: ObservableObject {
    public static let shared = LiveMountOverlay()
    @Published public var content: AnyView? = nil
    private init() {}
}
