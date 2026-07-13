#pragma once

// LivingWorld entity store: the game's own
// per-instance identity + opacity for cLivingWorldPresEntity-derived
// entities (census pedestrians, traffic vehicles, spawned props), served to
// the native scene the way the console serves it.
//
// Write point: the sub_827C1188 (cLivingWorldPresEntity::Update) EXIT hook,
// per entity per sim tick: the instant the game finishes writing the
// entity's evaluated opacity Vector4 at entity+528 (x = alpha, the value
// BindConstants pushes into every mesh ctx's material params, which the
// character-family shaders output as alpha: c13.x / c21.x / c22.x / c20.x).
// The walk mirrors sub_827C1720 (BindConstants): instance array at
// [e+(idx+8)*4] stride 0x28, per-instance MeshContext array at +4 stride
// 0x50; each ctx IS a DrawItem::ctx, so lookups are exact per instance
// per mesh. Entities whose layout fails a guard simply never enter the
// store (self-limiting: they keep the captured-row fade path).

#include <cstdint>

namespace skate3::native_lw {

// sub_827C1188 EXIT (sim thread): read the entity's fresh opacity and map
// it onto every MeshContext the entity binds. Cheap (a dozen guarded loads
// per entity); called only while the native renderer is enabled.
void OnLwEntityTick(uint8_t* base, uint32_t entity);

// Fresh (<= 500 ms) store entry for a MeshContext. Returns false when the
// ctx is unknown or stale (despawned entity / reused arena memory). Guest
// render thread; internally locked: use the bulk pattern (one call per
// item per frame is fine, the map is small).
bool LookupLwCtx(uint32_t ctx, float* out_alpha, uint32_t* out_entity);

// The entity's OWN packed palette slice for this ctx's mesh, snapshotted at
// the entity's last sim tick (the LivingWorld pack writer sub_827C1D38
// writes the concatenated per-mesh 4x3 palettes into the instance's
// m_matrices; mesh k's slice starts at 3*sum(preceding bone counts), the
// same walk the live-proven SnapshotLwEntity shadow reader uses). Copies up
// to max_floats host-order floats (3 float4 rows per bone) into out and
// returns the bone-row count, or 0 when the ctx has no fresh slice. This is
// the authoritative replacement for GUESSED ortho caster-bank palettes on
// edge-of-view vehicles.
uint32_t LookupLwPalette(uint32_t ctx, float* out, uint32_t max_floats);

// Store size counters for the periodic stats line: live ctx entries and
// distinct entities ticked in the last second.
void QueryLwStats(uint32_t* out_ctxs, uint32_t* out_entities);

}  // namespace skate3::native_lw
