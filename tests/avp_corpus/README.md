# AVP Bridge Locked Test Corpus

**Status**: FROZEN — No changes without Decision Log entry (see RK_PLAN.md §9)

This corpus is the anti-gaming measure described in RK_PLAN.md §2.22.
All scenes are defined and committed BEFORE Phase 1 implementation begins.

## Rules

1. **Freeze before implementation.** This corpus is committed before Phase 1.
2. **No replacement without decision-log entry.** A scene can only be removed or
   replaced if the reason is documented in the Decision Log (§9) with date and rationale.
3. **Three scenes are intentionally hostile.** Marked `[HOSTILE]`.

## Corpus Scenes

| # | Directory | Content Class | Purpose | Hostile? |
|---|-----------|--------------|---------|----------|
| C1 | `C01_static_showroom` | 50 static meshes, 20 materials, 5 textures | Baseline — does the bridge work at all? | No |
| C2 | `C02_material_zoo` | 20 objects, distinct StandardMaterial3D variations | Proof B — material fidelity classification | No |
| C3 | `C03_skinned_parade` | 10 skinned characters with animation | Skinned mesh streaming proof (§2.18 red risk) | No |
| C4 | `C04_multimesh_field` | 1 MultiMesh with 2000 instances | Instancing path validation | No |
| C5 | `C05_create_destroy_storm` | Script spawns/destroys 50 entities/sec | Churn, entity pool, ID stability | No |
| C6 | `C06_input_gauntlet` | 100 entities with collision shapes | Input routing + interaction overhead | No |
| C7 | `C07_volume_transitions` | Bounded↔immersive toggle every 5 sec | Volume camera + SwiftUI lifecycle | No |
| C8 | `C08_deep_hierarchy` | 10 levels of parent-child nesting | Validates flat model transform correctness | No |
| C9 | `C09_hostile_light_dependent` | Scene dependent on Godot lights/shadows | **[HOSTILE]** Tests lighting gap — EXPECTED wrong | Yes |
| C10 | `C10_hostile_custom_shader` | 8 objects with custom .gdshader, no @rk_shader | **[HOSTILE]** Tests unmapped shader fallback | Yes |
| C11 | `C11_hostile_particle_billboard` | GPUParticles3D, billboards, sprites | **[HOSTILE]** Tests unsupported features | Yes |
| C12 | `C12_mixed_real_world` | 30 static + 5 skinned + MultiMesh + lights + HUD | Closest to a real game — "honest demo" | No |
| C13 | `C13_hierarchical_input` | Parent InputTarget + 5 child CollisionComponents | 3-part input routing proof (§2.30) | No |

### Optional (encouraged)

| # | Directory | Purpose |
|---|-----------|---------|
| C14 | `C14_texture_stress` | 50 objects with unique 1024² textures |
| C15 | `C15_lod_scene` | Objects at varying distances with LOD levels |
| C16 | `C16_recycled_rid_torture` | Rapid create/destroy to force RID reuse |

## Pass Criteria Per Scene

Each scene has TWO possible pass outcomes:

- **CORRECT:** Scene renders correctly on Vision Pro hardware.
- **DOCUMENTED DEGRADATION:** Scene renders incorrectly, but degradation is documented,
  expected, and matches what §2.17 predicts. No crash, no leak, no contamination.

A scene **FAILS** if:
- Bridge crashes or hangs
- Memory grows without bound
- Silent visual corruption in OTHER entities (contamination)
- Degradation occurs but is NOT predicted by §2.17 or §2.18

## Proof Assignments

| Proof | Corpus Scenes Used |
|-------|-------------------|
| Proof A (Extraction Truthfulness) | C1, C8, C11, C16 |
| Proof B (Visual Parity Buckets) | C2 (primary) + others |
| Proof C (Frame Budget) | C1, C3, C5 |
| Proof D (Fork Invasiveness) | N/A (code audit) |
| Proof E (Failure Envelopes) | C5, C7, C12, C16 |
