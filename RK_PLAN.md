# Godot → Apple Vision Pro Renderer
## Project Plan v1.5 (Adversarially Reviewed, Scope-Collapse-Aware)

**Version**: 1.5
**Date**: 2026-03-08
**Status**: v1.5 — Feasibility program with concrete kill switches and explicit scope-collapse tracking. Godot line references are illustrative only (see line-reference policy below). RealityKit API existence confirmed from public docs; runtime viability is UNPROVEN. v1.5 strengthens input-routing proof (3-part: entity target + Godot receiver + coordinate space), downgrades hit normal to unproven payload, fixes collision shape default order, adds dual-path instancing benchmark reporting, and tightens M2 hostile scene classification.
**Pivot**: Replaces the Web Canvas Compatibility Renderer plan. Complete architectural pivot.
**Scope-collapse warning**: This project may conclude successfully by proving only a constrained Godot 3D subset is viable on RealityKit. That outcome counts as success ONLY if explicitly accepted and labeled (see §10 outcome tiers). A plan that silently narrows from "Godot spatial renderer" to "rigid PBR scene bridge with many exclusions" has failed its own honesty standard even if every proof passes.
**Honesty policy**: This plan distinguishes between "an API exists" and "the pipeline is viable." Where evidence is missing, the plan says so. Unproven assumptions are flagged with ⚠️. Proofs cannot be passed with toy scenes — a locked hostile test corpus prevents optimism-driven demo selection.
**Line-reference policy**: All Godot source line numbers in this document are illustrative, based on a snapshot read of 4.6-stable. They are NOT pinned to a specific commit hash. Before forking, every line reference must be revalidated against the exact tagged commit and replaced with commit-hash permalinks (e.g., `github.com/godotengine/godot/blob/<hash>/path#L123`). Until then, treat them as "approximately here" — do not cite them as proof of anything.

---

## Changelog

### Canvas/Web Plan → AVP Plan (Project Pivot)

| Area | Previous (Canvas/Web) | New (AVP/RealityKit) |
|------|----------------------|----------------------|
| Rendering target | HTML5 Canvas 2D API | RealityKit (visionOS spatial renderer) |
| Scene dimension | 2D only | 3D spatial |
| Bridge protocol | Binary command buffer (draw opcodes) | Entity state updates (create/update/destroy) |
| Renderer role | Translates draw commands to Canvas API calls | Forwards final render state to RealityKit entities |
| Godot role | Full engine + renderer replacement | Full engine for simulation, headless renderer |
| Platform | Web browsers | Apple Vision Pro (visionOS) |
| Prior art | None | Unity PolySpatial, GodotVision (archived), Godot 4.5 visionOS PR #105628 |
| Language bridge | C++ → JS (Emscripten) | C++ → Swift (`extern "C"` call mechanism; system overhead UNKNOWN — see §2.9) |

---

## 1. PROJECT BRIEF

### 1.1 Problem Statement

