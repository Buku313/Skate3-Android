#pragma once

// Authoritative character bone-palette serve.
//
// The game packs each skinned character's bone palette into
// cModelInstance.m_matrices (48-byte 3x4 rows) right before uploading it to
// the vertex shader. This module snapshots those rows the moment the game
// finishes packing them, per instance, per piece, one sim tick, coherent by
// construction, and serves them to the scene builder in place of the
// draw-time constant-bank captures, which can be stale or belong to another
// character (multi-pass draws, shadow casters, shared banks).
//
// Snapshot sources (hooked at function exit, when m_matrices is settled):
//  - cModelInstance::PackAndMultiplyMatricesForUpload: most characters.
//  - Sk8::UpdateBoneTransforms: the player skater's per-piece part records
//    (a per-frame arena that zeroes by frame end, so the hook exit is the
//    only readable moment). Row layout is confirmed by a per-session vote
//    against fresh bank captures before these rows are ever served.
//  - The LivingWorld batch pack writer: LOD pedestrians (fused
//    UpdateBoneTransforms+Pack; per-mesh slices of one concatenated buffer).
//
// Items are matched to snapshots by MeshContext containment in the owning
// instance's m_arrMeshContext window, refined by the ctx -> entity identity
// store (skate3_native_entity.h) and joint-proximity/bone-count content
// checks. Unmapped items keep the capture pipeline's palette unchanged.

#include <cstddef>
#include <cstdint>

namespace skate3::native_scene {
struct FrameScene;
}  // namespace skate3::native_scene

namespace skate3::native_palette {

// cModelInstance::PackAndMultiplyMatricesForUpload EXIT: m_matrices (+0x14)
// holds exactly the packed upload palette; snapshot it.
void OnPackPalette(uint8_t* base, uint32_t instance);

// Sk8::UpdateBoneTransforms EXIT: parts = an ARRAY of cModelInstance part
// records (stride 0x28), count = record count. Snapshots every part
// (per-frame arena, unreadable by frame end).
void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count);

// LivingWorld batch pack writer EXIT: entity = the
// cLivingWorldPresEntity-derived writer. Snapshots each instance's
// m_matrices as per-mesh slices.
void OnLwPack(uint8_t* base, uint32_t entity);

// Pack-owner bracket (thread-local): the hook layer wraps every entity
// method that produces palette packs; set the owner on entry, restore the
// returned previous value after the original call. Snapshots taken inside
// the bracket carry the owning entity for identity-exact serving.
uint32_t ExchangePackOwner(uint32_t entity);

// Called from BuildFrameScene while the frame's items are finalized:
// replaces each mapped skinned character item's captured bank palette with
// its instance's own packed rows. Served items get dbg_src=11 and
// caster_bank=false. Returns the number of items served.
uint32_t ServeAuthoritativePalettes(uint8_t* base,
                                    skate3::native_scene::FrameScene& scene);

// Called from BuildFrameScene at the frame tail: rotates this frame's
// snapshots into the previous-frame slot (draws of frame N may consume the
// palette packed during frame N-1) and emits the optional stats line.
void OnFrameBuilt();

}  // namespace skate3::native_palette