Godot 4.6 has no spatial rendering capability for Apple Vision Pro. The engine can compile for visionOS (initial platform support landed in Godot 4.5 via PR #105628), but the rendering pipeline targets flat screens via Vulkan/Metal/OpenGL. visionOS requires content to be rendered through **RealityKit** for spatial presentation — volumes, immersive spaces, hand tracking, eye tracking, and passthrough compositing are all mediated by RealityKit's rendering pipeline.

A traditional GPU renderer (Vulkan/Metal) cannot:

1. **Composite with passthrough** — visionOS requires RealityKit for real-world blending
2. **Participate in spatial interactions** — hand/eye input is routed through RealityKit entities
3. **Render in shared spaces** — multiple apps coexist; only RealityKit content is composited by the system
4. **Access spatial anchoring** — world-locked content requires RealityKit's anchor system

### 1.2 Proposed Solution

Build a **Godot → RealityKit bridge** that:

- Runs the full Godot engine (scene tree, GDScript, physics, animation, audio, networking) as the **simulation layer**
- Extracts final render state (meshes, materials, transforms, visibility) from Godot's rendering pipeline
- Streams entity state updates to RealityKit through a C++ → Swift bridge
- Presents content via **SwiftUI + RealityView** as the spatial presentation layer

This architecture mirrors **Unity PolySpatial** for visionOS: the game engine runs simulation, RealityKit renders.

### 1.3 What This Is

- A **spatial presentation bridge** from Godot to RealityKit
- An **additional platform target**, not a replacement for existing renderers
- A runtime that keeps Godot authoritative for all simulation (transforms, physics, animation, scripting)
- Framed for upstream as: **"visionOS Spatial Renderer via RealityKit"**

### 1.4 What This Is NOT

- Not a port of Godot's GPU renderer to Metal for visionOS (that exists separately)
- Not a scene graph replicator (RealityKit receives flat render state, not hierarchy)
- Not a shader transpiler (GLSL → Metal conversion is not reliable; explicit material mapping instead)
- Not a replacement for the existing windowed visionOS support in Godot 4.5+

### 1.5 Prior Art

| Project | Architecture | Status | Lessons |
|---------|-------------|--------|---------|
| **Unity PolySpatial** | Unity simulation + RealityKit rendering | Production (visionOS 1.0+) | Volume Camera concept, material mirroring overhead, scene complexity limits |
| **GodotVision** | Godot headless + SwiftGodot + RealityKit | Archived (github.com/kevinw/GodotVision) | Cooperative main-thread ticking, transform streaming, SwiftGodotKit embedding |
| **Godot 4.5 visionOS** | Native platform, windowed Metal rendering | Merged (PR #105628) | Platform scaffolding exists; spatial/immersive support is future work |
| **SwiftGodot** | Swift GDExtension bindings | Active (github.com/migueldeicaza/SwiftGodot) | GDExtension API for Swift interop, node inspection |

---

## 2. ARCHITECTURE

### 2.1 Core System Philosophy

```
Godot  = authoritative simulation engine (transforms, physics, animation, scripting)
RealityKit = pure presentation layer (meshes, materials, lighting, spatial compositing)
```

Godot does NOT delegate simulation to RealityKit.
RealityKit does NOT replicate Godot's scene graph.

The bridge sends **final render state**, not scene hierarchy. RealityKit receives exactly what it needs to draw: mesh, material, world transform, visibility.

### 2.2 High-Level Pipeline

```
Godot Runtime (C++)
         │
    SceneTree tick (physics, animation, scripting)
         │
    RenderingServer processes instances
         │
    RendererSceneCull computes visibility, final transforms
         │                                                    ┐
    HOOK POINT: extract render state from geometry instances   │
         │                                                    │ NEW CODE
    Render Extraction Layer (diff against previous frame)      │
         │                                                    │
    Flat entity update stream                                  │
         │                                                    │
    C Bridge (extern "C")                                      │
         │                                                    ┘
    Swift EntitySynchronizer
         │
    RealityKit entity create / update / destroy
         │
    SwiftUI + RealityView presentation
         │
    visionOS compositor → spatial display
```

### 2.3 Why the Scene Graph Is NOT Replicated

Godot already performs, every frame:

- **Hierarchy transform propagation** — parent → child transforms computed in `RendererSceneCull::_update_instance()` (`renderer_scene_cull.cpp`)
- **Physics simulation** — collision, rigid body, area detection
- **Animation evaluation** — skeletal, property, blend tree
- **Visibility determination** — frustum culling, occlusion, layer masks

RealityKit receives the **output** of all this work:

```
mesh_id          ← which geometry to render
material_id      ← which material to apply
world_transform  ← final 4x4 matrix (already computed by Godot)
visible          ← should this entity be drawn
```

This eliminates:

- Reparenting logic
- Hierarchy synchronization
- Transform propagation in RealityKit
- Duplicate physics/animation state

### 2.4 The Godot Renderer Hook (Critical Integration Point)

**Line references below are illustrative** (see line-reference policy in header). They are based on a snapshot read of the Godot 4.6-stable source tree and must be revalidated against the exact commit hash before forking.

The hook intercepts the renderer pipeline where final render items are produced. **This is the most optimistic part of the plan** and the most likely to fail on contact with reality.

#### What `render_scene()` actually receives

The `render_scene()` virtual method (`renderer_scene_render.h`, `RendererSceneRender` class) receives **far more** than geometry:

```
render_scene(
    geometry_instances,          ← WE USE THIS
    lights,                      ← ⚠️ NOT FORWARDED (see §2.17)
    reflection_probes,           ← ⚠️ NOT FORWARDED
    voxel_gi_instances,          ← ⚠️ NOT FORWARDED
    decals,                      ← ⚠️ NOT FORWARDED
    lightmaps,                   ← ⚠️ NOT FORWARDED
    fog_volumes,                 ← ⚠️ NOT FORWARDED
    environment,                 ← ⚠️ NOT FORWARDED
    camera_attributes,           ← ⚠️ NOT FORWARDED
    shadow_atlas,                ← ⚠️ NOT FORWARDED
    shadow_data,                 ← ⚠️ NOT FORWARDED
    sdfgi_data,                  ← ⚠️ NOT FORWARDED
    ...)
```

**The plan currently cherry-picks only the easy part of this handoff.** Everything marked ⚠️ is visual information that affects how Godot scenes look but has no direct representation in the bridge. See §2.17 for the honest accounting of what is lost.

#### Where Godot produces the data we use

**File:** `servers/rendering/renderer_scene_cull.cpp` (see Appendix A for illustrative line references)

```
instance_set_transform(RID, Transform3D)
         │
    _instance_queue_update(instance, true)           ← queues AABB recompute
         │
    _update_instance(instance)
         │    computes world AABB, calls:
         │    geom->geometry_instance->set_transform(xform, aabb, transformed_aabb)
         │
    _scene_cull(cull_data, cull_result, ...)
         │    frustum culling, visibility range, occlusion
         │    cull_result.geometry_instances.push_back(idata.instance_geometry)
         │
    scene_render->render_scene(cull_result.geometry_instances, ...)
```

#### Hook strategy

The AVP backend implements `RendererSceneRender` and overrides `render_scene()` as a **render state extractor**:

1. Receives the `PagedArray<RenderGeometryInstance*>`
2. For each geometry instance, reads: `base` (mesh RID), `transform`, `surface_materials[]`, `layer_mask`
3. Diffs against the previous frame's state
4. Emits only changed entities to the bridge

**⚠️ UNPROVEN ASSUMPTION:** That a backend which does not actually perform GPU rendering still receives fully populated, coherent `RenderGeometryInstance` data. The existence of `RasterizerDummy` suggests this is possible (its `render_scene()` is a no-op and Godot doesn't crash), but "doesn't crash" ≠ "data is complete and correct." Specifically:

- Does culling still produce correct results without GPU-side occlusion queries?
- Are material RIDs resolvable without the renderer's own material storage being initialized?
- Is skeleton data still populated if no GPU skinning pass runs?
- Does LOD selection still function without screen-space metrics from a real projection?

**These questions MUST be answered by Proof A (§2.20) before M2 is considered passed.**

#### What each RenderGeometryInstance contains

**File:** `servers/rendering/renderer_geometry_instance.h`, `RenderGeometryInstanceBase` class (see Appendix A)

```
RenderGeometryInstanceBase:
    Transform3D transform           ← world transform (rigid body placement only)
    RID mesh_instance               ← the mesh
    Data.base                       ← base RID (mesh/particles/etc.)
    Data.base_type                  ← RS::InstanceType (MESH, LIGHT, etc.)
    Data.surface_materials[]        ← per-surface material RIDs
    Data.material_override          ← override material (if any)
    Data.skeleton                   ← skeleton RID (⚠️ not final vertex positions)
    uint32_t layer_mask             ← visibility layers
    float lod_bias                  ← LOD selection bias (⚠️ may not function without real projection)
    AABB transformed_aabb           ← world-space bounding box
```

**What this gives us:** rigid mesh placement, material assignment, visibility.
**What this does NOT give us:** skinned vertex positions, blend shape results, particle positions, billboard orientations, camera-relative effects, lighting influence, shadow casting results, GI contribution. See §2.17 and §2.18.

### 2.5 Flat Render Entity Model

Each Godot renderable object becomes a flat render entity. No hierarchy.

```cpp
struct RenderEntity {
    uint64_t id;                    // Stable identifier (from Godot instance RID)
    uint64_t mesh_id;               // RID of the mesh resource
    uint64_t material_id;           // RID of the primary material
    float world_transform[16];      // Column-major 4×4 matrix
    bool visible;                   // Visibility state
    uint32_t layer_mask;            // For VolumeCamera culling
    uint32_t flags;                 // Changed fields bitmask (dirty tracking)
};
```

**RealityKit side:**

```swift
var entityRegistry: [UInt64: ModelEntity] = [:]

// On update:
if let entity = entityRegistry[renderEntity.id] {
    entity.transform = Transform(matrix: simd_float4x4(...))
} else {
    let entity = ModelEntity()
    entity.model = ModelComponent(
        mesh: meshStore[renderEntity.meshId],
        materials: [materialStore[renderEntity.materialId]]
    )
    sceneRoot.addChild(entity)
    entityRegistry[renderEntity.id] = entity
}
```

### 2.6 Entity Lifecycle Operations

The bridge protocol is intentionally minimal:

| Operation | Payload | When Emitted |
|-----------|---------|-------------|
| `CreateEntity(id)` | mesh_id, material_id, transform | New instance enters the scene |
| `DestroyEntity(id)` | — | Instance freed or exits VolumeCamera |
| `SetTransform(id, matrix)` | 16 floats (column-major 4×4) | Transform changed this frame |
| `SetMesh(id, mesh_id)` | mesh RID | Mesh resource changed |
| `SetMaterial(id, material_id)` | material RID | Material changed |
| `SetVisible(id, visible)` | bool | Visibility toggled |

**Persistence:** RealityKit entities persist across frames. Only changes are transmitted. A scene with 500 entities where 20 move per frame emits ~20 `SetTransform` calls, not 500.

### 2.7 Dirty Tracking

Transform updates are sent only when changed.

```cpp
class RenderStateExtractor {
    struct PreviousState {
        Transform3D transform;
        RID mesh;
        RID material;
        bool visible;
    };

    HashMap<uint64_t, PreviousState> previous_frame;

    void extract(const PagedArray<RenderGeometryInstance*>& instances) {
        for (auto* gi : instances) {
            uint64_t id = gi->get_instance_rid().get_id();
            auto& prev = previous_frame[id];

            if (gi->transform != prev.transform) {
                bridge_set_transform(id, gi->transform);
                prev.transform = gi->transform;
            }
            // ... same for mesh, material, visibility
        }
    }
};
```

**Expected traffic:** In a typical scene, 80-95% of entities are static per frame. Only moving objects generate bridge calls.

### 2.8 Resource Stores

RealityKit resources are cached in three stores to prevent duplication.

#### MeshStore

```swift
actor MeshStore {
    private var meshes: [UInt64: MeshResource] = [:]

    func getOrCreate(meshId: UInt64, data: GodotMeshData) async throws -> MeshResource {
        if let existing = meshes[meshId] { return existing }

        let lowLevelMesh = try createLowLevelMesh(from: data)
        let resource = try MeshResource(from: lowLevelMesh)
        meshes[meshId] = resource
        return resource
    }
}
```

Meshes are created using **LowLevelMesh** (`developer.apple.com/documentation/realitykit/lowlevelmesh`) which supports:
- Runtime vertex buffer creation with custom attribute layouts
- `withUnsafeMutableBytes` for zero-copy buffer writes from C++ pointers
- Position, normal, UV (up to 8 channels), tangent, color attributes
- Triangle, triangle strip, line, point topologies

#### TextureStore

```swift
actor TextureStore {
    private var textures: [UInt64: TextureResource] = [:]

    func getOrCreate(textureId: UInt64, data: GodotTextureData) async throws -> TextureResource {
        if let existing = textures[textureId] { return existing }

        let lowLevelTex = try LowLevelTexture(descriptor: .init(
            pixelFormat: mapFormat(data.format),
            width: Int(data.width),
            height: Int(data.height),
            mipmapLevelCount: Int(data.mipmapCount),
            textureUsage: [.shaderRead]
        ))
        // Write pixel data
        let mtlTex = lowLevelTex.replace(using: commandBuffer)
        // ... copy pixel data to mtlTex

        let resource = try TextureResource(from: lowLevelTex)
        textures[textureId] = resource
        return resource
    }
}
```

Textures are created using **LowLevelTexture** (`developer.apple.com/documentation/realitykit/lowleveltexture`) which supports:
- Any `MTLPixelFormat` (rgba8Unorm, rgba16Float, bc7, etc.)
- GPU-side updates via `replace(using: MTLCommandBuffer)`
- Mipmap chains

#### MaterialStore

```swift
actor MaterialStore {
    private var materials: [UInt64: RealityKit.Material] = [:]

    func getOrCreate(materialId: UInt64, desc: MaterialDescriptor) async throws -> RealityKit.Material {
        if let existing = materials[materialId] { return existing }

        let material: RealityKit.Material
        switch desc.type {
        case .pbr:
            material = try await buildPBR(desc)
        case .unlit:
            material = try await buildUnlit(desc)
        case .custom:
            material = try await loadCustomShader(desc.shaderName)
        }
        materials[materialId] = material
        return material
    }
}
```

### 2.9 C++ → Swift Bridge Architecture

**Decision: `extern "C"` bridge.**

Direct Swift/C++ interop (available since Swift 5.9) has known limitations: virtual function dispatch bugs, template restrictions, no `std::function`/`std::variant` support. Godot's C++ codebase uses all of these heavily. The `extern "C"` facade is safer.

**⚠️ On overhead claims:** Isolated C-call overhead is sub-nanosecond in benchmarks. **This is not the system cost.** The real per-frame path is: Godot extraction → dirty diffing → marshaling → Swift boundary → MainActor scheduling → RealityKit entity lookup/update → possible resource creation. The plan does not know the end-to-end cost of this path. Proof C (§2.20) is mandatory before any performance claims are made.

#### Bridge header (`godot_avp_bridge.h`)

```c
#ifndef GODOT_AVP_BRIDGE_H
#define GODOT_AVP_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Entity lifecycle
void godot_avp_entity_create(uint64_t id, uint64_t mesh_id, uint64_t material_id,
                              const float* transform_16);
void godot_avp_entity_destroy(uint64_t id);
void godot_avp_entity_set_transform(uint64_t id, const float* transform_16);
void godot_avp_entity_set_visible(uint64_t id, int visible);
void godot_avp_entity_set_mesh(uint64_t id, uint64_t mesh_id);
void godot_avp_entity_set_material(uint64_t id, uint64_t material_id);

// Batched transform update (preferred — one call per frame)
void godot_avp_entities_update_transforms(const uint64_t* ids,
                                           const float* transforms,
                                           uint32_t count);

// Mesh data upload
void godot_avp_mesh_create(uint64_t mesh_id,
                            const float* positions, const float* normals,
                            const float* uvs, const uint32_t* indices,
                            uint32_t vertex_count, uint32_t index_count);
void godot_avp_mesh_destroy(uint64_t mesh_id);

// Texture data upload
void godot_avp_texture_create(uint64_t texture_id,
                               const uint8_t* pixels,
                               uint32_t width, uint32_t height,
                               uint32_t format);
void godot_avp_texture_destroy(uint64_t texture_id);

// Material descriptor
void godot_avp_material_create_pbr(uint64_t material_id,
                                    uint64_t albedo_tex, float albedo_r, float albedo_g,
                                    float albedo_b, float albedo_a,
                                    float roughness, float metallic,
                                    uint64_t normal_tex, uint64_t emission_tex,
                                    float emission_strength);
void godot_avp_material_create_custom(uint64_t material_id,
                                       const char* shader_name);
void godot_avp_material_destroy(uint64_t material_id);

// Frame synchronization
void godot_avp_frame_begin(void);
void godot_avp_frame_end(void);

// VolumeCamera
void godot_avp_volume_camera_set_bounds(float size_x, float size_y, float size_z);
void godot_avp_volume_camera_set_transform(const float* transform_16);
void godot_avp_volume_camera_set_mode(int mode); // 0=bounded, 1=immersive

// Spatial input callbacks (Swift → C++)
// RealityKit is the SOLE authority for spatial input (eye gaze ray is system-private)
typedef void (*godot_avp_tap_callback)(uint64_t entity_id,
                                        float hit_pos_x, float hit_pos_y, float hit_pos_z,
                                        float hit_normal_x, float hit_normal_y, float hit_normal_z);
// ⚠️ hit_normal fields are UNPROVEN. Public docs confirm entity-targeted gestures
// and coordinate conversion (EntityTargetValue.position), but do not clearly establish
// that a surface normal is directly available in the gesture payload. Prototype must
// verify normal availability; if unavailable, remove these fields and document.
typedef void (*godot_avp_drag_callback)(uint64_t entity_id, int phase, // 0=began, 1=changed, 2=ended, 3=cancelled
                                         float hit_pos_x, float hit_pos_y, float hit_pos_z,
                                         float translation_x, float translation_y, float translation_z);
// NOTE: No hover callback. Apple says apps cannot receive hover event callbacks
// for RealityKit entities — eye gaze hover state is private. Hover is
// presentation-only via HoverEffectComponent styling (see §2.30).

void godot_avp_set_tap_callback(godot_avp_tap_callback callback);
void godot_avp_set_drag_callback(godot_avp_drag_callback callback);

#ifdef __cplusplus
}
#endif

#endif
```

#### Performance budget

At 90 FPS (visionOS target), frame budget is **11.1ms**.

**⚠️ No end-to-end performance data exists for this pipeline.** The following are unknowns that must be measured on device (Proof C, §2.20):

| Operation | Microbenchmark (isolated) | Real System Cost | Status |
|-----------|--------------------------|-------------------|--------|
| C function call boundary | <1 ns per call | Irrelevant in isolation | Known |
| Transform matrix copy (16 floats) | ~64 bytes / ~0.5 ns | Unknown with MainActor scheduling | ⚠️ UNPROVEN |
| Batched 500 transform updates | Unknown | Unknown (includes entity lookup + RK setter) | ⚠️ UNPROVEN |
| Entity create (mesh + material) | Unknown | Unknown (LowLevelMesh + actor overhead) | ⚠️ UNPROVEN |
| Entity destroy | Unknown | Unknown (resource cleanup + GC) | ⚠️ UNPROVEN |
| Mesh update (10K verts) | Unknown | Unknown (MainActor-constrained API) | ⚠️ RED RISK |
| Texture update (1024²) | Unknown | Unknown (MTLCommandBuffer path) | ⚠️ UNPROVEN |

The bottleneck is likely RealityKit entity/resource operations, but **we do not know this until Proof C is complete.**

### 2.10 Swift-Side Architecture

```swift
import SwiftUI
import RealityKit

@main
struct GodotAVPApp: App {
    @State private var godotEngine = GodotEngine()

    var body: some Scene {
        // Bounded volume window
        WindowGroup(id: "game-volume") {
            GodotSpatialView(engine: godotEngine)
        }
        .windowStyle(.volumetric)
        .defaultSize(width: 1, height: 1, depth: 1, in: .meters)

        // Immersive space (optional)
        ImmersiveSpace(id: "immersive") {
            GodotImmersiveView(engine: godotEngine)
        }
    }
}

struct GodotSpatialView: View {
    let engine: GodotEngine

    var body: some View {
        RealityView { content in
            // Create root entity
            let root = Entity()
            content.add(root)
            engine.setSceneRoot(root)
            engine.start()
        } update: { content in
            // Called when SwiftUI state changes
            engine.applyPendingUpdates()
        }
    }
}
```

**RealityView documentation:** `developer.apple.com/documentation/RealityKit/RealityView`

The `make` closure runs once at view creation (async). The `update` closure runs when `@State` changes — NOT every frame. The engine's per-frame update loop uses a separate mechanism (e.g., `Task` with frame timing, or cooperative ticking as in GodotVision).

### 2.11 Spatial Volume Camera

The renderer supports a **SpatialVolumeCamera** concept, modeled after Unity PolySpatial's Volume Camera (`docs.unity3d.com/Packages/com.unity.polyspatial.visionos/manual/VolumeCamera.html`).

This is NOT a projection camera. It defines a **3D bounding region** of the Godot scene that is exported to visionOS.

#### Concept

```
Godot scene (arbitrary size)
         │
    SpatialVolumeCamera node
         │
    Bounding box (size + transform)
         │
    Cull objects outside volume
         │
    Export objects within to RealityKit
```

#### Godot Node Definition

```gdscript
class_name SpatialVolumeCamera extends Node3D

@export var dimensions: Vector3 = Vector3(1, 1, 1)  # Meters
@export var mode: VolumeMode = VolumeMode.BOUNDED
@export var layer_mask: int = 0xFFFFFFFF
@export var scale_content_with_volume: bool = true

enum VolumeMode {
    BOUNDED,    # Content in visionOS volume window
    IMMERSIVE   # Content in immersive space
}
```

#### Culling logic

```
For each RenderGeometryInstance in render list:
    if (instance.layer_mask & volume_camera.layer_mask) == 0:
        skip  # Layer filtered out
    if not volume_camera.aabb.intersects(instance.transformed_aabb):
        skip  # Outside volume bounds
    emit to bridge
```

#### Modes

| Mode | visionOS Mapping | Behavior |
|------|-----------------|----------|
| **Bounded** | Volume Window | Content clipped to bounding box. User sees it as a 3D window. Up to 255 volumes. |
| **Immersive** | Immersive Space | Full scene rendered in spatial context. One per app. No clipping. |

### 2.12 Threading Model

Godot and RealityKit run on different threading models. Two viable approaches:

#### Option A: Cooperative Main-Thread Ticking (GodotVision model)

```
Each RealityKit frame:
    1. Godot ticks on main thread (physics + scene tree + scripting)
    2. Extract changed render state
    3. Apply to RealityKit entities (already on main thread)
    4. RealityKit renders
```

**Pros:** No cross-thread synchronization. Simple. Deterministic.
**Cons:** Godot must complete its tick within the main thread's frame budget (~5-6ms of the 11.1ms frame).

#### Option B: Background Thread + Double Buffer

```
[Godot Thread]                        [Main Thread / MainActor]
    tick engine                             │
    extract render state ──────────> FrameBuffer (lock-protected)
    signal frame ready                      │
                                      drain FrameBuffer
                                      apply to RealityKit entities
                                      RealityKit renders
```

**Pros:** Godot can take longer than one visionOS frame. Better for complex scenes.
**Cons:** One frame of latency. Thread synchronization overhead (NSLock, ~50ns).

**Recommendation:** Start with Option A (simpler). Move to Option B only if Godot's tick exceeds 5ms consistently.

### 2.13 Material Mapping Strategy

#### Tier 1 — Automatic PBR Mapping (⚠️ coverage percentage UNKNOWN — requires Proof B corpus test)

Godot's `StandardMaterial3D` maps directly to RealityKit's `PhysicallyBasedMaterial` (`developer.apple.com/documentation/realitykit/physicallybasedmaterial`):

| Godot StandardMaterial3D | RealityKit PhysicallyBasedMaterial | Notes |
|--------------------------|-----------------------------------|-------|
| `albedo_color` | `baseColor.tint` | UIColor conversion |
| `albedo_texture` | `baseColor.texture` | Via TextureStore |
| `roughness` | `roughness.scale` | Direct float |
| `roughness_texture` | `roughness.texture` | Via TextureStore |
| `metallic` | `metallic.scale` | Direct float |
| `metallic_texture` | `metallic.texture` | Via TextureStore |
| `normal_map` | `normal.texture` | Via TextureStore |
| `emission` | `emissiveColor.color` | UIColor conversion |
| `emission_texture` | `emissiveColor.texture` | Via TextureStore |
| `emission_energy` | `emissiveIntensity` | Direct float |
| `clearcoat` | `clearcoat.scale` | Direct float |
| `clearcoat_roughness` | `clearcoatRoughness.scale` | Direct float |
| `ao_texture` | `ambientOcclusion.texture` | Via TextureStore |
| `transparency` | `blending` | `.opaque` / `.transparent(opacity:)` |
| `cull_mode` | `faceCulling` | `.none` / `.front` / `.back` |

**⚠️ This table shows API-level property correspondence, NOT visual fidelity proof.** Whether these mappings produce perceptually acceptable results depends on:
- How Godot and RealityKit interpret the same roughness/metallic values
- Whether Godot's texture import pipeline (sRGB, channel packing) matches RealityKit's expectations
- Whether RealityKit's PBR model handles the same edge cases (alpha-tested foliage, ORM textures, translucent glass)

**Until Proof B (§2.20) classifies 20 real materials into exact/perceptual/degraded/broken buckets, no coverage percentage should be claimed.**

#### Tier 3 — Explicit Replacement Shaders (custom shaders)

Godot shaders (GLSL-like) cannot be automatically converted to RealityKit materials. Instead, custom shaders declare their RealityKit equivalent via annotation:

```glsl
shader_type spatial;
// @rk_shader: ToonMaterial
// @rk_params: outline_width=0.02, color_steps=4

void fragment() {
    // Godot shader code (ignored on AVP)
}
```

The exporter reads the `@rk_shader` annotation and loads the matching Swift material from a registry:

```
RealityKitShaders/
    ToonMaterial.swift          ← ShaderGraphCoder implementation
    HologramMaterial.swift
    GlassMaterial.swift
    WaterMaterial.swift
```

**ShaderGraphCoder** (`github.com/praeclarum/ShaderGraphCoder`) is used for programmatic shader construction on visionOS:
- 759 shader graph nodes, 117 operators
- Generates MaterialX-compatible graphs
- Covers noise, blending, texture sampling, PBR surface outputs

```swift
// Example: ToonMaterial via ShaderGraphCoder
func toonMaterial(steps: Int, outlineWidth: Float) async throws -> ShaderGraphMaterial {
    let uv = SGValue.uv0
    let baseColor = SGValue.texture(contentsOf: albedoURL).sampleColor3f(texcoord: uv)
    let normal = SGValue.geometryNormal
    let light = SGValue.dot(normal, SGValue.vector3f([0, 1, 0]))
    let quantized = SGValue.floor(light * SGScalar(Float(steps))) / SGScalar(Float(steps))
    let surface = pbrSurface(baseColor: baseColor * quantized)
    return try await ShaderGraphMaterial(surface: surface)
}
```

**Note:** `CustomMaterial` (Metal surface shaders) is available on iOS/macOS but **NOT on visionOS**. visionOS requires ShaderGraph/USD materials. ShaderGraphCoder is the programmatic path.

**⚠️ ShaderGraphCoder is a third-party library** (`github.com/praeclarum/ShaderGraphCoder`), not an Apple platform API. It is real and substantial (759 nodes, 117 operators, active development), but it is not "platform parity" — it is a community bridge over a platform gap. Consequences:

1. **Dependency risk:** The project depends on a single maintainer's open-source library. If the library is abandoned or breaks with a visionOS update, the shader replacement system breaks.
2. **Not equivalent to CustomMaterial.** CustomMaterial on iOS/macOS provides Metal surface shader access. ShaderGraphCoder generates MaterialX shader graphs, which is a different (and more constrained) programming model. Some Godot shader effects that would be trivial in Metal are impossible or awkward in ShaderGraphCoder.
3. **Godot custom shader limitations are partially PLATFORM constraints, not engineering backlog.** When a Godot `.gdshader` cannot be replicated on visionOS, the cause may be "visionOS doesn't support this shader capability" rather than "we haven't built the mapping yet." The material rubric (§2.23) and Proof B must classify failures honestly: platform limitation vs. engineering gap.
4. **Mitigation:** Pin ShaderGraphCoder to a specific version in the project. Run Proof B shader tests against that pinned version. If the library breaks, evaluate whether the project can vendor-fork it or whether shader replacement must be descoped.

#### Material Descriptor Format

The bridge uses a serializable descriptor:

```cpp
struct MaterialDescriptor {
    enum Type { PBR, UNLIT, CUSTOM };
    Type type;

    // PBR fields
    Color albedo_color;
    uint64_t albedo_texture;
    float roughness;
    uint64_t roughness_texture;
    float metallic;
    uint64_t metallic_texture;
    uint64_t normal_texture;
    Color emission;
    float emission_energy;
    uint64_t emission_texture;

    // Custom shader fields
    String shader_name;
    Dictionary parameters;
};
```

### 2.14 Instancing

Repeated objects (vegetation, tiles, particle approximations) should use **MeshInstancesComponent** (announced WWDC 2025, `developer.apple.com/videos/play/wwdc2025/287/`):

```swift
// Godot MultiMeshInstance3D → RealityKit MeshInstancesComponent
var component = MeshInstancesComponent()
let instances = try LowLevelInstanceData(instanceCount: count)

instances.withMutableTransforms { transforms in
    for i in 0..<count {
        transforms[i] = instanceTransforms[i].matrix
    }
}

component[partIndex: 0] = instances
entity.components.set(component)
```

Detection: when the render extraction layer encounters `RS::INSTANCE_MULTIMESH`, it converts to instanced rendering instead of individual entities.

### 2.15 UI Strategy

Godot Control nodes should NOT be mirrored to RealityKit.

Instead, use **SwiftUI + RealityKit Attachments** (`developer.apple.com/documentation/realitykit/realityviewattachments`):

```swift
RealityView { content, attachments in
    // Add 3D content
    content.add(gameRoot)

    // Add floating HUD
    if let hud = attachments.entity(for: "hud") {
        hud.position = [0, 1.5, -1]  // 1.5m up, 1m forward
        content.add(hud)
    }
} attachments: {
    Attachment(id: "hud") {
        GameHUDView(score: engine.score, health: engine.health)
            .glassBackgroundEffect()
    }
}
```

**visionOS 26+** also supports `ViewAttachmentComponent`, which can be added to entities from anywhere (not just the RealityView closure):

```swift
let hudEntity = Entity()
hudEntity.components.set(ViewAttachmentComponent {
    Text("Score: \(score)")
        .font(.extraLargeTitle)
})
gameRoot.addChild(hudEntity)
```

Use cases:
- Floating HUD (score, health, inventory)
- Dialog boxes
- Settings panels
- Spatial labels on objects

### 2.16 Headless Renderer Investigation

The AVP backend should run Godot with a **minimal/headless renderer** so that Godot's own GPU pipeline is not active.

**Illustrative reference (revalidate before forking):** `RasterizerDummy` (`servers/rendering/dummy/rasterizer_dummy.h`) provides a complete no-op renderer. When active:
- `RasterizerSceneDummy` (`rasterizer_scene_dummy.h`) implements all `RendererSceneRender` virtual methods as no-ops
- `render_scene()` is an empty function
- Physics, animation, scene tree updates continue normally
- Game logic execution is unaffected

**The AVP backend can subclass `RendererSceneRender`** and implement `render_scene()` as the render extraction hook, while delegating everything else to the dummy implementations. This means:

1. No GPU resources allocated by Godot
2. No draw calls emitted by Godot
3. All simulation continues at full speed
4. The `render_scene()` override extracts render state and forwards to the bridge

**Alternatively**, use GodotVision's approach: run Godot with `--headless` flag and query the scene tree via GDExtension. This is simpler but loses access to the renderer-level culling data.

**Spike required** (Milestone 0) to determine the right integration depth.

### 2.17 Visual Features NOT Forwarded (Honest Accounting)

The bridge forwards: mesh, material descriptor, world transform, visibility.

The bridge does NOT forward the following. **For each, what happens to the visual result?**

| Godot Feature | What It Does | Bridge Behavior | Visual Impact |
|---------------|-------------|-----------------|---------------|
| **Light2D / Light3D** | Direct/spot/omni/area lighting | Not forwarded. RealityKit uses its own environment lighting. | ⚠️ **Scenes look different.** Godot-authored lighting is lost. RealityKit IBL may or may not approximate the intent. |
| **Reflection probes** | Localized specular reflections | Not forwarded. RealityKit uses its own reflection model. | ⚠️ Reflective surfaces will look wrong in authored scenes. |
| **Baked lightmaps** | Pre-computed GI baked into textures | ⚠️ Lightmap textures may transfer as additional material textures, but RealityKit has no "lightmap" material slot. | Requires investigation. Possibly forward as emissive overlay. |
| **Decals** | Projected textures on surfaces | Not forwarded. No RealityKit equivalent. | **Broken.** Decal-heavy scenes (bullet holes, blood splatter, signage) will be missing content. |
| **Fog volumes** | Volumetric/height fog | Not forwarded. No RealityKit volumetric fog. | **Broken.** Atmospheric scenes lose depth cues. |
| **Shadow casting** | Dynamic shadows from lights | Not forwarded. RealityKit has its own shadow system. | ⚠️ **Unpredictable.** RealityKit may cast shadows from its own lighting, but not matching Godot's shadow setup. |
| **Environment / sky** | Background, ambient light, tonemap | Not forwarded. RealityKit uses visionOS passthrough or its own environment. | ⚠️ Sky/background will not match. Ambient lighting will differ. |
| **Post-processing** | Glow, SSAO, SSR, DOF, tonemap | Not forwarded. No custom post-processing on visionOS. | **Broken.** Scenes relying on post-FX will look flat/washed out. |
| **Screen-space effects** | SSAO, SSR, screen-space fog | Not forwarded. Not available in RealityKit on visionOS. | **Broken.** |
| **Custom shaders (vertex)** | Vertex displacement, wind, water | Not forwarded unless explicit `@rk_shader` replacement exists. | **Broken** unless developer provides replacement. |
| **Custom shaders (fragment)** | Non-PBR surface appearance | Not forwarded unless explicit `@rk_shader` replacement exists. | **Broken** unless developer provides replacement. |
| **Particles (GPU)** | GPU compute particle systems | Not forwarded. No RealityKit compute particle equivalent. | **Broken.** Must use CPUParticles3D or accept loss. |
| **Camera-relative effects** | Billboarding, sprites facing camera, impostors | Not forwarded. Would need camera position forwarding + RealityKit billboarding. | ⚠️ Partially possible via RealityKit billboard component. Needs investigation. |

**What this means:** The bridge produces correct output ONLY for scenes that depend on rigid mesh placement + PBR materials + RealityKit's own lighting. Any scene that was authored with specific lighting, post-processing, custom shaders, or atmospheric effects will look different at best, broken at worst.

**The plan must accept this explicitly.** The bridge is NOT a visual parity layer. It is a spatial presentation adapter for a constrained subset of Godot 3D content.

### 2.18 Red-Risk Features (Unproven Viability)

These features are proposed in the plan but their viability is unproven. Each must be de-risked with a specific proof before being treated as "in scope."

#### 🔴 Skeletal Animation via Mesh Streaming

**The claim:** Extract skinned mesh final vertex positions from Godot, stream to LowLevelMesh per frame.

**Why this is red-risk:**
- LowLevelMesh's `withUnsafeMutableBytes` is reportedly MainActor-constrained. Per-frame vertex buffer updates may cause main-thread contention.
- CPU skinning in Godot + copy to Swift + write to LowLevelMesh = triple the work of GPU skinning. Budget impact unknown.
- A 10K-vertex skinned character at 90 FPS = 900K vertices/sec = ~36 MB/sec of vertex data streaming. For 10 characters = 360 MB/sec.
- RealityKit has its own skeleton/joint animation system. **Alternative:** forward joint matrices instead of final vertices, and let RealityKit skin. This requires mesh assets in a format RealityKit can skin, which may conflict with Godot's mesh representation.

**Resolution options (must pick one by M2):**
1. **Joint-matrix forwarding** — send bone transforms, let RealityKit skin. ⚠️ Requires RealityKit-compatible rig format.
2. **RealityKit-native animation** — export skeletal assets as USDZ and let RealityKit own animation. ⚠️ Loses Godot animation control.
3. **CPU mesh streaming** — the plan's current approach. ⚠️ Performance disaster until proven otherwise.
4. **Skeletal animation out of scope** — accept that skinned characters require pre-baked animation or RealityKit-native assets.

**Required proof:** Proof A (§2.20) must include one skinned mesh and measure extraction + streaming cost on device.

#### 🔴 MainActor Contention

**The claim:** Entity updates and mesh/texture streaming can happen at 90 FPS without hitching.

**Why this is red-risk:**
- Apple developer forum discussions report LowLevelMesh and LowLevelTexture as MainActor-constrained, causing bottlenecks for real-time streaming.
- RealityKit entity property setters (`.transform`, `.model`) may also be MainActor-isolated.
- If ALL RealityKit mutations must happen on MainActor, the plan's entire streaming pitch is bottlenecked by one thread.
- The cooperative ticking model (Option A from §2.12) puts Godot AND RealityKit updates on the same thread, making this even tighter.

**Required proof:** Proof D (§2.20) must explicitly profile MainActor contention on device.

#### 🔴 Data Completeness in Dummy Renderer Path

**The claim:** A `RendererSceneRender` subclass that does no GPU work still receives fully populated `RenderGeometryInstance` data.

**Why this is red-risk:**
- `RasterizerDummy` proves Godot doesn't crash without a real renderer. It does NOT prove the data the plan wants to harvest is complete.
- LOD selection may depend on screen-space metrics from a real camera projection — which doesn't exist in the dummy path.
- Occlusion culling results may depend on GPU-side readback — which doesn't exist in the dummy path.
- Material parameter resolution may depend on shader compilation state — which doesn't exist in the dummy path.

**Required proof:** Proof A (§2.20) must validate data completeness for a representative scene.

### 2.19 Missing Architecture: Lighting, Camera, Input, Memory

#### Lighting Model Ownership

**Who owns lighting truth?**

The plan does not answer this. Options:

| Option | Behavior | Visual Risk |
|--------|----------|------------|
| **RealityKit owns lighting** | Godot lights ignored. RealityKit IBL + spatial environment used. | Scenes authored with specific Godot lighting will look wrong. |
| **Partial forwarding** | Forward Godot light positions/colors as RealityKit lights. | Mismatch between Godot and RealityKit light transport models. Shadows, falloff, GI all differ. |
| **Accept visual drift** | Document that lighting is "different, not wrong." | Honest but limits appeal. |

**Recommendation:** Option 1 (RealityKit owns lighting) with honest documentation. Attempting to mirror Godot lights into RealityKit will produce uncanny-valley results that are worse than simply accepting RealityKit's own lighting.

**This must be decided and documented before M2.**

#### Viewport / Camera Semantics

The VolumeCamera concept (§2.11) describes a 3D bounding box, not a camera. But the mapping from Godot's camera semantics to visionOS presentation has unresolved questions:

- **Scale mapping:** A Godot scene at scale 1 unit = 1 meter maps naturally to RealityKit. But what about games at different scales? The VolumeCamera needs a scale factor, and content scaling interacts with interaction distances and physics.
- **Bounded volume clipping:** PolySpatial docs explicitly state GPU clipping at volume boundaries. Objects partially inside the volume will be visually clipped. This is correct behavior but may surprise developers.
- **Immersive mode constraints:** PolySpatial docs constrain unbounded mode to one per scene. Transitions between bounded and immersive modes have lifecycle implications (ImmersiveSpace open/dismiss).
- **Multiple viewports:** Godot supports multiple viewports/cameras. How does this map? One VolumeCamera per volume? Multiple volumes require multiple WindowGroups?

**These questions are not showstoppers but must be answered by M3.**

#### Input: Eye Gaze Makes RealityKit the Only Possible Authority

On visionOS, spatial input is **eye gaze + hand pinch**. The eye gaze ray is system-private — Apple never exposes it to applications. This has fundamental consequences:

- **Godot CANNOT perform spatial hit testing.** It does not have the eye gaze ray. `Area3D.input_event` and `CollisionObject3D._input_event` cannot fire from spatial taps — there is no ray to cast in Godot's physics world.
- **RealityKit is the sole input authority.** Not by design choice, but by platform constraint. The system casts the eye ray against `CollisionComponent` on RealityKit entities. Only entities with `InputTargetComponent` + `CollisionComponent` receive input.
- **Collision shape fidelity matters.** The RealityKit `CollisionComponent` on each entity determines what the user can tap. If the collision shape doesn't match the visual mesh, the user will miss or hit the wrong thing. The bridge must auto-generate collision shapes from mesh data (default: convex hull) and allow developer override.
- **Input routing:** When a user taps a RealityKit entity, the bridge identifies the corresponding Godot node via entity_id → Godot instance RID reverse lookup, and fires new bridge-specific signals (`spatial_tap`, `spatial_drag`). Note: hover is NOT bridged — Apple says apps cannot receive hover callbacks (§2.30).
- **Drag semantics:** `DragGesture` returns `translation3D` in RealityKit world space. This must be converted back to Godot world space, accounting for any scale/offset from VolumeCamera mapping.
- **Game-rule validation is possible but spatial re-picking is not.** Godot can decide "this entity is not interactable right now" (game rules), but it CANNOT say "actually the user tapped a different entity" — that is determined solely by eye gaze + RealityKit collision.

See §2.30 for full input-authority policy, bridge callback signatures, and collision shape strategy.

#### Resource Lifetime / Memory Pressure

RealityKit users have reported cases where mesh/texture-heavy experiences do not return to baseline memory after teardown. A bridge architecture that mirrors resources into RealityKit is especially vulnerable because:

- Godot owns source data in C++ memory
- The bridge copies data to Swift
- Swift creates RealityKit resources (backed by Metal buffers)
- Three copies of the same data may exist simultaneously

**Required stress tests (Proof E, §2.20):**
- Load scene A → B → A. Does memory return to scene-A baseline?
- Rapidly create/destroy 500 entities. Does memory stabilize?
- Suspend/resume app. Do resources survive correctly?
- Run for 10 minutes. Is there a memory leak trend?

### 2.20 Proof Ladder (Mandatory Evidence Before Proceeding)

**The plan earns the right to proceed only by completing these proofs.** Each proof is tied to a milestone gate.

#### Proof A: Extraction Truthfulness (Required for M2)

Build a Godot test scene containing:
- 5 rigid meshes (different scales, rotations)
- Visibility toggles (2 hidden, 3 visible)
- Parented transforms (3 levels deep)
- LOD / visibility range (1 mesh with LOD)
- Material override on 1 mesh
- 1 MultiMeshInstance3D (50 instances)
- 1 skinned mesh with animation
- 1 GPUParticles3D
- 1 Decal
- 1 FogVolume

For each frame, dump extracted state from the bridge and compare against expected ground truth from Godot-side instrumentation. **The goal is not "it runs." The goal is "the extractor is not lying."**

**Positive pass criteria:**
- Rigid mesh transforms match within float epsilon
- Visibility state matches exactly
- Material RIDs resolve to correct material data
- MultiMesh instance transforms are all captured
- Skinned mesh: at minimum, skeleton data is accessible (even if vertex streaming is not yet proven)
- GPUParticles3D: degradation is graceful (not a crash)
- Decal: acknowledged as not forwarded (not silently dropped)
- FogVolume: acknowledged as not forwarded

**Negative controls (must ALL pass — these catch bridge bugs that positive tests miss):**

| Negative Test | Setup | Expected Result | What It Catches |
|---------------|-------|-----------------|-----------------|
| Hidden entity must not leak | Set 2 meshes to `visible = false` | Neither appears in RealityKit. entityRegistry must not contain them (or they must be disabled). | Bridge ignoring visibility state |
| Deleted entity must not survive | Create entity, then `queue_free()` the node | Entity removed from RealityKit within 1 frame. entityRegistry.count decreases. | Orphaned entities (memory leak, visual corruption) |
| Parent transform must not double-apply | 3-level hierarchy: root rotated 45°, child offset (1,0,0), grandchild offset (0,1,0) | Grandchild world position matches Godot's computed world transform exactly. NOT root_rot × child_offset × grandchild_offset applied twice. | Flat model accidentally re-applying hierarchy transforms |
| Material override must not lag | Change material_override on a mesh, capture next frame | RealityKit entity has new material on the SAME frame (or next frame if double-buffered). Not the frame after that. | Stale material in dirty-tracking cache |
| Recycled RID must not resurrect | Create entity A (RID X). Destroy A. Wait 5 frames. Create entity B (Godot may reuse RID X). | B has its OWN mesh/material/position. B does NOT inherit A's appearance or transform. | ID reuse collision (see §2.27) |

**Cross-frame truth assertions (must ALL pass — these catch temporal bridge bugs):**

Frame-local correctness is not enough. The bridge must handle entity state transitions across frames without leaking stale state:

| Assertion | Frame Sequence | Expected | What It Catches |
|-----------|---------------|----------|-----------------|
| Create → mutate → delete | Frame N: create entity (id=1, mesh=A, pos=origin). Frame N+1: set_transform(id=1, pos=(1,0,0)). Frame N+2: destroy(id=1). | Frame N: entity at origin. N+1: entity at (1,0,0). N+2: entity gone. No stale ghost. | Async dispatch reordering, stale transform after delete |
| Rapid property churn | Frame N: set_material(id=1, mat=red). Frame N+1: set_material(id=1, mat=blue). Frame N+2: set_material(id=1, mat=green). | Entity shows green on frame N+2 (or blue→green if double-buffered). Never shows red after frame N. | Dirty-tracking cache holding stale intermediate values |
| Delete + recreate same frame | Frame N: destroy(id=1). Same frame N: create(id=2) at same position with same mesh. | id=1 gone, id=2 present. They are different entities. No property bleed from 1 to 2. | Create-after-destroy in same frame batch |
| Visibility toggle rapid | Frame N: visible=false. Frame N+1: visible=true. Frame N+2: visible=false. 10 cycles. | Entity toggles visibility correctly each frame. No stuck-visible or stuck-invisible. | Visibility state cache desync |

**Proof A uses test corpus scenes C1, C8, C11, and C16.**

#### Proof B: Visual Parity Buckets (Required for M5)

On physical Vision Pro hardware, compare Godot native output (screenshot) vs bridge output (RealityKit render) for:

| # | Scene | Expected Bucket |
|---|-------|----------------|
| 1 | Baseline PBR (metal sphere, wood floor) | Exact or perceptual |
| 2 | Alpha-tested foliage | Unknown |
| 3 | Normal map + ORM packed textures | Unknown |
| 4 | Emissive material (neon sign) | Unknown |
| 5 | Lightmapped mesh (baked GI) | Degraded or broken |
| 6 | Reflection-probe-heavy scene | Degraded or broken |
| 7 | Skinned character (idle animation) | Unknown (depends on Proof A) |
| 8 | MultiMesh vegetation (200 instances) | Unknown |
| 9 | Transparent glass with refraction | Broken (no screen-space effects) |
| 10 | GPU particles (fire, smoke) | Broken (no GPU particles) |

**For each material, score using the 8-axis rubric (§2.23):** texture orientation, alpha behavior, normal response, metallic/roughness response, emissive response, camera-dependent behavior, double-sided/cull mode, depth/transparency ordering. Classify based on total score.

**Proof B uses test corpus scene C2 (material zoo) as its primary input.** The 10 scenes above are additional coverage from other corpus scenes.

**Pass criteria:**
- Scenes 1-4: must score ≥8 (perceptual match or better) on the rubric
- Scenes 5-10: must be honestly classified with scores and side-by-side screenshots
- Overall: ≤30% of 20 test materials in degraded (score 5-7), ≤10% in broken (score 0-4)
- **Every material gets a published score card.** No material may be classified without its 8-axis breakdown.
- **Texture orientation/channel audit (§2.29) must pass before Proof B is considered valid.** Silent UV/normal/ORM errors contaminate every classification.

#### Proof C: End-to-End Frame Budget (Required for M2)

Measure on physical Vision Pro hardware (NOT simulator). **30-second stress window for each test.**

| Test | P50 | P95 | P99 | Worst Frame | Required |
|------|-----|-----|-----|-------------|----------|
| 100 transform updates / frame | Record | Record | < 2ms | Record | P99 < 2ms |
| 500 transform updates / frame | Record | Record | Record | Record | Document ceiling |
| 50 entity creates in one frame | Record | Record | < 11ms | < 16ms | P99 < 11ms, worst < 16ms |
| 50 entity destroys in one frame | Record | Record | < 11ms | < 16ms | P99 < 11ms, worst < 16ms |
| 5 mesh updates / frame (1K verts each) | Record | Record | Record | Record | Document ceiling |
| 2 texture updates / frame (512²) | Record | Record | Record | Record | Document ceiling |
| Interaction active during churn | N/A | N/A | N/A | N/A | Tap latency < 100ms |

**Reporting requirements:**
- **ALL columns must be filled.** "Record" means the value must be measured and reported even if no threshold is set.
- **Frame-time histograms required** for each test (not just summary statistics). Export as CSV or Instruments trace file.
- **P50/P95/P99/worst-frame** reported for a continuous 30-second window under load. NOT cherry-picked "best run."
- **Hitch count:** number of frames exceeding 16ms (missed VSync) during the 30-second window. Threshold: ≤5 hitches for pass, ≤20 for conditional pass, >20 for fail.
- RealityKit / MainActor bottlenecks are hitch problems, not average-throughput problems. The worst-frame column exists to catch them.

**Long-run hitch test (required for Proof C completeness at M2, repeated at M8):**

The 30-second microbenchmark window catches acute bottlenecks. It does NOT catch problems that emerge from cache churn, resource fragmentation, or GC pressure over time. "Fine for 30 seconds" can lie.

| Test | Duration | Corpus Scene | Metrics Required |
|------|----------|-------------|-----------------|
| Sustained static | 10 minutes | C1 (50 static meshes) | P95 frame time at minute 1, 5, 10. Hitch count per minute. RSS trend. |
| Sustained animated | 10 minutes | C3 (skinned characters) — at hard-cut ceiling from §2.24 | P95 frame time at minute 1, 5, 10. Hitch count per minute. RSS trend. |
| Sustained churn | 10 minutes | C5 (create/destroy storm) | P95 frame time at minute 1, 5, 10. Hitch count per minute. RSS trend. entityRegistry.count stability. |

**Pass criteria:**
- P95 frame time must not drift upward >20% from minute-1 to minute-10
- Hitch count (frames >16ms) per minute must not increase monotonically
- RSS must not grow monotonically (>10MB growth from minute-1 to minute-10 = fail)
- entityRegistry.count must remain stable for C1 and C3 (±0). For C5 (churn), must oscillate around a stable mean (±10%).

#### Proof D: Fork Invasiveness + MainActor Contention (Required for M2)

**Part 1: Fork invasiveness audit**

| Metric | Tool | Pass Criterion |
|--------|------|---------------|
| Files modified outside `drivers/avp/` and `platform/visionos/` | `git diff --stat` | ≤ 15 files |
| LOC changed in shared files | `git diff --stat` | ≤ 200 LOC |
| Subsystems touched (rendering, scene, core, servers, editor) | Manual review | ≤ 3 subsystems |
| **Rebase rehearsal:** cherry-pick all AVP commits onto latest upstream `master` | `git rebase` | Resolves with ≤ 5 conflicts, none in critical paths |
| **Deleteability review:** compile Godot with AVP code removed (or compile-flagged out) | `scons platform=linux` (no AVP) | Builds and passes existing tests with zero AVP-related changes visible |

**A fork can stay under 200 LOC and still touch the worst possible 12 files.** The subsystem count and deleteability review catch that.

**Part 2: MainActor contention profiling**

Explicitly profile on device using Instruments:

| Measurement | Tool | Pass Criteria |
|-------------|------|--------------|
| Time spent awaiting MainActor | Time Profiler | < 1ms / frame |
| Time inside RealityKit entity setters | Time Profiler | < 2ms / frame for 100 updates |
| Hitching during mesh streaming | Metal System Trace | No hitches > 5ms |
| Hitching during texture replacement | Metal System Trace | No hitches > 5ms |
| Hand input responsiveness during churn | Manual test | No perceptible lag |

**If MainActor contention exceeds 3ms/frame for typical workloads, the cooperative ticking model (Option A) is dead and Option B is required.**

#### Proof E: Failure Envelopes + Lifecycle Abuse (Required for M8)

Deliberately stress and break the system. **RealityView sits inside SwiftUI lifecycle semantics.** Integration projects usually die at lifecycle boundaries, not during steady-state rendering.

**Stress tests:**

| Test | What It Proves | Pass Criterion |
|------|---------------|----------------|
| Rapidly spawn/despawn 500 entities (10 frames) | Entity pool handles churn | No crash, entityRegistry returns to baseline ±5, no frame >50ms |
| Toggle immersive/bounded modes 5 times rapidly | Mode transitions are stable | No crash, correct mode active after each toggle, entities survive |
| Resize bounded volume during gameplay | Content rescales correctly | Entities reposition smoothly, no visual glitches |
| Run for 10 minutes with 200 entities | No memory growth trend | RSS growth < 5MB over 10 minutes |

**Lifecycle abuse tests (SwiftUI-specific):**

| Test | Setup | Pass Criterion | What It Catches |
|------|-------|----------------|-----------------|
| **View create/destroy churn** | Dismiss and recreate the hosting RealityView 10 times in 30 seconds | No crash, no leaked entities, memory returns to baseline ±20MB | RealityView lifecycle cleanup failures |
| **Suspend / resume** | Move app to background (press Digital Crown), wait 30 seconds, resume | Scene state intact, entities in correct positions, 90 FPS within 2 seconds of resume | State loss on suspend, RealityKit resource invalidation |
| **Immersive ↔ bounded transitions** | Enter immersive space → exit → enter bounded volume → exit → re-enter immersive. 5 cycles. | No crash, correct entities visible in each mode, memory stable | Scene root confusion, entity duplication across spaces |
| **Volume resize** | While scene is rendering, resize bounded volume from 1m³ → 0.5m³ → 2m³ → 1m³ | Content rescales correctly each time, no entities lost or duplicated | VolumeCamera recalculation failures |
| **Hot scene reload** | Load scene A (50 entities) → unload → load scene B (30 entities) → unload → load scene A again. Compare memory and entity state to first load. | Final state matches first load of scene A. RSS within 20MB of first-load baseline. | Resource leaks, stale cache entries, ID collisions across scene loads |

**Memory-floor test (§2.28) is a prerequisite for Proof E.** If memory-floor fails, Proof E lifecycle tests are meaningless.

**Proof E uses test corpus scenes C5, C7, C12, and C16.**

### 2.21 2D Game Support (Not Just "Deferred")

The plan's 2D answer ("deferred, spike required") understates the problem. Many Godot projects are 2D, mixed 2D/3D, or depend on:

- HUDs and Control nodes
- Sprite-based games (the entire 2D pipeline)
- Billboards and screen-space text
- 2D overlays on 3D content

**If 2D is out, the platform target is much narrower than the plan implies.** The plan must be honest about this:

**For v1.0, 2D game support is OUT OF SCOPE.** The bridge supports 3D spatial content only. Godot projects that are primarily 2D should use the windowed visionOS support (Godot 4.5 PR #105628) instead.

**For UI overlays** (HUD, menus, dialogs): use SwiftUI + RealityKit Attachments (§2.15). This is native visionOS UI, not Godot Control nodes. The developer must re-implement UI in SwiftUI.

**For mixed 2D/3D:** a future option is rendering Godot's 2D canvas to an offscreen texture and displaying it as a flat panel entity in RealityKit. This is feasible but out of scope for v1.0.

**Spike required** (Milestone 0) to determine the right integration depth.

### 2.22 Locked Test Corpus (Anti-Gaming Measure)

Proofs A through E are only as honest as the scenes they test against. Without a locked corpus, a team can unconsciously (or deliberately) replace failing test scenes with friendlier ones until all proofs "pass."

#### Rules

1. **Freeze before implementation.** The test corpus is defined and committed to version control BEFORE Phase 1 begins. It lives in `tests/avp_corpus/` and is checked in as Godot project files.
2. **No replacement without decision-log entry.** A scene can only be removed or replaced if the reason is documented in the Decision Log (§9) with date and rationale. "It was too hard" is not a valid rationale.
3. **Three scenes are intentionally hostile.** These are marked `[HOSTILE]` and are designed to break the bridge. If the bridge handles them gracefully (correct output OR clean documented degradation), good. If it silently produces wrong output, that's a proof failure.

#### Corpus (13 scenes minimum)

| # | Scene | Content Class | Purpose | Hostile? |
|---|-------|--------------|---------|----------|
| C1 | **Static showroom** | 50 static meshes, 20 materials, 5 textures | Baseline — does the bridge work at all? | No |
| C2 | **Material zoo** | 20 objects, each with a distinct StandardMaterial3D variation (metallic, rough, glass, emissive, alpha-test, double-sided, ORM-packed, etc.) | Proof B corpus — material fidelity classification | No |
| C3 | **Skinned character parade** | 10 skinned characters with skeletal animation playing | Proof for §2.18 red risk — skinned mesh streaming | No |
| C4 | **MultiMesh field** | 1 MultiMesh with 2000 instances (grass/vegetation) | Instancing path validation | No |
| C5 | **Create/destroy storm** | Script spawns 50 entities/sec and destroys 50 entities/sec | Churn, entity pool, ID stability | No |
| C6 | **Input gauntlet** | 100 entities with collision shapes, all tappable/draggable | Input routing + interaction overhead | No |
| C7 | **Volume transitions** | Scene with bounded volume, script toggles bounded↔immersive every 5 sec | Volume camera + SwiftUI lifecycle | No |
| C8 | **Deep hierarchy** | 10 levels of parent-child nesting, animation on root | Validates flat model doesn't double-apply transforms | No |
| C9 | **[HOSTILE] Light-dependent scene** | Scene where visual correctness depends entirely on Godot point/spot lights, baked lightmaps, and shadow casting | Tests lighting gap honestly — EXPECTED to look wrong | Yes |
| C10 | **[HOSTILE] Custom shader gallery** | 8 objects using custom .gdshader (not StandardMaterial3D), no `@rk_shader` annotations | Tests unmapped shader fallback — every object should fall back to default PBR with logged warning | Yes |
| C11 | **[HOSTILE] Particle + billboard scene** | GPUParticles3D, CPUParticles3D, billboard sprites, camera-facing quads | Tests features NOT in the bridge — expected to be missing or degraded | Yes |
| C12 | **Mixed real-world** | Combination: 30 static, 5 skinned, 1 MultiMesh (500 instances), 3 lights, 2 custom shaders, HUD attachment, tappable objects | Closest to a real game. The "honest demo." | No |
| C13 | **Hierarchical input routing** | Parent entity with `InputTargetComponent`, 5 child entities each with `CollisionComponent` and distinct meshes at different positions/scales. Three-part proof required (§2.30): (1) which RealityKit entity was actually targeted, (2) which Godot node becomes the receiver, (3) whether the bridged hit position lands in the correct coordinate space after parent/child routing. All three must be verified per-tap. | No |

#### Additional scenes (optional, encouraged)

| # | Scene | Purpose |
|---|-------|---------|
| C14 | **Texture stress** | 50 objects, each with unique 1024² texture. Tests texture memory pressure. |
| C15 | **LOD scene** | Objects at varying distances with LOD levels. Tests LOD selection without GPU projection. |
| C16 | **Recycled RID torture test** | Script rapidly creates/destroys instances to force RID reuse. Validates no stale entity resurrection. |

#### Pass criteria per scene

Each scene has TWO possible pass outcomes:

- **CORRECT:** Scene renders correctly (or close enough per the material rubric §2.23) on Vision Pro hardware.
- **DOCUMENTED DEGRADATION:** Scene renders incorrectly, but the exact degradation is documented, expected, and matches what §2.17 predicts. The bridge does not crash, leak memory, or silently corrupt other entities.

A scene FAILS if:
- Bridge crashes or hangs
- Memory grows without bound
- Silent visual corruption in OTHER entities (contamination)
- Degradation occurs but is NOT predicted by §2.17 or §2.18 (surprise failure)

### 2.23 Material Quality Rubric (Replacing Subjective Buckets)

The v1.1 material fidelity classification used four buckets (exact / perceptual / degraded / broken). "Degraded" was a loophole — it could mean anything from "slightly shinier" to "completely different look."

#### Scoring rubric (6 dimensions, scored per material)

For each test material in the corpus (scene C2), score on these 6 axes. Each axis is scored 0 (wrong), 1 (close), or 2 (correct):

| Axis | Score 2 (Correct) | Score 1 (Close) | Score 0 (Wrong) |
|------|------------------|-----------------|-----------------|
| **Texture orientation** | UVs match exactly. No flipping, rotation, or tiling errors. | Minor UV offset (<5% of texture dimension) | Flipped, rotated, or wrong UV channel |
| **Alpha behavior** | Opaque/transparent/alpha-test matches Godot. Cutoff threshold correct. | Alpha mode correct but cutoff threshold slightly off | Wrong alpha mode (opaque renders transparent or vice versa) |
| **Normal response** | Normal map produces correct surface detail. Handedness correct (tangent-space convention matches). | Normal map applies but slight intensity difference (±20%) | Inverted normals, wrong handedness, or normals ignored |
| **Metallic/roughness response** | Metal and rough surfaces visually match Godot preview under similar lighting. | Slight difference in reflection intensity (<±15% subjective) | Metal looks matte, rough looks glossy, or values ignored |
| **Emissive response** | Emissive color and intensity match. Glow visible in comparable conditions. | Emissive present but intensity notably different (±30%) | Emissive absent or wrong color |
| **Camera-dependent behavior** | If material has no camera-dependent features: N/A (scores 2). If it does: documented as unsupported. | Camera-dependent effect partially works | Camera-dependent effect silently missing (no warning logged) |
| **Double-sided / cull mode** | `cull_disabled` renders both faces. `cull_back`/`cull_front` culls correct side. Consistent between Godot and RealityKit. | Cull mode mostly correct but occasional back-face leaking at grazing angles | Wrong cull mode (single-sided renders double or vice versa), or faces inverted |
| **Depth / transparency ordering** | Transparent objects sort correctly relative to each other and to opaque objects. No z-fighting between overlapping transparent layers. | Minor ordering artifacts at extreme overlap but generally correct | Transparent objects render in wrong order, behind opaque objects they should be in front of, or persistent z-fighting |

**Why the last two axes matter:** Many materials that "look fine in screenshots" fail in motion on exactly these axes. Cull-mode errors produce invisible back faces or double-rendering artifacts. Transparency ordering errors produce flickering or pop-in that only manifests with layered transparent geometry — exactly the scenario a static screenshot misses.

#### Classification from scores

| Total Score (out of 16) | Classification | Meaning |
|------------------------|---------------|---------|
| 14-16 | **Exact** | Visually indistinguishable or near-identical |
| 11-13 | **Perceptual match** | Recognizably the same material, minor differences acceptable |
| 6-10 | **Degraded** | Same general appearance, but a designer would notice and object |
| 0-5 | **Broken** | Visually wrong. Not usable without replacement shader. |

#### Gate thresholds (using scored classifications)

- **M5 gate:** Of the 20 test materials, ≤6 (30%) may score "degraded" (6-10). ≤2 (10%) may score "broken" (0-5).
- **M5 hard kill:** If >12 (60%) score "degraded" or "broken" combined, material mapping strategy is fundamentally incorrect. Stop and redesign.
- **Reporting requirement:** Every material gets a side-by-side screenshot (Godot editor preview left, RealityKit right) AND a short video clip showing the material in motion (rotation, camera movement) to catch ordering/cull errors. Published in the Proof B report with the 8-axis score card.

### 2.24 Skeletal Animation Hard-Cut Rule

Skeletal animation via CPU-side skinning + per-frame LowLevelMesh re-upload is the plan's most likely murder weapon (§2.18).

**Hard-cut rule:** At the FIRST dedicated animation spike (Phase 4, task 4.2), the following tests must be run within the first 3 days of work:

**Test 1 — Baseline (corpus scene C3):**
1. Load corpus scene C3 (10 skinned characters) on Vision Pro hardware
2. Measure per-frame mesh re-upload cost per character (Instruments Time Profiler)
3. Record max characters at 90 FPS sustained (30-second window, P95 frame time ≤11.1ms)

**Test 2 — Hostile animated scene:**
4. Load a hostile variant: skinned characters + material variation + `InputTargetComponent` on each + spawn/despawn churn (create 2 characters, destroy 2 characters every 5 seconds)
5. Record max characters at 90 FPS sustained (30-second window, P95) under this combined load
6. The hostile result, NOT the baseline, determines the decision matrix tier

**Test 3 — 10-minute soak:**
7. Run the hostile scene at the max character count from Test 2 for 10 continuous minutes
8. Record frame-time trend (is P95 stable, growing, or spiking?)
9. Record RSS at minute 0, 5, and 10 (must not show monotonic growth >10MB)
10. A short animation spike can pass while real cost emerges from cache churn or resource fragmentation. The soak catches this.

**Decision matrix:**

| Characters at 90 FPS (P95) | Decision |
|----------------------------|----------|
| ≥20 (hostile) | Proceed as planned |
| 10-19 (hostile) | Proceed with documented limitation. Update authoring contract: "limit skinned characters to N." |
| 5-9 (hostile) | ⚠️ Evaluate RealityKit-native skeleton API as alternative path. Budget 1 week for spike. |
| <5 (hostile) | **DROP skinned mesh support from v1.0 scope immediately.** Document as known limitation. Do NOT let this poison the rest of the schedule. Move to future work. |

**Additional kill:** If Test 3 (10-min soak) shows P95 frame time drifting upward >20% from minute-1 to minute-10, OR RSS grows monotonically >10MB, skinned support is unstable even if the character count passes. Drop or investigate memory/fragmentation root cause before proceeding.

**Rationale:** CPU-side skinning is the most expensive per-frame operation in the bridge. If it fails, it fails fast and loudly at Phase 4. Carrying the risk further wastes schedule on a path that may be architecturally impossible under MainActor pressure.

### 2.25 Lighting Consequence Table

The bridge does not forward Godot's lighting data (§2.17, decision #18). This section operationalizes what that means for content authors.

#### Feature-by-feature consequences

| Godot Lighting Feature | Bridge Behavior | RealityKit Equivalent | Visual Consequence | Authoring Guidance |
|----------------------|----------------|----------------------|-------------------|-------------------|
| **DirectionalLight3D** | NOT forwarded | RealityKit uses system environment lighting (IBL from passthrough or skybox) | Scenes designed around a specific sun direction will look different. Shadows absent. | Design with ambient/IBL in mind. Accept that visionOS controls the light environment. |
| **OmniLight3D** | NOT forwarded | No equivalent point light in scene | Point-lit areas will be flat-lit by IBL only | Bake lighting into textures or emissive materials for critical areas |
| **SpotLight3D** | NOT forwarded | No equivalent | Spotlight cones absent | Use emissive materials on "lit" surfaces |
| **Baked lightmaps** | NOT forwarded | No equivalent (lightmap textures could theoretically be applied as a second UV channel, but this is not implemented in v1.0) | Baked GI absent. Indoor scenes lose ambient bounce. | Accept IBL-only. Consider future lightmap-as-texture pass. |
| **ReflectionProbe** | NOT forwarded | RealityKit has its own environment reflection system | Reflective surfaces use RealityKit's system reflections, not Godot's captured cubemaps | Accept platform reflections |
| **Environment (sky, fog, ambient)** | NOT forwarded | visionOS controls environment (passthrough AR or system skybox) | Godot's authored sky/fog is invisible | Passthrough IS the environment. For immersive: accept RealityKit's environment settings. |
| **Shadow casting** | NOT forwarded | RealityKit renders its own shadows for entities (GroundingShadowComponent) | Godot shadow maps absent. RealityKit may add its own grounding shadows. | Use `GroundingShadowComponent` on entities that need shadows. Accept platform shadow quality. |
| **VoxelGI / SDFGI** | NOT forwarded | No equivalent | Global illumination absent | Accept IBL-only ambient |
| **Fog volumes** | NOT forwarded | No equivalent | Volumetric fog absent | Not supported. Design without fog. |

#### Benchmark scenes that depend on lighting

From the locked test corpus:
- **C9 [HOSTILE]:** Light-dependent scene. Expected result: DOCUMENTED DEGRADATION. The scene will look visually different because Godot lights are not forwarded. The bridge should not crash, and the geometry/material/transform should still be correct.
- **C12 (mixed real-world):** Contains 3 lights. Expected result: lights are ignored, but the rest of the scene renders correctly.

#### Product decision (required before M3)

**Question:** Is "Godot geometry with RealityKit lighting" acceptable for target use cases?

- If YES: proceed. Document as "spatial content bridge, not visual fidelity replicator."
- If NO: the project scope must expand to include light forwarding (add ~4 weeks), or the project should be descoped to "static content exporter" instead of "runtime bridge."

This decision must be recorded in the Decision Log (§9) with rationale.

### 2.26 Instancing Validation (API vs. Performance)

MeshInstancesComponent exists (WWDC 2025). But API existence does not prove renderer cost reduction.

#### Two-stage validation

| Stage | Test | Pass Criterion | When |
|-------|------|----------------|------|
| **Stage 1: API works** | Create a MeshInstancesComponent with 500 instances from corpus scene C4. All render correctly. | 500 instances visible, correct positions, 90 FPS | Phase 4, task 4.3 |
| **Stage 2: Cost reduction measured** | Compare: (a) 500 individual ModelEntities with same mesh vs (b) 1 entity with MeshInstancesComponent × 500. Record: CPU frame time, GPU frame time (if observable via Metal System Trace), memory per instance, draw/submit cost proxy (RealityKit render time in Instruments). | Option (b) must show ≥30% improvement in **at least TWO** of the four metrics | Phase 4, task 4.3 |

**Why ≥2 metrics:** A single cherry-picked metric can hide failure elsewhere. If instancing saves CPU time but increases memory, or saves draw calls but adds GPU overhead, the benefit is ambiguous at best. Requiring improvement in two independent metrics proves the optimization is real, not illusory.

**If Stage 1 fails:** MeshInstancesComponent does not work as documented. Fall back to individual entities for MultiMesh content (with lower instance ceiling).

**If Stage 1 passes but Stage 2 fails (improvement in ≤1 metric):** Instancing works for correctness but provides no reliable performance benefit. Remove all performance claims about instancing from the plan. Document as "functionally correct but not a performance optimization." Continue using it for MultiMesh correctness only.

#### ⚠️ API Availability Gate

`MeshInstancesComponent` was announced at WWDC 2025 and is available in visionOS 26 (not visionOS 2.0). This plan declares a minimum deployment target of visionOS 2.0 (decision #15). This creates a version conflict:

| API | Minimum visionOS Version | Plan's Minimum Target |
|-----|-------------------------|----------------------|
| LowLevelMesh | visionOS 2.0 | ✅ Available |
| LowLevelTexture | visionOS 2.0 | ✅ Available |
| MeshInstancesComponent | visionOS 26 | ⚠️ NOT available at deployment target |

**Resolution (must be decided during Phase 0 and recorded at M0):**

1. **Option A: Raise minimum to visionOS 26.** Enables MeshInstancesComponent unconditionally. Reduces device compatibility (excludes users on visionOS 2.x who haven't updated).
2. **Option B: Keep visionOS 2.0, use `@available` check.** MeshInstancesComponent is used only on visionOS 26+. On older devices, MultiMesh falls back to individual entities. MultiMesh instancing becomes a performance optimization, not a guaranteed feature.
3. **Option C: Keep visionOS 2.0, drop MeshInstancesComponent entirely.** MultiMesh always maps to individual entities. Simpler but no instancing benefit.

**Default: Option B** (best compatibility with graceful degradation). But this must be explicitly confirmed and recorded in the decision log at M0. Stage 1 validation (above) must be run on the ACTUAL minimum deployment target — testing only on visionOS 26 and claiming "it works" while shipping to visionOS 2.0 is dishonest.

### 2.27 ID Stability Audit

Entity identity flows through: Godot instance RID → `uint64_t id` in C bridge → Swift `entityRegistry[id]` → RealityKit `ModelEntity`. Any collision, reuse, or resurrection along this chain causes silent corruption.

#### Required tests (run as part of Proof A and corpus scene C16)

| Test Case | Setup | Expected | Failure Mode |
|-----------|-------|----------|-------------|
| **RID reuse** | Create entity A (RID 42). Destroy A. Create entity B (Godot may reuse RID 42). | B gets its own RealityKit entity. A's entity is gone. B does NOT inherit A's mesh/material/position. | Stale entity resurrection: B appears where A was with A's appearance |
| **Rapid create/destroy** | Create and destroy 100 entities per second for 10 seconds. | entityRegistry.count returns to baseline (±5). No leaked ModelEntities. | Memory leak or orphaned RealityKit entities |
| **Duplicate bridge calls** | Send `CreateEntity(id=7)` twice in same frame. | Second call is idempotent (updates existing) or logged as warning. No crash. | Crash, duplicate entity, or corrupted registry |
| **Destroy non-existent** | Send `DestroyEntity(id=999)` when no entity 999 exists. | No-op with warning log. | Crash or silent corruption |
| **Frame-boundary ID coherence** | Entity created in frame N, transform updated in frame N+1, destroyed in frame N+2. | All three operations target the same entity. | Wrong entity updated or destroyed due to async dispatch |

#### Implementation requirement

The `EntitySynchronizer` must maintain a generation counter or use Godot's RID generation bits to detect stale IDs. A simple `[UInt64: ModelEntity]` dictionary is vulnerable to RID reuse unless the bridge includes generation information.

### 2.28 Memory-Floor Test

After loading and unloading heavy content, resident memory must return close to baseline. RealityKit entity/resource lifecycle has known cleanup concerns in developer forums.

#### Test protocol

1. Launch app. Wait 5 seconds for stabilization. Record baseline RSS (Resident Set Size) via Instruments.
2. Load corpus scene C1 (50 meshes, 20 materials). Wait 5 seconds. Record peak RSS.
3. Unload scene C1 (destroy all entities, clear all stores). Wait 10 seconds for GC/ARC. Record post-unload RSS.
4. Repeat steps 2-3 with corpus scene C3 (skinned characters) and C4 (MultiMesh).
5. After all three load/unload cycles, record final RSS.

#### Pass criteria

| Metric | Threshold | Rationale |
|--------|-----------|-----------|
| Post-unload RSS vs baseline (per cycle) | ≤ baseline × 1.15 + 10MB | Percentage-based scales with scene size. Fixed 10MB buffer for RealityKit runtime overhead. |
| Final RSS vs baseline (after all 3 cycles) | ≤ baseline × 1.15 + 15MB | Slightly more generous for cumulative minor overhead. |
| **Monotonic growth check** | RSS(cycle3_post) ≤ RSS(cycle2_post) + 5MB AND RSS(cycle2_post) ≤ RSS(cycle1_post) + 5MB | No monotonic growth across cycles. Each unload must return close to the same floor. |
| MeshStore.count after all unloads | 0 | Hard requirement — no leaked mesh resources |
| TextureStore.count after all unloads | 0 | Hard requirement — no leaked texture resources |
| MaterialStore.count after all unloads | 0 | Hard requirement — no leaked material resources |
| entityRegistry.count after all unloads | 0 | Hard requirement — no orphaned entities |

**Additional stress test:** After the three A→B→C cycles, load scene A a fourth time. Compare memory and entity state to the first load of scene A. If they differ by >20MB or entity count differs, there is a cumulative leak.

**If monotonic growth is detected:** RealityKit or the bridge has a leak. Investigate before proceeding past Phase 4. Do not ship with unbounded memory growth. A flat MB threshold alone is arbitrary across different scene sizes — the percentage + monotonic check catches leaks that a fixed threshold would miss for small or large scenes.

### 2.29 Texture Orientation and Channel-Convention Audit

Godot and RealityKit may disagree on texture coordinate conventions, normal map handedness, and channel packing. ShaderGraphCoder has known texture-coordinate issues in public bug reports. These mismatches produce subtle visual errors that are easy to miss in casual testing.

#### Required validation (part of Proof B, scene C2)

| Convention | Godot Standard | RealityKit Standard | Test |
|-----------|---------------|--------------------|----- |
| **UV origin** | Bottom-left (OpenGL convention) | Verify — may be top-left (Metal convention) | Render a texture with asymmetric content (e.g., arrow pointing up-right). Verify orientation matches. |
| **Normal map handedness** | OpenGL convention (Y+ = up) | Verify — Metal/RealityKit may expect DirectX convention (Y+ = down) | Render a normal-mapped surface with a known bump pattern. Check if bumps are inverted. |
| **ORM texture packing** | R=AO, G=Roughness, B=Metallic (Godot convention) | PhysicallyBasedMaterial expects separate textures or specific channel assignments | Test with ORM-packed texture. Verify channels are read correctly. |
| **sRGB vs linear** | Albedo textures are sRGB. Data textures (normal, roughness) are linear. | Verify RealityKit texture import respects the same convention | Render a checkerboard in both sRGB and linear. Compare brightness. |
| **Texture tiling** | `uv1_scale` and `uv1_offset` in StandardMaterial3D | Must be applied when building PhysicallyBasedMaterial | Render a tiled floor texture. Verify tiling matches. |

**Failure on any of these is NOT a kill criterion** — it's a fixable bug. But it must be detected and fixed before Proof B is considered passed. Silent orientation errors contaminate every material classification.

### 2.30 Input-Authority Policy

#### Why RealityKit is the ONLY possible input authority (not a choice)

On visionOS, the primary interaction model is **eye gaze + hand pinch**:

1. The system tracks where the user's eyes are looking (eye gaze ray)
2. The system casts this ray against RealityKit collision geometry (`CollisionComponent`)
3. The user pinches to confirm (tap) or pinch-and-drag (drag gesture)
4. RealityKit delivers the gesture event to the entity that was hit

**The eye gaze ray is NEVER exposed to the application.** It is a system-level input that Apple keeps private for privacy reasons. Neither Godot nor the bridge can access it. This means:

- Godot **cannot** independently determine what the user tapped
- Godot **cannot** cross-validate RealityKit's hit test
- There is no "Godot authoritative" option for spatial input — it is architecturally impossible
- The v1.2 "context-dependent authority table" with "Godot cross-validation for gameplay picks" was **wrong** — Godot has no ray to cross-validate against

**Policy (v1.0):** RealityKit is the sole and mandatory authority for all spatial input.

#### What the bridge forwards

The bridge fully passes through RealityKit gesture data to Godot. Godot receives everything RealityKit provides, which includes:

| Gesture Data | Source | Forwarded to Godot |
|-------------|--------|-------------------|
| **Entity ID** | `EntityTargetValue.entity` | Which Godot entity was tapped/dragged |
| **Hit position** | `EntityTargetValue.position` (entity-local 3D position) | Where on the entity the gaze ray hit |
| **Hit normal** | ⚠️ **UNPROVEN.** Expected from collision result, but public docs do not clearly confirm surface normal is directly available in the gesture payload. Must be verified in prototype (Phase 7.2). If unavailable, remove from bridge and document. | Surface normal at hit point (if available) |
| **Gesture phase** | `.began` / `.changed` / `.ended` / `.cancelled` | Tap vs. drag start/move/end |
| **Gesture type** | `SpatialTapGesture` / `DragGesture` | Which gesture was performed |
| **3D drag translation** | `DragGesture.Value.translation3D` | World-space drag delta for drag gestures |

**What Godot does NOT receive:**

| Data | Why |
|------|-----|
| Eye gaze ray origin/direction | Private. Never exposed by visionOS. |
| Eye gaze confidence / fixation duration | Private. |
| Which eye is dominant | Private. |
| **Hover enter/exit events** | **Apple explicitly says apps cannot receive hover event callbacks for RealityKit entities.** Eye gaze hover state is private. `HoverEffectComponent` provides visual styling only (highlight, shader effect) — not an app-observable event. In visionOS 2, hover state can drive `ShaderGraphMaterial` parameters for visual effects, but still cannot be read by app code. |
| Raw hand joint positions (during gesture) | Available via HandTrackingProvider, but separate from gesture system. Out of scope for v1.0. |

**⚠️ HOVER IS PRESENTATION-ONLY.** The bridge does NOT forward hover events to Godot. Hover is handled entirely on the RealityKit side via `HoverEffectComponent` for visual feedback (e.g., highlight glow when the user looks at an entity). If a developer needs "what is the user looking at?" for gameplay, that information is not available on visionOS. Design gameplay around tap/drag confirmation, not gaze tracking.

#### Consequence for collision shapes

Because RealityKit hit-tests against its OWN collision geometry (not Godot's), the **RealityKit `CollisionComponent` on each entity determines what is tappable.** This means:

1. Every entity that should receive input MUST have `InputTargetComponent` + `CollisionComponent`
2. The bridge must auto-generate `CollisionComponent` from Godot mesh bounds (or Godot collision shapes if available)
3. If RealityKit's auto-generated collision shape doesn't match the developer's intent, the developer must provide explicit collision configuration

**Collision shape strategy (priority order):**

Since visionOS spatial input relies on `InputTargetComponent` + `CollisionComponent`, and collision geometry defines the interactive region, the default must preserve Godot authoring intent as closely as possible. A convex-hull-by-default policy creates false positives in gameplay-heavy scenes where the RealityKit pick region drifts away from the Godot-authored one.

| Priority | Source | Method | Fidelity | Performance |
|----------|--------|--------|----------|-------------|
| **1 (preferred)** | Godot CollisionShape3D | Extract Godot's collision shape, convert to ShapeResource equivalent | **Matches gameplay intent** — author explicitly defined this shape | Depends on shape |
| **2 (fallback)** | Convex hull of mesh | `ShapeResource.generateConvex(from:)` from MeshResource | Medium — approximates mesh surface | Moderate |
| **3 (last resort)** | Mesh bounding box | `ShapeResource.generateBox(size:)` from `transformed_aabb` | Low — over-sized hit area, false positives | Fast |
| **Override** | Exact mesh collision | `ShapeResource.generateStaticMesh(from:)` from MeshResource | High — exact triangle hit test | Expensive — opt-in only |

**Default resolution order:** If the Godot node has a child `CollisionShape3D` (e.g., from a `StaticBody3D` or `Area3D`), use that shape. Otherwise, generate convex hull from mesh data. Bounding box is only used if convex hull generation fails or the developer explicitly requests it. Developer can override per-entity via a `@rk_collision` annotation or GDScript property.

#### Consequence for Godot scripting

Godot's `Area3D.input_event` and `CollisionObject3D._input_event` **do not fire** from spatial taps. They cannot — the eye gaze ray does not exist in Godot's physics world.

Instead, the bridge introduces new events:

```gdscript
# New signals on a bridge-provided node or singleton
signal spatial_tap(entity_id: int, hit_position: Vector3, hit_normal: Vector3)
# ⚠️ hit_normal is UNPROVEN — public docs confirm hit_position but not surface
# normal availability. If prototype shows normals unavailable, this field will be
# removed or replaced with Vector3.ZERO + a documented limitation.
signal spatial_drag_began(entity_id: int, hit_position: Vector3)
signal spatial_drag_moved(entity_id: int, translation: Vector3)
signal spatial_drag_ended(entity_id: int)

# NOTE: No spatial_hover signals. Apple says apps cannot receive hover
# event callbacks for RealityKit entities. Hover is presentation-only
# via HoverEffectComponent (visual highlight when user looks at entity).
```

**If the developer needs Godot-side raycast for gameplay logic** (e.g., "can this entity be picked up based on game rules?"), they can:
1. Receive `spatial_tap(entity_id, hit_position, ...)` from the bridge
2. Look up the Godot node for that entity_id
3. Run their own game-rule validation (`if inventory.is_full(): reject`)
4. But they CANNOT reject based on a different spatial pick — the "what was tapped" question is answered solely by RealityKit

#### Hierarchical InputTargetComponent routing

RealityKit supports a hierarchical pattern where a **parent entity** carries `InputTargetComponent` and **child entities** carry `CollisionComponent`. The system hit-tests against descendant collision shapes but delivers the gesture event to the **ancestor that owns `InputTargetComponent`**.

This creates an entity identity divergence:

| RealityKit reports | Godot expects |
|-------------------|---------------|
| Parent entity (the one with `InputTargetComponent`) | The specific child node whose collision shape was hit |

**Rules for the bridge:**

1. **Default: flat model.** Every RealityKit entity that represents a Godot `MeshInstance3D` gets its OWN `InputTargetComponent` + `CollisionComponent`. No parent/child sharing. This avoids the routing ambiguity entirely.
2. **Grouped input (opt-in only).** If a developer explicitly groups input (e.g., `@rk_input_group` annotation on a parent node), the parent entity gets `InputTargetComponent` and children get only `CollisionComponent`. In this case, the bridge reports the **parent entity ID** and the `spatial_tap` / `spatial_drag` signal fires on the **parent Godot node**. The hit_position still reflects where on the child's collision shape the tap landed.
3. **⚠️ Risk: entity ID mismatch.** If the flat model's `InputTargetComponent` placement accidentally creates a parent/child relationship (e.g., due to RealityKit's entity hierarchy not matching Godot's flat model), the wrong Godot node receives the event. This must be tested explicitly with corpus scene C13.

**Proof requirement (Phase 7, task 7.6):** Build corpus scene C13 — a parent with 5 children, each child with a distinct mesh and collision shape at different positions and scales. Tap each child individually. **Three-part verification required per tap:**

| # | Assertion | How to verify | Failure mode |
|---|-----------|---------------|--------------|
| 1 | **Correct RealityKit entity targeted** | Log `EntityTargetValue.entity.id` on the Swift side before bridge dispatch. Compare against expected child entity ID. | Gesture delivered to parent entity instead of child (or vice versa). Proves the `InputTargetComponent` / `CollisionComponent` placement is wrong. |
| 2 | **Correct Godot node receives the signal** | On the GDScript side, log which node's `spatial_tap` handler fired. Compare against expected Godot node for that child. | Entity→node reverse lookup is broken. RealityKit targeted the right entity but the bridge mapped it to the wrong Godot node. |
| 3 | **Hit position in correct coordinate space** | Compare the bridged `hit_position` (from `EntityTargetValue.position`) against the known surface geometry of the tapped child. Position must be in the expected space (entity-local or world, per bridge convention) and within the child's bounding box. | Coordinate space mismatch — position is in parent space, world space, or an entirely wrong frame. Apple's `EntityTargetValue` docs specifically describe spatial data conversion between gesture location and entity, so this must be validated. |

All three assertions must pass for EVERY tap in BOTH modes:
- **Flat model (default):** each child's Godot node receives the tap with correct hit position on that child's surface
- **Grouped model (opt-in):** the parent Godot node receives the tap with correct hit position on the child's surface (not the parent's origin)

**This policy must be documented in the authoring contract (§2.31).**

### 2.31 Authoring Contract (What Content Authors May and Must Not Do)

Engineering can "pass" every proof while content teams unknowingly produce unsupported scenes. This section defines what a Godot developer must know when targeting the AVP bridge.

#### SUPPORTED (use freely)

| Feature | Notes |
|---------|-------|
| MeshInstance3D with StandardMaterial3D | Core use case. PBR properties mapped automatically (see material rubric §2.23 for fidelity expectations). |
| Node3D transform hierarchy | Fully supported. Hierarchy flattened by the bridge — final world transforms forwarded. |
| AnimationPlayer (property animation) | Transform/visibility animations work. Skeletal animation subject to performance limits (§2.24). |
| MultiMeshInstance3D | Mapped to MeshInstancesComponent on visionOS 26+. Falls back to individual entities on visionOS 2.x (§2.26 API availability gate). Performance validated per §2.26. |
| Visibility toggling (`visible` property) | Mapped to entity enable/disable. |
| Layer masks | Used by VolumeCamera for culling. |
| StaticBody3D / RigidBody3D / CharacterBody3D | Physics runs in Godot. Transforms forwarded to RealityKit. Works. |

#### SUPPORTED WITH LIMITATIONS

| Feature | Limitation | Guidance |
|---------|-----------|----------|
| Skeletal animation (AnimationPlayer + Skeleton3D) | Performance-limited: max N characters at 90 FPS (determined by §2.24 test). CPU-side skinning. | Keep skinned character count below the measured ceiling. Prefer simple rigs. |
| Custom shaders (.gdshader) | Only work if `@rk_shader` annotation maps to a registered RealityKit replacement shader. | Write replacement shaders in Swift using ShaderGraphCoder. Annotate Godot shaders with `@rk_shader: MyMaterial`. |
| LOD (MeshInstance3D with LOD levels) | May not function correctly without GPU-side projection metrics in the dummy renderer. | Test on device. LOD selection may be unreliable. Consider manual LOD switching in GDScript. |

#### NOT SUPPORTED (do not use — will be silently missing or wrong)

| Feature | Why | Alternative |
|---------|-----|-------------|
| Godot lights (Directional/Omni/Spot) | Not forwarded (§2.25). RealityKit owns lighting. | Design for ambient/IBL. Bake critical lighting into textures. |
| Baked lightmaps / ReflectionProbe | Not forwarded. | Accept RealityKit environment reflections. |
| GPUParticles3D / CPUParticles3D | Particle positions not extracted by the bridge. | Implement particle effects natively in RealityKit (ParticleEmitterComponent). |
| Post-processing (WorldEnvironment effects) | Bloom, tonemap, SSAO, SSR, DOF not forwarded. | visionOS does not support custom post-processing. Accept platform rendering. |
| Camera3D (projection) | No concept of "Godot camera" in the bridge. SpatialVolumeCamera defines bounds, not view. | Use SpatialVolumeCamera for volume bounds. RealityKit controls the actual view. |
| 2D nodes (Sprite2D, Control, CanvasItem) | 2D pipeline not bridged (§2.21). | Use SwiftUI + RealityKit Attachments for UI. 2D games should use windowed visionOS mode. |
| Fog volumes | Not forwarded. No RealityKit equivalent. | Design without fog. |
| Decals | Not forwarded. | Bake decals into textures or use separate mesh overlays. |
| Custom vertex shaders | Vertex shader effects (wind, wave, displacement) not forwarded. | Implement in ShaderGraphCoder or accept static geometry. |
| Godot's input events for spatial interaction | `_input_event` on CollisionObject3D not triggered by spatial taps. **Impossible** — eye gaze ray is system-private; Godot cannot compute what the user looked at (§2.30). | Use the bridge's `spatial_tap` / `spatial_drag` signals. These carry entity_id, hit_position, and drag translation directly from RealityKit. hit_normal is ⚠️ UNPROVEN — may not be available (§2.30). **No hover events** — hover is presentation-only. |
| Godot collision shapes for input picking | Godot's `Area3D` / `CollisionObject3D` do NOT determine what is tappable. RealityKit's `CollisionComponent` does. However, **Godot CollisionShape3D is now the preferred source** — if present, the bridge extracts and converts it to a RealityKit ShapeResource. Fallback: convex hull from mesh data. Override with `@rk_collision` if needed. |
| Hover events / "what is the user looking at?" | **Impossible.** Apple says apps cannot receive hover event callbacks for RealityKit entities. Eye gaze hover state is private. | Design gameplay around tap/drag confirmation, not gaze tracking. `HoverEffectComponent` provides visual highlighting only. |
| Grouped input routing (parent receives child taps) | Only available via explicit `@rk_input_group` opt-in (§2.30). Default: each entity receives its own taps independently. | If you need a parent node to receive taps on child meshes, annotate the parent with `@rk_input_group`. Otherwise, connect `spatial_tap` on each node individually. |

#### Content validation checklist (for developers)

Before exporting to AVP, verify:

- [ ] Scene uses only StandardMaterial3D or materials with `@rk_shader` annotations
- [ ] No reliance on Godot lights for visual correctness (or: accept different lighting)
- [ ] Skinned character count below measured ceiling (§2.24)
- [ ] No GPUParticles3D (or: accept they will be missing)
- [ ] UI implemented in SwiftUI, not Godot Control nodes
- [ ] Custom shaders have registered RealityKit replacements
- [ ] Tested on Vision Pro hardware (simulator is insufficient)

---

## 3. DRIVER STRUCTURE

```
drivers/avp/
    rasterizer_avp.h                ← RendererCompositor (make_current pattern)
    rasterizer_avp.cpp

    rasterizer_scene_avp.h          ← RendererSceneRender (render_scene hook)
    rasterizer_scene_avp.cpp        ← Render state extraction + diffing

    rasterizer_canvas_avp.h         ← RendererCanvasRender (2D → attachment or skip)
    rasterizer_canvas_avp.cpp

    render_state_extractor.h        ← Diff engine (previous frame vs current)
    render_state_extractor.cpp

    avp_bridge.h                    ← extern "C" bridge declarations
    avp_bridge.cpp                  ← Bridge implementation (calls Swift callbacks)

    texture_storage_avp.h           ← RID → bridge texture ID mapping
    texture_storage_avp.cpp

    mesh_storage_avp.h              ← RID → bridge mesh ID mapping
    mesh_storage_avp.cpp

    material_storage_avp.h          ← RID → MaterialDescriptor extraction
    material_storage_avp.cpp

    SCsub

platform/visionos/                  ← (extends existing iOS platform or new)
    os_visionos.h
    os_visionos.mm
    display_server_visionos.h
    display_server_visionos.mm
    detect.py
    SCsub

swift/                              ← Swift package (visionOS app + RealityKit)
    Sources/
        GodotAVP/
            GodotEngine.swift       ← Engine wrapper + tick loop
            EntitySynchronizer.swift ← Entity CRUD from bridge calls
            MeshStore.swift         ← LowLevelMesh cache
            TextureStore.swift      ← LowLevelTexture cache
            MaterialStore.swift     ← PBR + custom material cache
            VolumeCameraNode.swift  ← VolumeCamera logic
        GodotAVPApp/
            App.swift               ← SwiftUI app entry point
            GodotSpatialView.swift  ← RealityView integration
    Package.swift
```

### 3.1 Code Volume Estimate

| Component | Language | Lines | Notes |
|-----------|----------|-------|-------|
| `drivers/avp/` (8 files) | C++ | ~2,500 | Render extractor + bridge + storage stubs |
| `avp_bridge.h/.cpp` | C/C++ | ~200 | Thin `extern "C"` facade |
| `platform/visionos/` | Obj-C++ | ~800 | Platform integration (extends iOS) |
| `swift/Sources/GodotAVP/` | Swift | ~2,000 | Entity sync, stores, volume camera |
| `swift/Sources/GodotAVPApp/` | Swift | ~500 | SwiftUI app, RealityView |
| `RealityKitShaders/` | Swift | ~600 | ShaderGraphCoder materials |
| Test suite | Swift/GDScript | ~2,000 | Unit + integration + visual |
| **Total new** | | **~8,600** | |
| **Total modified** | | **~100** | Platform hooks, build system |

### 3.2 Performance Targets (Content-Class Benchmark Matrix)

⚠️ **All targets below are hypotheses. None have been measured.** They become pass/fail criteria ONLY after Proof C (§2.20) establishes baseline measurements.

#### Raw operation targets (microbenchmarks — Proof C)

| Operation | Target | Measurement Method | Status |
|-----------|--------|-------------------|--------|
| Transform update (1 entity) | <0.01ms | Instruments Time Profiler | ⚠️ UNPROVEN |
| Transform batch (200 entities) | <1ms | Instruments, end-to-end | ⚠️ UNPROVEN |
| Entity creation (mesh + material) | <5ms | Instruments, amortized over 10 | ⚠️ UNPROVEN |
| Mesh streaming (10K vertices) | <10ms | Instruments, LowLevelMesh path | ⚠️ UNPROVEN |
| Texture upload (1024² RGBA8) | <5ms | Instruments, LowLevelTexture path | ⚠️ UNPROVEN |
| Godot tick (simulation only) | <5ms | Leaves 6ms for RealityKit | ⚠️ UNPROVEN |
| Full bridge overhead per frame | <1ms | Instruments, frame_begin→frame_end | ⚠️ UNPROVEN |

#### Content-class benchmark matrix (Proof E — required for M8)

"500 cubes at 90 FPS" is not a meaningful test. The following content classes represent **realistic failure modes**:

| Content Class | Scene Description | Pass Criterion | Why This Fails Differently |
|---------------|------------------|----------------|---------------------------|
| **Static rigid** | 300 static meshes, 50 unique MeshResource, 30 unique materials | 90 FPS sustained, <200MB | Baseline. Tests entity count + material diversity |
| **Skinned characters** | 20 skinned characters (5K verts each), playing animations | 90 FPS sustained, mesh re-upload <3ms/character | Tests per-frame mesh streaming bandwidth (see §2.18 red risk) |
| **Unique materials** | 200 static meshes, each with a UNIQUE material + texture | 90 FPS sustained, material creation <100ms total | Tests MaterialStore pressure, texture memory |
| **Create/destroy churn** | 100 entities created + 100 destroyed every 2 seconds | No frame drops >16ms during churn burst | Tests entity pool, RealityKit GC, resource cleanup |
| **High dirty rate** | 500 entities, 400 moving per frame | 90 FPS sustained, transform batch <2ms | Tests worst-case bridge throughput (80% dirty) |
| **Low dirty rate** | 1000 entities, 10 moving per frame | 90 FPS sustained, <300MB | Tests scaling of static scene overhead |
| **Input-targeted** | 200 entities with `InputTargetComponent` + `CollisionComponent` | Tap latency <100ms, 90 FPS sustained | Tests interaction system overhead at scale |
| **Texture churn** | 50 entities, 10 textures re-uploaded per second (1024²) | 90 FPS sustained, no visible hitch | Tests LowLevelTexture update pipeline |
| **MultiMesh instancing** | 1 MultiMesh with 2000 instances (grass/vegetation) | 90 FPS sustained, <150MB | Tests MeshInstancesComponent path. **⚠️ Dual-path reporting required** (see below). |
| **Mixed** | 100 static + 10 skinned + 50 instanced + HUD attachment | 90 FPS sustained, <250MB | Closest to real game content |

#### ⚠️ Dual-path instancing reporting (required)

Because `MeshInstancesComponent` is only available on visionOS 26+ and the plan's minimum target is visionOS 2.0 (§2.26), the **MultiMesh instancing** and **Mixed** content classes must be benchmarked on BOTH paths:

| Path | What it tests | Must report separately |
|------|--------------|----------------------|
| **visionOS 26+ (MeshInstancesComponent)** | Instanced rendering via API | FPS, memory, CPU frame time |
| **visionOS 2.x fallback (individual entities)** | Each instance as separate ModelEntity | FPS, memory, CPU frame time |

The plan CANNOT produce one benchmark graph on the visionOS 26+ path and present it as the shipping baseline when the minimum deployment target ships on the fallback path. Both results must appear side-by-side in the Proof E report. If the fallback path fails the content class criterion but the instancing path passes, the content class is marked as **"PASS on visionOS 26+, FAIL on visionOS 2.x"** — not simply "PASS."

**How to use this matrix:** Each content class is a test scene built during Phase 8. A content class "passes" if it meets its criterion on physical Vision Pro hardware (not simulator). The total pass rate determines project health:
- **8-10 pass:** Production ready
- **5-7 pass:** Viable with documented limitations
- **<5 pass:** Serious architectural problems; reconsider scope

**The previous "500 entities at 90 FPS" target is replaced by this matrix.** There is no single entity ceiling — the ceiling depends on content type.

---

## 4. PITFALLS

### 4.1 RealityKit Entity Creation Cost (Risk: High)

**The trap:** Creating `ModelEntity` instances with `MeshResource` and materials is expensive (~5ms each). A scene transition that spawns 100 entities simultaneously will stall.

**Mitigation:** Entity pooling. Pre-allocate a pool of inactive entities. On create, pull from pool and configure. On destroy, deactivate and return to pool. Spread creation across multiple frames if pool is exhausted.

### 4.2 Shader Incompatibility (Risk: High)

**The trap:** Any Godot project using custom shaders will render incorrectly unless explicit RealityKit replacement shaders are provided. `CustomMaterial` is NOT available on visionOS.

**Mitigation:** The Tier 1/Tier 3 strategy accepts this limitation. Document clearly. Provide a starter library of common replacement shaders (toon, glass, hologram, water). Export-time warnings for unmapped shaders.

### 4.3 2D Game Support (Risk: Medium)

**The trap:** Godot's 2D rendering pipeline (`RendererCanvasCull` → `RendererCanvasRender`) is completely separate from the 3D pipeline. A 2D game won't produce any `RenderGeometryInstance` data.

**Mitigation:** For 2D games, the canvas render backend generates flat quads in 3D space — each CanvasItem becomes a textured plane at z=0. Alternatively, render 2D to an offscreen texture and display as a flat panel in RealityKit. Spike required.

### 4.4 GDExtension vs Renderer Hook Depth (Risk: Medium)

**The trap:** Hooking at the `RendererSceneRender` level requires modifying Godot engine internals. This makes upstream acceptance harder and creates maintenance burden across Godot versions.

**Mitigation:** Evaluate GDExtension-based approach first (read scene tree from Swift via SwiftGodot). If performance is insufficient (too many per-node queries), escalate to renderer-level hook. Document the trade-off.

### 4.5 Latency from Double Buffering (Risk: Low)

**The trap:** If using the background thread model (Option B from §2.12), there's one frame of latency between Godot's simulation and RealityKit's display. For fast-moving objects, this can feel "sluggish."

**Mitigation:** Use cooperative main-thread ticking (Option A) for low-complexity games. For complex games requiring Option B, implement transform extrapolation on the Swift side.

### 4.6 visionOS Platform Evolution (Risk: Medium)

**The trap:** visionOS APIs change significantly between major versions. visionOS 1.0 → 2.0 added LowLevelMesh/LowLevelTexture. visionOS 26 added Observable entities, ViewAttachmentComponent, MeshInstancesComponent.

**Mitigation:** Target visionOS 2.0 as minimum (LowLevelMesh is essential). Use `@available` checks for visionOS 26+ features. Pin SDK version in CI.

---

## 5. WARNINGS

### 5.1 CRITICAL: Not a General-Purpose Renderer

This is a presentation bridge, not a GPU renderer. It inherits RealityKit's limitations: no custom post-processing (on visionOS), limited shader control, material system dictated by Apple. Document this clearly.

### 5.2 CRITICAL: Performance Scales with Scene Complexity

Unity PolySpatial's documentation warns that mirroring overhead scales linearly with scene complexity. Every entity, mesh, material, and texture in the bridge adds overhead. Actual entity ceilings depend on content type — see content-class benchmark matrix (§3.2).

### 5.3 CRITICAL: Test on Device

The visionOS Simulator does NOT accurately represent Vision Pro rendering performance. Many RealityKit features behave differently in simulation. Every milestone must be validated on physical hardware.

### 5.4 WARNING: Godot 4.5 visionOS Support Exists

Godot 4.5 (PR #105628) already has basic visionOS platform support for windowed apps. This project should build on that platform scaffolding, NOT create a parallel platform from scratch. Coordinate with the existing visionOS work.

### 5.5 WARNING: Upstream Acceptance Is Uncertain

A RealityKit-based renderer is architecturally different from anything in Godot today. Upstream acceptance requires careful framing as an opt-in platform target, not a core renderer change. File GIP early.

---

## 6. TASKS

### Phase 0: RealityKit Spike (Week 1)

**Goal:** Prove RealityKit entity creation, update, and destruction works from a Swift app. No Godot involved.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 0.1 | Create visionOS app with RealityView. Display a single ModelEntity (cube). | 0.5 | None | Cube renders in simulator |
| 0.2 | Implement animated transform (rotation). Verify 90 FPS. | 0.5 | 0.1 | Smooth rotation |
| 0.3 | Create 100 entities dynamically. Measure creation time and frame rate. | 0.5 | 0.2 | Performance baseline |
| 0.4 | Create mesh at runtime via LowLevelMesh. Apply PhysicallyBasedMaterial. | 1 | 0.2 | Dynamic mesh works |
| 0.5 | Create texture at runtime via LowLevelTexture. Apply to material. | 0.5 | 0.4 | Dynamic texture works |
| 0.6 | Test on physical Vision Pro hardware. Record performance. | 1 | 0.1-0.5 | Hardware baseline |
| 0.7 | **Build and freeze test corpus** (§2.22). Create 13 canonical scenes (3 hostile). Commit to `tests/avp_corpus/`. No changes after this without decision-log entry. | 1.5 | None | Corpus committed to VCS |
| **0.P** | **RealityKit viability confirmed + test corpus locked** | -- | 0.1-0.7 | **Spike complete, corpus frozen** |

**Kill criteria (concrete — any one kills the approach):**

1. **LowLevelMesh creation latency:** Creating a 5K-vertex LowLevelMesh takes >50ms on Vision Pro hardware (amortized over 10 creations)
2. **Entity ceiling:** 100 static ModelEntities with unique MeshResource + PhysicallyBasedMaterial cannot sustain 90 FPS on hardware
3. **Transform update throughput:** Updating 100 entity transforms per frame drops below 90 FPS on hardware
4. **MainActor starvation:** LowLevelMesh/LowLevelTexture API calls are MainActor-constrained AND creating 10 meshes serially exceeds 5ms of MainActor time (blocking UI)
5. **Texture upload:** A 1024×1024 RGBA8 LowLevelTexture takes >20ms to create and populate

**If any criterion triggers:** Stop. The RealityKit entity model cannot support the target content density. Evaluate alternative architectures (compositor rendering, Metal layer injection, or scope reduction to <20 entities).

### Phase 1: Bridge Prototype (Weeks 2-3)

**Goal:** C++ → Swift bridge works. Fake entity stream creates/updates/destroys RealityKit entities.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 1.1 | Define `godot_avp_bridge.h` — `extern "C"` API for entity lifecycle, transforms, mesh/texture/material create/destroy. | 1 | 0.P | Bridge header |
| 1.2 | Implement Swift `EntitySynchronizer` — entity registry, create/update/destroy dispatch. | 2 | 1.1 | Entity sync works |
| 1.3 | Implement `MeshStore` with LowLevelMesh pipeline. | 2 | 0.4 | Mesh caching works |
| 1.4 | Implement `TextureStore` with LowLevelTexture pipeline. | 1 | 0.5 | Texture caching works |
| 1.5 | Implement `MaterialStore` with PhysicallyBasedMaterial construction. | 2 | 1.4 | PBR materials work |
| 1.6 | Write fake entity stream (C side) that creates 50 entities, moves 10 per frame, destroys 5 after 3 seconds. Verify visual correctness and 90 FPS. | 1 | 1.2-1.5 | Fake stream works |
| 1.7 | Batched transform update: `godot_avp_entities_update_transforms()` — single call for all changed transforms per frame. | 1 | 1.2 | Batched updates work |
| **1.P** | **Bridge prototype: fake entities render in RealityKit** | -- | 1.1-1.7 | **Bridge works** |

### Phase 2: Godot Integration (Weeks 4-7)

**Goal:** Godot engine runs. Render state extracted from `render_scene()`. Real Godot scenes appear in RealityKit.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 2.1 | Fork `RasterizerDummy` → `RasterizerAVP`. Implement `make_current()` following `RendererCompositor::_create_func` pattern. | 2 | 1.P | Compiles |
| 2.2 | Implement `RasterizerSceneAVP : RendererSceneRender`. Override `render_scene()` to extract `PagedArray<RenderGeometryInstance*>`. | 3 | 2.1 | Hook captures geometry |
| 2.3 | Implement `RenderStateExtractor` — diff current vs previous frame, emit create/update/destroy calls to bridge. | 3 | 2.2 | Diffing works |
| 2.4 | Implement `MeshStorageAVP` — extract vertex/index data from Godot's `MeshStorage`. Forward to bridge's `godot_avp_mesh_create`. | 3 | 2.2, 1.3 | Meshes transfer |
| 2.5 | Implement `MaterialStorageAVP` — extract `StandardMaterial3D` parameters, build `MaterialDescriptor`, forward to bridge. | 3 | 2.2, 1.5 | Materials transfer |
| 2.6 | Implement `TextureStorageAVP` — extract texture pixel data, forward to bridge's `godot_avp_texture_create`. | 2 | 2.5, 1.4 | Textures transfer |
| 2.7 | Build system integration: `detect.py`, `SCsub`, platform hooks for visionOS. Build with `scons platform=visionos avp=yes`. | 2 | 2.1 | Builds |
| 2.8 | Embed Godot as library in Swift app (SwiftGodotKit or direct embedding). Cooperative main-thread ticking. | 2 | 2.7 | Godot ticks |
| 2.9 | Test: load a simple Godot scene (3 meshes, 2 materials). Verify it appears correctly in RealityKit. | 2 | 2.3-2.8 | Scene renders |
| 2.10 | **Proof A** (§2.20): Load test scene with meshes, lights, particles. Log all `RenderGeometryInstance` fields. Verify data completeness (≤5% null/missing). | 2 | 2.2 | Proof A report |
| 2.11 | **Proof C** (§2.20): Instrument full frame path on Vision Pro hardware. Record extraction→bridge→entity update→RealityKit times. | 2 | 2.9 | Instruments trace + timing report |
| 2.12 | **Proof D** (§2.20): Count files touched outside `drivers/avp/` and `platform/visionos/`. Must be ≤15 files, ≤200 LOC. | 0.5 | 2.7 | LOC report |
| 2.13 | File GIP with Godot platform maintainers (ONLY if Proofs A/C/D pass). Include proof data. | 1 | 2.10-2.12 | Early upstream engagement (evidence-backed) |
| **2.P** | **PoC: Real Godot scene renders in RealityKit + Proofs A/C/D pass** | -- | 2.1-2.12 | **Integration works, de-risked** |

### Phase 3: Volume Camera + Culling (Weeks 8-9)

**Goal:** VolumeCamera node controls what content appears in the visionOS volume.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 3.1 | Implement `SpatialVolumeCamera` as a Godot Node3D subclass (GDExtension or engine module). Properties: dimensions, mode, layer_mask. | 2 | 2.P | Node exists |
| 3.2 | Integrate VolumeCamera with render extraction — cull entities outside volume bounds using `transformed_aabb` intersection. | 2 | 3.1, 2.3 | Culling works |
| 3.3 | Implement bounded mode — content appears in visionOS volume window. Map Godot coordinates to volume space. | 2 | 3.2 | Bounded mode works |
| 3.4 | Implement immersive mode — content appears in immersive space. No clipping. | 1 | 3.2 | Immersive mode works |
| 3.5 | Test: Scene with 100 objects, VolumeCamera showing 50. Verify only 50 appear in RealityKit and performance is 90 FPS. | 1 | 3.2-3.4 | Volume culling validated |
| **3.P** | **VolumeCamera controls spatial export** | -- | 3.1-3.5 | **Volume camera works** |

### Phase 4: Mesh Streaming + Animation (Weeks 10-12)

**Goal:** Dynamic meshes, skeletal animation, and MultiMesh instancing work.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 4.1 | Implement mesh update pipeline — detect mesh changes (morph targets, blend shapes), re-upload via LowLevelMesh `withUnsafeMutableBytes`. | 3 | 2.4 | Dynamic meshes update |
| 4.2 | Implement skeletal animation — extract skinned mesh final vertex positions from Godot, stream to LowLevelMesh. **Apply hard-cut rule (§2.24) within first 3 days.** | 3 | 4.1 | Skinned meshes animate OR hard-cut decision made |
| 4.3 | Implement MultiMesh → MeshInstancesComponent conversion. Run two-stage validation (§2.26): Stage 1 (API works) then Stage 2 (cost reduction measured). | 2 | 2.3 | Instancing validated (both stages) |
| 4.4 | Implement LOD — extract LOD selection from Godot, switch mesh resources on the RealityKit side. | 1 | 2.4 | LOD works |
| 4.5 | Performance test: measure max skinned characters at 90 FPS on hardware. Target 20, minimum viable 5. Document mesh re-upload cost per character. (⚠️ This is RED RISK — see §2.18. Expect this to underperform.) | 2 | 4.1-4.2 | Skinned character ceiling documented |
| **4.P** | **Dynamic content pipeline complete** | -- | 4.1-4.5 | **Animation + instancing work** |

### Phase 5: Material Mapping (Weeks 13-14)

**Goal:** Godot materials appear correctly in RealityKit.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 5.1 | Complete StandardMaterial3D → PhysicallyBasedMaterial mapping (all properties from §2.13 table). | 3 | 2.5 | Full PBR mapping |
| 5.2 | Implement transparency/alpha modes (opaque, transparent, alpha scissor). | 1 | 5.1 | Transparency works |
| 5.3 | Implement unlit materials (UnlitMaterial in RealityKit). | 1 | 5.1 | Unlit works |
| 5.4 | Test material accuracy — compare Godot editor preview to RealityKit render for 20 material variations. Document differences. | 2 | 5.1-5.3 | Material fidelity documented |
| 5.5 | **Proof B** (§2.20): Score all 20 test materials using 8-axis rubric (§2.23). Run texture orientation/channel audit (§2.29) first. Side-by-side screenshots with score cards. | 2 | 5.4 | Proof B report with scored rubric |
| **5.P** | **Material mapping complete + Proof B passed** | -- | 5.1-5.5 | **Materials classified, coverage known** |

### Phase 6: Replacement Shaders (Weeks 15-16)

**Goal:** Custom shader annotation system works. Starter shader library available.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 6.1 | Implement `@rk_shader` annotation parser — read shader metadata from Godot .gdshader files at export/load time. | 2 | 5.P | Annotations parsed |
| 6.2 | Implement shader registry — map annotation names to Swift ShaderGraphCoder implementations. | 2 | 6.1 | Registry works |
| 6.3 | Create starter shaders: ToonMaterial, GlassMaterial, HologramMaterial, WaterMaterial using ShaderGraphCoder. | 3 | 6.2 | 4 replacement shaders |
| 6.4 | Implement export-time warning for unmapped custom shaders — log and fall back to default PBR. | 1 | 6.2 | Warnings work |
| 6.5 | Document shader replacement workflow for developers. | 1 | 6.3 | Documentation |
| **6.P** | **Replacement shader system works** | -- | 6.1-6.5 | **Custom shaders supported** |

### Phase 7: Input + Interaction (Weeks 17-18)

**Goal:** Spatial input (eye gaze + pinch tap, drag gestures) fully forwarded from RealityKit to Godot. RealityKit is the sole input authority (§2.30) — eye gaze ray is system-private and inaccessible to Godot.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 7.1 | Auto-generate `InputTargetComponent` + `CollisionComponent` on RealityKit entities from Godot mesh data. Default: convex hull. Configurable per-entity. | 2 | 2.P | Entities are tappable via eye gaze + pinch |
| 7.2 | Implement tap callback bridge — `SpatialTapGesture` → `godot_avp_tap_callback` with entity_id, hit_position (entity-local). **Verify hit_normal availability** — public docs confirm position but not surface normal in gesture payload. If normal unavailable, document limitation and remove from bridge signature. Forward as `spatial_tap` signal in Godot. | 2 | 7.1 | Tap events reach GDScript; hit_normal availability confirmed or documented as unavailable |
| 7.3 | Implement drag callback bridge — `DragGesture` → `godot_avp_drag_callback` with entity_id, phase (began/changed/ended/cancelled), hit_position, translation3D. Forward as `spatial_drag_*` signals. | 2 | 7.1 | Drag events reach GDScript with 3D translation |
| 7.4 | Implement hover styling (presentation-only) — add `HoverEffectComponent` to entities that should visually highlight on eye gaze. **No callback to Godot** — Apple says apps cannot receive hover events. Hover is RealityKit-side visual effect only. | 0.5 | 7.1 | Entities highlight on gaze (visual only) |
| 7.5 | Implement collision shape strategy (§2.30) with priority order: (1) Godot CollisionShape3D extraction if available, (2) convex hull fallback, (3) bounding box last resort, (4) exact mesh opt-in. Developer can override via `@rk_collision` annotation or property. | 2 | 7.1 | Collision shape priority order works |
| 7.6 | Implement and test hierarchical `InputTargetComponent` routing (§2.30). Build corpus scene C13. Run 3-part verification per tap: (1) correct RealityKit entity targeted, (2) correct Godot node receives signal, (3) hit position in correct coordinate space. Test both flat and grouped modes. | 2 | 7.1-7.2 | 3-part input routing proof passes |
| 7.7 | Test: corpus scene C6 (100 entities with collision). Verify tap latency <100ms, correct entity identification, drag 3D translation accuracy. Test with corpus scene C12 (mixed real-world) and C13 (hierarchical input). | 1 | 7.1-7.6 | Interaction validated on hardware |
| **7.P** | **Spatial input works — tap + drag passthrough from RealityKit, hover presentation-only** | -- | 7.1-7.7 | **Interaction complete** |

### Phase 8: Testing + Polish (Weeks 19-22)

**Goal:** Full test suite. Production-quality code. Documentation.

| # | Task | Days | Dependencies | Deliverable |
|---|------|------|--------------|-------------|
| 8.1 | Unit tests: bridge calls, entity sync, mesh/texture/material stores, volume camera culling. | 3 | 7.P | 50+ unit tests |
| 8.2 | Integration tests: load 10 Godot demo scenes, verify visual correctness in RealityKit. | 3 | 7.P | 10 scenes validated |
| 8.3 | Performance tests: run content-class benchmark matrix (§3.2) on Vision Pro hardware. Record pass/fail for each content class. | 3 | 8.2 | Benchmark matrix report |
| 8.4 | **Memory-floor test** (§2.28): load/unload scenes C1, C3, C4 in sequence. Verify RSS returns to baseline ±30MB. Verify all stores empty. | 1 | 8.2 | Memory-floor report |
| 8.5 | **ID stability audit** (§2.27): run corpus scene C16 (RID torture test) + C5 (create/destroy storm). Verify no stale resurrection, no leaked entities, no duplicate bridge calls. | 1 | 8.2 | ID stability report |
| 8.6 | **Lifecycle abuse tests** (Proof E, §2.20): view create/destroy churn, suspend/resume, immersive↔bounded transitions, volume resize, hot scene reload. | 2 | 8.4 | Lifecycle abuse report |
| 8.7 | Memory profiling: verify no leaks in entity pool, mesh/texture stores, bridge buffers via Instruments. | 1 | 8.4, 8.5 | No leaks |
| 8.8 | Code cleanup: Swift code style, C++ Godot code style (clang-format), copyright headers. | 2 | 8.1-8.7 | Style compliant |
| 8.9 | User documentation: "Exporting Godot Projects for Apple Vision Pro." Include authoring contract (§2.31). | 2 | 8.8 | Docs complete |
| 8.10 | Create demo: spatial 3D game running on Vision Pro, with volume camera, gestures, and floating HUD. | 2 | 8.3 | Demo video |
| 8.11 | Complete Proof E (§2.20): aggregate lifecycle abuse + benchmark matrix + memory-floor results. | 1 | 8.3, 8.6 | Proof E report |
| 8.12 | Update GIP with results and benchmark data. PR description. | 1 | 8.8-8.11 | GIP updated |
| 8.13 | **Submit upstream PR** (ONLY if Proofs A-E all pass AND GIP accepted). | 1 | 8.12 | **PR SUBMITTED** (or out-of-tree plugin) |

---

## 7. MILESTONES

### M0: RealityKit Spike (End of Week 1)

| Criterion | Target |
|-----------|--------|
| RealityView displays a ModelEntity | Visual |
| LowLevelMesh creates runtime geometry | Visual |
| LowLevelTexture creates runtime texture | Visual |
| 100 entities at 90 FPS on simulator | Measured |
| Tested on physical Vision Pro | Hardware test |
| **Test corpus frozen** (§2.22): 13 scenes (3 hostile) committed to `tests/avp_corpus/` | VCS commit hash |

**Kill criteria:** Same as Phase 0 kill criteria (see §6 Phase 0). All five criteria measured on physical Vision Pro hardware, not simulator.

---

### M1: Bridge Prototype (End of Week 3)

| Criterion | Target |
|-----------|--------|
| `extern "C"` bridge compiles and links | Build test |
| EntitySynchronizer creates/updates/destroys entities | Visual |
| MeshStore caches LowLevelMesh resources | Unit test |
| TextureStore caches LowLevelTexture resources | Unit test |
| MaterialStore builds PhysicallyBasedMaterial from descriptor | Visual |
| Batched transform update works (single call, 50 entities) | Unit test |
| Fake entity stream at 90 FPS | Measured |

---

### M2: Godot Integration (End of Week 7)

| Criterion | Target |
|-----------|--------|
| `RasterizerAVP` compiles with `make_current()` | Build test |
| `render_scene()` override captures geometry instances | Debug log |
| `RenderStateExtractor` diffs and emits create/update/destroy | Unit test |
| Mesh data transfers from Godot to RealityKit | Visual |
| Material properties transfer correctly | Visual |
| Texture data transfers correctly | Visual |
| Simple Godot scene (3 meshes) renders in RealityKit | Screenshot |
| **Proof A passed** (§2.20): Data completeness validated for target scene classes | Report |
| **Proof C passed** (§2.20): End-to-end frame budget measured on hardware | Instruments trace |
| **Proof D passed** (§2.20): Fork invasiveness below threshold | LOC count |
| GIP filed (ONLY if Proofs A/C/D pass) | Link |

**Kill criteria (concrete — any one kills the renderer-hook approach):**

1. **Geometry list incomplete:** `RenderGeometryInstance` data is missing or null for >5% of visible objects in a test scene with meshes, lights, and particles when using the dummy/AVP backend (Proof A)
2. **Material data unresolvable:** Material parameters (albedo, roughness, metallic, textures) cannot be read from `MaterialStorage` without duplicating major renderer storage initialization logic (>500 LOC of renderer-specific setup)
3. **Culling depends on GPU:** Frustum culling or visibility determination requires GPU-side occlusion query data that is absent in the dummy path, causing >20% of visible objects to be incorrectly culled or not culled
4. **Thread-affinity trap:** MainActor constraints force >2ms of cross-thread synchronization per frame for a 200-entity scene (measured via Instruments, Proof C)
5. **Fork invasiveness:** The AVP backend touches >15 core renderer files (outside `drivers/avp/` and `platform/visionos/`) OR requires >200 LOC of changes to files shared with other backends

**Cross-scene consistency rule:** Criteria 1-3 are tested against AT LEAST corpus scenes C1, C3, C8, and C12 (covering static, skinned, hierarchical, and mixed content). **If any criterion triggers on two or more corpus scenes, fail M2 immediately.** A single-scene failure triggers investigation; a two-scene failure proves systemic unsuitability.

Specifically, fail M2 immediately if ANY of the following occur on two different corpus scenes for two consecutive frames:
- Extracted geometry set differs from native Godot visible set by more than the threshold
- Material identity cannot be resolved without backend-specific storage spelunking
- Skeleton data needs renderer-private state not available in the extraction layer
- Culling correctness depends on data only produced by the real render backend

**Hostile scene classification requirement:** At M2, the project must run ALL THREE hostile corpus scenes (C9, C10, C11) through the bridge and produce an honest classification for each:

| Scene | Expected Outcome | What Must Be Documented |
|-------|-----------------|------------------------|
| C9 (light-dependent) | Geometry correct, lighting wrong | Specific visual differences. Whether ambient-only result is acceptable per §2.25. |
| C10 (custom shader) | Fallback to default PBR | Which shaders fell back, which produced warnings, which silently failed. |
| C11 (particles/billboards) | Missing or degraded | Which features are absent vs. degraded. Whether degradation is documented in §2.17. |

**A hostile scene cannot be "skipped" at M2.** If the bridge crashes on a hostile scene, that's a kill signal (bridge is fragile). If the bridge produces silent wrong output without logging a warning, that's a Proof A failure (extraction is lying). The ONLY acceptable outcome is: correct output, OR documented degradation that matches what §2.17/§2.18 predicted.

**Plain-language preservation requirement:** For each hostile scene, the M2 report must include a plain-language classification of every visual feature in the scene, using exactly three categories:

| Category | Meaning | Example |
|----------|---------|---------|
| **PRESERVED** | Feature renders correctly on RealityKit | Geometry, PBR materials, transforms |
| **APPROXIMATED** | Feature renders differently but acceptably | Godot point light → RealityKit IBL (different but lit) |
| **LOST** | Feature is absent or visually broken | Particle system not rendered, custom shader shows default gray |

**If any feature in a hostile scene cannot be classified into one of these three categories with a one-sentence explanation, M2 fails immediately.** "Something rendered" is not a classification. The renderer-hook path receives geometry alongside lights, probes, voxel GI, decals, lightmaps, fog volumes, environment, and shadow data — the hostile scenes exist specifically to test what happens to features the bridge does NOT forward. Every such feature must be explicitly accounted for.

**If criteria 1-4 trigger:** Evaluate GDExtension-only approach (read scene tree via SwiftGodot, bypass renderer entirely). If that also fails, kill the project.
**If criterion 5 triggers:** Redesign as out-of-tree GDExtension plugin (not upstream-able). Continue only if standalone plugin is acceptable.

---

### M3: Volume Camera (End of Week 9)

| Criterion | Target |
|-----------|--------|
| SpatialVolumeCamera node works in Godot editor | Editor test |
| Bounded mode shows content in visionOS volume | Visual |
| Immersive mode shows content in immersive space | Visual |
| Layer mask filtering works | Visual |
| 100 objects, 50 in volume, 90 FPS | Measured |
| **Lighting acceptance gate signed off** (see below) | Decision-log entry |

**Lighting acceptance gate (required before M3 exits):**

The bridge does not forward Godot lights (§2.25). Before M3, the project must sign off on one of these product positions and record it in the Decision Log:

| Option | Meaning | Consequence |
|--------|---------|-------------|
| **A: Lighting parity is a goal** | Project will invest in forwarding Godot light data to RealityKit (add ~4 weeks) | Scope expansion. Re-estimate timeline. Investigate `PointLightComponent`, `DirectionalLightComponent`, `SpotLightComponent` in RealityKit. |
| **B: Lighting approximation accepted** | RealityKit IBL/environment lighting is "good enough." Develop authoring guidance for lighting-independent content. | Lighting-dependent Godot scenes look different. Documented in authoring contract. No additional work. |
| **C: Lighting-dependent scenes out of scope** | The bridge explicitly targets content that does not depend on Godot lighting. | Narrower target. Must be reflected in scope-collapse tracking (§10). |

**Default (if not explicitly decided): Option B.** But this must be a conscious decision, not a default-by-neglect. The decision must be accompanied by a review of corpus scene C9 (hostile, light-dependent) showing the actual visual impact.

---

### M4: Dynamic Content (End of Week 12)

| Criterion | Target |
|-----------|--------|
| Skinned meshes animate correctly (⚠️ RED RISK — see §2.18) | Visual |
| MultiMesh → MeshInstancesComponent instancing works | Visual |
| LOD transitions work | Visual |
| Skinned character benchmark: measure max characters at 90 FPS on hardware | Report (target: 20, acceptable: 5) |
| If <5 skinned characters at 90 FPS: document limitation, evaluate RealityKit-native skeleton fallback | Decision |

---

### M5: Materials Complete (End of Week 14)

| Criterion | Target |
|-----------|--------|
| All StandardMaterial3D properties map correctly | Visual comparison |
| Transparency/alpha modes work | Visual |
| **Proof B passed** (§2.20): 20 materials classified into exact/perceptual/degraded/broken | Report with side-by-side screenshots |
| Material fidelity documented for 20 material variations | Report |

**Gate:** If Proof B shows >30% of materials in "degraded" or "broken" buckets, material mapping strategy must be revised before M6.

---

### M6: Replacement Shaders (End of Week 16)

| Criterion | Target |
|-----------|--------|
| `@rk_shader` annotations parse correctly | Unit test |
| 4 starter replacement shaders work | Visual |
| Unmapped shaders fall back to PBR with warning | Log test |

---

### M7: Spatial Input (End of Week 18)

| Criterion | Target |
|-----------|--------|
| Tap gesture fires Godot InputEvent | Event test |
| Drag gesture updates Godot node position | Visual |
| Eye tracking hover highlights entities | Visual |
| Grab-and-move works with physics | Visual |

---

### M8: Production Release (End of Week 22)

| Criterion | Target |
|-----------|--------|
| 50+ unit tests pass | Report |
| 10 demo scenes render correctly | Screenshots |
| Content-class benchmark matrix passed (see §3.2) | Report |
| No memory leaks | Instruments |
| Code style compliant | CI |
| Documentation complete | Doc PR |
| Demo video created | Link |
| **Proof E passed** (§2.20): Real content validation with diverse scene types | Report with traces |
| **Upstream PR submitted** (ONLY if all Proofs A-E pass and GIP accepted) | **PR URL** |

**Gate:** Upstream PR is NOT submitted unless Proofs A-E are all passed AND the GIP has been accepted by Godot platform maintainers. If GIP is rejected or pending, the project ships as an out-of-tree plugin.

---

## 8. TIMELINE SUMMARY

```
Week  1      ████████  Phase 0: RealityKit Spike                → M0 ★
Week  2-3    ████████████████  Phase 1: Bridge Prototype         → M1 ★
Week  4-7    ████████████████████████████████  Phase 2: Godot    → M2 ★ (+ GIP filed)
Week  8-9    ████████████████  Phase 3: Volume Camera            → M3 ★
Week 10-12   ████████████████████████  Phase 4: Mesh + Animation → M4 ★
Week 13-14   ████████████████  Phase 5: Material Mapping         → M5 ★
Week 15-16   ████████████████  Phase 6: Replacement Shaders      → M6 ★
Week 17-18   ████████████████  Phase 7: Input + Interaction      → M7 ★
Week 19-22   ████████████████████████████████  Phase 8: Test+PR  → M8 ★ PR SUBMITTED
```

**Total Duration**: 20-22 weeks (one senior developer)
**Team Size**: 1 senior developer (Godot internals + Swift + RealityKit + visionOS experience)
**Hardware Required**: Apple Vision Pro device, Mac with Apple Silicon (Xcode, visionOS SDK)

---

## 9. DECISION LOG

### v1.0 decisions

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 1 | Scene model | Flat entity updates (no hierarchy replication) | Godot already computes final transforms; duplicating hierarchy is waste |
| 2 | Bridge protocol | Entity state updates, NOT draw commands | RealityKit is a retained-mode renderer; command streams are wrong abstraction |
| 3 | Bridge mechanism | `extern "C"` (overhead UNKNOWN — requires Proof C) | Direct C++ interop has virtual function bugs; `extern "C"` is battle-tested |
| 4 | Mesh API | LowLevelMesh (visionOS 2.0+) | Only API that supports runtime mesh creation with custom layouts |
| 5 | Texture API | LowLevelTexture (visionOS 2.0+) | Runtime texture creation with GPU updates |
| 6 | Material strategy | Tier 1 (auto PBR) + Tier 3 (explicit replacement) | No Tier 2 (auto shader conversion) — too unreliable |
| 7 | Shader tool | ShaderGraphCoder (759 nodes) | CustomMaterial not available on visionOS; ShaderGraphCoder is the path |
| 8 | Threading | Cooperative main-thread ticking (initially) | GodotVision proved this works; avoids synchronization complexity |
| 9 | Volume camera | Modeled on Unity PolySpatial VolumeCamera | Proven concept; bounded + immersive modes match visionOS window types |
| 10 | Godot renderer | Subclass RendererSceneRender (render_scene override) | Access to culled geometry list with final transforms; no GPU allocation |
| 11 | 2D support | OUT OF SCOPE for v1.0 (see decision #19) | 2D pipeline is architecturally separate; v1.0 targets 3D spatial content only |
| 12 | UI | SwiftUI + RealityKit Attachments | Native visionOS UI; no Control node mirroring |
| 13 | Instancing | MeshInstancesComponent (WWDC 2025) | Maps directly to Godot MultiMesh |
| 14 | Platform base | Extend existing visionOS platform from Godot 4.5 PR #105628 | Don't duplicate platform scaffolding |
| 15 | Minimum visionOS | visionOS 2.0 | LowLevelMesh/LowLevelTexture require visionOS 2.0 |
| 16 | GIP timing | Phase 2 (week 7), ONLY after Proofs A/C/D pass | After PoC demonstrates viability AND de-risking evidence exists |
| 17 | Upstream framing | "visionOS Spatial Renderer via RealityKit" | Positions as platform-specific rendering target, not core architecture change |

### v1.1 decisions (adversarial review)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 18 | Lighting ownership | RealityKit owns lighting entirely | Godot light parameters cannot be forwarded meaningfully; RealityKit IBL is the lighting model. Godot lights are NOT forwarded (§2.17) |
| 19 | 2D game support | OUT OF SCOPE for v1.0 | 2D pipeline is architecturally separate; the bridge targets 3D spatial content only (§2.21) |
| 20 | Performance claims | No claims without Proof C measurement | Previous <1ns overhead claim was misleading; system cost is unknown until on-device measurement (§2.9) |
| 21 | Material coverage | No percentage claims without Proof B corpus test | Previous ~90% claim was unsubstantiated; requires 20-material classification study (§2.13) |
| 22 | Skeletal animation | RED RISK — not assumed viable | CPU-side skinning + per-frame mesh re-upload is unproven at scale. May require RealityKit-native skeleton (§2.18) |
| 23 | Kill criteria | Concrete, measurable thresholds | Previous "invasive changes required" was subjective. Replaced with LOC counts, latency thresholds, percentage gates (§6 Phase 0, Phase 2) |
| 24 | Upstream PR timing | BEHIND de-risking gates (Proofs A-E) | PR is not submitted until all proofs pass and GIP is accepted. Project ships as out-of-tree plugin if upstream rejects |
| 25 | Performance validation | Content-class benchmark matrix (10 classes) | Previous "500 entities at 90 FPS" was arbitrary. Replaced with diverse failure-mode scenarios (§3.2) |
| 26 | Camera semantics | Godot projection camera is NOT forwarded | RealityKit controls its own camera. SpatialVolumeCamera defines volume bounds, not view projection (§2.19) |
| 27 | Input model | RealityKit sole authority — eye gaze ray is system-private | Godot CANNOT perform spatial picking. Eye gaze never exposed to apps. Bridge passes through RealityKit gesture data (entity_id, hit_position, drag translation) as new Godot signals. hit_normal is ⚠️ UNPROVEN (§2.30) |

### v1.2 decisions (anti-self-deception)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 28 | Test corpus | Frozen before Phase 1, locked in VCS, 3 hostile scenes | Prevents optimism-driven demo selection. Proofs can't be gamed with toy scenes (§2.22) |
| 29 | Material quality | 8-axis scored rubric, not subjective buckets | "Degraded" was a loophole. Scoring on texture orientation, alpha, normal, metallic/roughness, emissive, camera-dependent, double-sided/cull, depth/transparency ordering makes classification reproducible (§2.23) |
| 30 | Skeletal animation | Hard-cut at first spike: <5 chars at 90 FPS → drop from v1.0 | Prevents the red risk from poisoning the rest of the schedule (§2.24) |
| 31 | Instancing validation | Two-stage: API works AND measurable cost reduction | API existence ≠ renderer cost reduction. Must prove both independently (§2.26) |
| 32 | Lighting consequences | Operationalized with feature table + authoring guidance | "RealityKit owns lighting" was a technical note. Now it's a product decision with content author implications (§2.25) |
| 33 | Line references | Illustrative only until pinned to commit-hash permalinks | Prevents visual overclaiming of certainty (header policy) |
| 34 | Proof A | Includes 5 negative controls (hidden, deleted, hierarchy, override, RID reuse) | Positive tests catch "it works." Negative tests catch "it lies." (§2.20) |
| 35 | Proof C | Requires P50/P95/P99/worst-frame, not averages | MainActor bottlenecks are hitch problems, not throughput problems (§2.20) |
| 36 | Proof D | Includes rebase rehearsal + deleteability review | LOC count alone doesn't prevent touching the worst possible files (§2.20) |
| 37 | Proof E | Includes lifecycle abuse (view churn, suspend, transitions, hot reload) | Integration projects die at SwiftUI lifecycle boundaries (§2.20) |
| 38 | ID stability | Required audit with 5 test cases including RID reuse | Bridge bugs love ID reuse. Must be tested explicitly (§2.27) |
| 39 | Memory floor | Load/unload cycle must return RSS to baseline ±30MB | RealityKit resource cleanup is a known concern (§2.28) |
| 40 | Texture conventions | UV origin, normal handedness, ORM packing, sRGB audited before Proof B | Silent orientation errors contaminate every material classification (§2.29) |
| 41 | Input authority | RealityKit sole authority — not a design choice, a platform constraint | Eye gaze ray is system-private; Godot has no ray to pick with. Cross-validation is impossible (§2.30) |
| 42 | Authoring contract | Explicit supported / limited / unsupported feature lists for content authors | Engineering "passes" are meaningless if content teams produce unsupported scenes (§2.31) |

### v1.3 decisions (scope-collapse-aware)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 43 | Scope-collapse tracking | Explicit 4-tier outcome labels (Tier 1-4) with mandatory assignment at M8 | Prevents silently narrowing from "spatial renderer" to "rigid scene bridge" without acknowledging it (§10) |
| 44 | M2 cross-scene kill | Fail M2 immediately if any kill criterion triggers on 2+ corpus scenes | Single-scene failure is investigation; two-scene failure is systemic (§7 M2) |
| 45 | Input authority correction | RealityKit sole authority for ALL spatial input — cross-validation impossible | v1.2 proposed "Godot cross-validation for gameplay picks" but eye gaze ray is system-private. Godot cannot independently determine what was tapped. Full gesture passthrough instead (§2.30) |
| 46 | Material rubric expansion | 8 axes (added double-sided/cull + depth/transparency ordering) | Materials that pass in screenshots fail in motion on cull/ordering axes (§2.23) |
| 47 | Skeletal hard-cut strengthening | Hostile animated scene + 10-min soak required, not just baseline | Short spikes lie; cache churn and resource fragmentation emerge over time (§2.24) |
| 48 | Instancing Stage 2 threshold | ≥30% improvement in ≥2 of 4 metrics (CPU, GPU, memory, draw cost) | Single cherry-picked metric hides failure elsewhere (§2.26) |
| 49 | Memory-floor threshold | Percentage-based (baseline × 1.15 + buffer) + monotonic growth check | Fixed MB threshold is arbitrary across different scene sizes (§2.28) |
| 50 | Line references in narrative | Stripped from body text; only file/function names remain | Precise-looking line numbers launder uncertainty into authority, even with disclaimer (§2.4, Appendix A) |
| 51 | Cross-frame truth | Proof A includes 4 temporal assertions (create→mutate→delete, rapid churn, same-frame ops, visibility toggle) | Frame-local correctness misses async dispatch reordering and dirty-cache bugs (§2.20 Proof A) |
| 52 | Long-run hitches | Proof C includes 10-minute soak on 3 corpus scenes with trend analysis | 30-second microbenchmarks miss resource fragmentation and GC pressure over time (§2.20 Proof C) |
| 53 | Lighting acceptance gate | Must sign off on Option A/B/C before M3 exits | "RealityKit owns lighting" was a technical note. It defines the product. (§7 M3) |

### v1.4 decisions (hover removal, input routing, API gates)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 54 | Hover bridge | REMOVED — impossible | Apple explicitly says apps cannot receive hover event callbacks for RealityKit entities. Eye gaze hover state is private. `HoverEffectComponent` provides visual styling only (§2.30) |
| 55 | Hover handling | Presentation-only via `HoverEffectComponent` on RealityKit side | No callback to Godot. Developers who need "what is the user looking at?" must design around tap/drag confirmation, not gaze tracking |
| 56 | Hierarchical InputTargetComponent | Default: flat model (each entity gets own InputTargetComponent). Grouped input opt-in only | Avoids entity ID routing ambiguity. Parent/child sharing creates divergence between what RealityKit reports and which Godot node receives the event (§2.30) |
| 57 | Hierarchical input proof | Corpus scene C13 + explicit test in Phase 7.6 | Parent/child input routing must be tested — silent misrouting is a class of bug that casual testing misses (§2.22, §2.30) |
| 58 | MeshInstancesComponent availability | ⚠️ Available in visionOS 26, NOT visionOS 2.0 (plan's minimum target) | Default Option B: `@available` check, fallback to individual entities on older devices. Must be confirmed at M0 (§2.26) |
| 59 | ShaderGraphCoder | Third-party library, not platform parity | Real and substantial but introduces dependency risk and capability constraints. Some Godot shader limitations are platform constraints, not engineering backlog (§2.13) |
| 60 | M2 hostile scene classification | All 3 hostile scenes (C9, C10, C11) must be run and classified at M2 | Cannot skip hostile scenes. Crash = kill signal. Silent wrong output = Proof A failure. Only acceptable: correct output or documented degradation matching §2.17 predictions (§7 M2) |

### v1.5 decisions (input proof tightening, collision defaults, dual-path reporting)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 61 | C13 input proof | 3-part verification: (1) correct RealityKit entity targeted, (2) correct Godot node receives signal, (3) hit position in correct coordinate space | "Tap arrives" is not proof — grouped input can silently misroute while appearing to work. Apple's EntityTargetValue docs describe spatial data conversion between gesture location and entity (§2.30) |
| 62 | Hit normal | ⚠️ UNPROVEN — downgraded from planned to prototype-verified | Public docs confirm entity-targeted gestures and coordinate conversion (EntityTargetValue.position) but do not clearly establish surface normal availability in gesture payload. Bridge signature includes it provisionally; prototype must confirm or remove (§2.30) |
| 63 | Collision shape default | Priority: (1) Godot CollisionShape3D if available, (2) convex hull, (3) bounding box last resort | Convex-hull-by-default creates false positives in gameplay-heavy scenes where pick region drifts from Godot-authored one. Godot CollisionShape3D preserves authoring intent (§2.30) |
| 64 | Instancing benchmark | Dual-path reporting required: visionOS 26+ path AND visionOS 2.x fallback path side-by-side | Cannot produce one nice graph on newer path and understate the real shipping baseline on the older one. Both results in Proof E report (§3.2) |
| 65 | M2 hostile classification | Plain-language PRESERVED/APPROXIMATED/LOST classification required for every visual feature in every hostile scene | "Something rendered" is not a classification. Every feature the bridge does NOT forward must be explicitly accounted for (§7 M2) |

---

## 10. SUCCESS CRITERIA AND SCOPE-COLLAPSE TRACKING

### Outcome tiers (explicit labels for what was actually proven)

The project can "succeed" at different scope levels. Each level must be explicitly named, not silently collapsed into. **A plan that narrows from "Godot spatial renderer" to "rigid PBR scene bridge with many exclusions" has failed its own honesty standard UNLESS it explicitly accepts the narrower label.**

| Tier | Label | What Was Proven | Criteria |
|------|-------|----------------|----------|
| **Tier 1** | **General-purpose Godot spatial renderer** | Most Godot 3D content renders correctly on Vision Pro with acceptable fidelity. Skinned animation works. Materials ≥70% exact/perceptual. ≥8 content-class benchmarks pass. Input works. | All Proofs A-E pass. ≤30% materials degraded. Skinned animation passes hard-cut at ≥10 characters. Instancing provides measurable benefit (Stage 2). Upstream PR accepted. |
| **Tier 2** | **Constrained Godot spatial renderer** | Static/rigid 3D content renders correctly. Skinned animation limited or cut. Some material types degraded. Working but with documented exclusions. | Proofs A/C/D pass. Proof B shows >30% degraded but ≤60%. Skinned animation hard-cut triggered at 5-9 characters OR dropped. ≥5 content-class benchmarks pass. Viable as plugin. |
| **Tier 3** | **Rigid PBR scene bridge** | Only StandardMaterial3D + static meshes + basic transforms work. No animation, limited materials, no custom shaders functional, no instancing benefit. | Proofs A/C/D pass. Proof B shows >60% degraded. Skeletal animation dropped. ≤4 content-class benchmarks pass. Instancing Stage 2 fails. Project is a tech demo, not a product. |
| **Tier 4** | **Failed — architecture not viable** | Renderer-hook approach does not work. GDExtension fallback also failed. | Any M0 or M2 kill criterion triggers AND fallback fails. |

**At M8, the project MUST assign itself to a tier.** The tier assignment must be published alongside the Proof E report. It is not acceptable to claim Tier 1 while Tier 2 or Tier 3 evidence exists.

**Scope-collapse tracking:** At each milestone (M2, M4, M5, M8), record the current projected tier in the decision log. If the projected tier drops between milestones (e.g., from Tier 1 to Tier 2), document why and whether it's acceptable.

### Full success (Tier 1 — all of):

1. A Godot 3D scene renders spatially on Apple Vision Pro via RealityKit
2. VolumeCamera controls bounded/immersive presentation mode
3. **Proof B passed:** 20 materials scored on 8-axis rubric (§2.23), ≤30% degraded (score 6-10), ≤10% broken (score 0-5)
4. **Content-class benchmark matrix:** ≥8 of 10 content classes pass on Vision Pro hardware (§3.2)
5. Spatial input (tap, drag) routes to GDScript via bridge signals; hover is presentation-only via HoverEffectComponent (§2.30)
6. Custom shader replacement system works with 4 starter shaders
7. Zero GPU resources allocated by Godot's own renderer
8. **All Proofs A-E passed** (§2.20)
9. Upstream PR submitted with GIP, documentation, tests, and demo

### Partial success (Tier 2 — viable product, not general-purpose):

Items 1-2 and 5-7 achieved, but one or more of:
- GIP rejected by platform maintainers → ship as out-of-tree GDExtension plugin
- Proof D fails (fork too invasive) → works but can't be upstreamed
- ≥5 of 10 content classes pass but <8 → viable with documented limitations
- Skeletal animation hard-cut triggered (limited or dropped)
- Material degradation >30% but ≤60%

**This outcome is explicitly acceptable IF labeled as Tier 2 and the authoring contract reflects the constraints.**

### Minimal success (Tier 3 — tech demo, not product):

Core rendering works for simple content, but:
- Skeletal animation dropped from scope
- >60% of materials degraded or broken
- <5 content-class benchmarks pass
- Instancing provides no measurable benefit

**This outcome must be honestly labeled as Tier 3.** It proves the architecture is directionally correct but not yet productizable. The project may continue as a research prototype.

### Kill criteria (Tier 4 — stop the project):

- **M0 (Week 1):** Any of the 5 Phase 0 kill criteria triggers (§6). RealityKit cannot sustain the entity model.
- **M2 (Week 7):** Any of the 5 Phase 2 kill criteria triggers (§6) AND GDExtension fallback also fails. Render state extraction is not viable from either path.
- **M5 (Week 14):** Proof B shows >60% of materials in degraded/broken AND no improvement path is identified. PBR mapping is fundamentally incorrect.
- **M8 (Week 22):** <3 of 10 content-class benchmarks pass. The architecture cannot support even limited real content.
- **At any time:** If Godot core team signals that no visionOS spatial renderer will be accepted in any form, evaluate whether out-of-tree plugin justifies continued investment.

---

## 11. ASSET PIPELINE (Future)

### Phase 1 (In Scope): Runtime Streaming

- Meshes: extract from Godot `MeshStorage`, create via LowLevelMesh at runtime
- Textures: extract pixel data, create via LowLevelTexture at runtime
- Materials: extract parameters, build PhysicallyBasedMaterial at runtime

### Phase 2 (Future): USDZ Export

RealityKit loads USDZ natively (`developer.apple.com/visionos/get-started/`). A future optimization:

```
Static meshes → USDZ files (exported at build time)
Dynamic transforms → streamed at runtime
```

This would dramatically reduce runtime mesh creation cost for static geometry. Static props, environment meshes, and level geometry could be pre-baked as USDZ. Only dynamic objects (characters, physics objects, particles) would use the runtime streaming pipeline.

---

## Appendix A: Source Code References (Godot 4.6-stable) — ILLUSTRATIVE ONLY

**⚠️ All line numbers in this appendix are from a snapshot read of the Godot 4.6-stable source tree. They are NOT pinned to a specific commit hash. Before forking, every reference must be revalidated and replaced with commit-hash permalink URLs.**

### A.1 3D Rendering Pipeline

| Symbol | File | Line | Purpose |
|--------|------|------|---------|
| `RendererSceneCull` | `servers/rendering/renderer_scene_cull.h` | 1-130 | 3D scene culling class |
| `Instance` (data structure) | `renderer_scene_cull.h` | 401-616 | Per-instance data: transform, base, materials, visibility |
| `InstanceCullResult` | `renderer_scene_cull.h` | 882-1000 | Culling output: geometry_instances, lights, probes |
| `_scene_cull()` | `renderer_scene_cull.cpp` | 2816-3050 | Frustum/visibility/occlusion culling |
| geometry push to result | `renderer_scene_cull.cpp` | 3031 | `cull_result.geometry_instances.push_back(idata.instance_geometry)` |
| `render_scene()` handoff | `renderer_scene_cull.cpp` | 3487 | Calls `scene_render->render_scene(cull_result.geometry_instances, ...)` |

### A.2 Instance Transform Flow

| Operation | File | Line | Content |
|-----------|------|------|---------|
| `instance_set_transform()` | `renderer_scene_cull.cpp` | 979 | Stores transform, queues AABB update |
| `_instance_queue_update()` | `renderer_scene_cull.cpp` | 4127 | Adds to dirty list |
| `_update_dirty_instance()` | `renderer_scene_cull.cpp` | 3929 | AABB recompute + instance update |
| `_update_instance()` | `renderer_scene_cull.cpp` | 1596-1730 | World AABB, calls `geom->geometry_instance->set_transform()` at line 1720 |

### A.3 RenderGeometryInstance

| Symbol | File | Line | Fields |
|--------|------|------|--------|
| `RenderGeometryInstance` (interface) | `renderer_geometry_instance.h` | 39-75 | Pure virtual: set_transform, set_materials, set_skeleton, etc. |
| `RenderGeometryInstanceBase` (base) | `renderer_geometry_instance.h` | 78-154 | transform, mesh_instance, Data.base, Data.surface_materials[], layer_mask, lod_bias |

### A.4 RendererSceneRender

| Symbol | File | Line | Purpose |
|--------|------|------|---------|
| `render_scene()` virtual | `renderer_scene_render.h` | 322 | Entry point: receives PagedArray of geometry + lights + probes |
| `geometry_instance_create()` | `renderer_scene_render.h` | 55 | Factory for geometry instances |
| `CameraData` struct | `renderer_scene_render.h` | 301-320 | View transform, projection, visible layers |

### A.5 RenderingServer

| Symbol | File | Line | Purpose |
|--------|------|------|---------|
| `instance_create()` | `rendering_server.h` | 1454 | Create 3D instance |
| `instance_set_base()` | `rendering_server.h` | 1456 | Set mesh/light/particles base |
| `instance_set_transform()` | `rendering_server.h` | 1460 | Set world transform |
| `instance_set_visible()` | `rendering_server.h` | 1464 | Set visibility |
| `create_func` pointer | `rendering_server.cpp` | 40-46 | Singleton factory |

### A.6 Mesh/Material/Texture Storage

| Symbol | File | Line | Purpose |
|--------|------|------|---------|
| `mesh_allocate()` | `storage/mesh_storage.h` | 42 | Allocate mesh RID |
| `mesh_add_surface()` | `storage/mesh_storage.h` | 49 | Add surface with vertex/index data |
| `mesh_get_surface()` | `storage/mesh_storage.h` | 64 | Retrieve surface data |
| `material_allocate()` | `storage/material_storage.h` | 78 | Allocate material RID |
| `material_set_param()` | `storage/material_storage.h` | 85 | Set shader parameter |
| `material_get_param()` | `storage/material_storage.h` | 86 | Get shader parameter |

### A.7 Dummy Renderer (Headless Template)

| Symbol | File | Line | Purpose |
|--------|------|------|---------|
| `RasterizerDummy` | `dummy/rasterizer_dummy.h` | 49-110 | Full no-op compositor |
| `RasterizerSceneDummy` | `dummy/rasterizer_scene_dummy.h` | 38 | No-op scene renderer |
| `render_scene()` no-op | `rasterizer_scene_dummy.h` | 155 | Empty `{}` — no GPU work |
| `GeometryInstanceDummy` | `rasterizer_scene_dummy.h` | 40-75 | All setters empty |
| `make_current()` | `rasterizer_dummy.h` | 106-109 | Sets `_create_func` |

### A.8 RendererCompositor Factory

| Symbol | File | Line | Purpose |
|--------|------|------|---------|
| `_create_func` pointer | `renderer_compositor.h` | 72 | `static RendererCompositor *(*_create_func)()` |
| `create()` factory | `renderer_compositor.h` | 77 | Calls `_create_func()` |
| `get_scene()` accessor | `renderer_compositor.h` | 87 | Returns `RendererSceneRender*` |

---

## Appendix B: RealityKit API Reference

| API | URL | Used For |
|-----|-----|----------|
| RealityView | `developer.apple.com/documentation/RealityKit/RealityView` | SwiftUI 3D content hosting |
| LowLevelMesh | `developer.apple.com/documentation/realitykit/lowlevelmesh` | Runtime mesh creation |
| LowLevelTexture | `developer.apple.com/documentation/realitykit/lowleveltexture` | Runtime texture creation |
| PhysicallyBasedMaterial | `developer.apple.com/documentation/realitykit/physicallybasedmaterial` | PBR material mapping |
| CustomMaterial | `developer.apple.com/documentation/realitykit/custommaterial` | Metal shaders (iOS/macOS only, NOT visionOS) |
| RealityViewAttachments | `developer.apple.com/documentation/realitykit/realityviewattachments` | SwiftUI views in 3D space |
| ShaderGraphCoder | `github.com/praeclarum/ShaderGraphCoder` | Programmatic shader graphs for visionOS |
| MeshInstancesComponent | `developer.apple.com/videos/play/wwdc2025/287/` | GPU instancing (WWDC 2025) |
| Unity PolySpatial VolumeCamera | `docs.unity3d.com/Packages/com.unity.polyspatial.visionos/manual/VolumeCamera.html` | Reference architecture |
| Godot RenderingServer | `docs.godotengine.org/en/stable/classes/class_renderingserver.html` | Godot's rendering API |
| GodotVision | `github.com/kevinw/GodotVision` | Prior art (archived) |
| SwiftGodot | `github.com/migueldeicaza/SwiftGodot` | Swift GDExtension bindings |
| Godot 4.5 visionOS PR | `github.com/godotengine/godot/pull/105628` | Existing platform support |

---

*v1.5 — adversarially reviewed, scope-collapse-aware. 65 decisions. Incorporates Godot 4.6-stable rendering pipeline analysis (line refs stripped from body — see header policy), RealityKit API documentation (LowLevelMesh, PhysicallyBasedMaterial, ShaderGraphCoder, MeshInstancesComponent), C++→Swift bridge research (overhead UNKNOWN — requires Proof C), and Unity PolySpatial architectural reference. Key architecture: flat render entity model, extern "C" bridge, Tier 1/Tier 3 shader strategy, VolumeCamera, cooperative main-thread ticking, RealityKit owns lighting, RealityKit sole input authority (eye gaze ray is system-private — Godot cannot pick). v1.5 changes: C13 input proof strengthened to 3-part verification (entity target + Godot receiver + coordinate space — Apple's EntityTargetValue describes spatial data conversion that must be validated), hit_normal downgraded to ⚠️ UNPROVEN (public docs confirm position but not surface normal in gesture payload), collision shape default reordered to prefer Godot CollisionShape3D → convex hull → bounding box (preserves authoring intent, avoids false positives), dual-path instancing benchmark reporting required (visionOS 26+ path vs visionOS 2.x fallback — cannot present only the newer path as shipping baseline), M2 hostile scene classification tightened to require plain-language PRESERVED/APPROXIMATED/LOST classification per visual feature. Honest gaps: 14 visual features NOT forwarded (§2.17), 3 red-risk features (§2.18), 2D out of scope (§2.21), hover events impossible (§2.30), hit normal unproven (§2.30). Anti-gaming: frozen hostile test corpus with mandatory M2 PRESERVED/APPROXIMATED/LOST classification (§2.22), 8-axis scored material rubric (§2.23), skeletal hard-cut with 10-min soak (§2.24), lighting acceptance gate at M3 (§7), two-stage instancing validation requiring ≥2 metrics with API availability gate and dual-path reporting (§2.26, §3.2), ID stability audit with cross-frame truth assertions (§2.27, §2.20), percentage-based memory-floor with monotonic growth check (§2.28), texture convention audit (§2.29), RealityKit-sole input authority with 3-part hierarchical routing proof and Godot-CollisionShape3D-preferred collision defaults (§2.30), authoring contract (§2.31). Proofs: Proof A has 5 negative controls + 4 cross-frame assertions, Proof C requires P50/P95/P99/worst-frame + 10-min long-run soak, Proof D includes rebase rehearsal + deleteability, Proof E includes lifecycle abuse + dual-path instancing benchmarks. Scope-collapse tracking: 4-tier outcome labels (§10) — project must honestly assign itself to Tier 1 (general-purpose), Tier 2 (constrained), Tier 3 (tech demo), or Tier 4 (failed) at M8. Total: 20-22 weeks. Risk gated by 5 mandatory proofs, concrete kill criteria at M0/M2/M5/M8, cross-scene consistency checks, mandatory hostile scene classification with per-feature accounting, and dual-path instancing reporting. Upstream PR gated behind all proofs + GIP acceptance. This project may conclude successfully by proving only a constrained subset is viable — that counts as success only if explicitly accepted.*
