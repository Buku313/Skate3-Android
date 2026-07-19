#pragma once

// Shared sample-vertex decode + skin + spread helpers for the native scene
// renderer: the one copy of the decode+skin loop shared by the palette and
// coherence gates (RefinePaletteBase, ScoreRigidAffine, RopaPayloadCoherent,
// PublishedPaletteSane, the InterpolateDynamicItems centroid pass).
//
// Decode conventions unified here:
//  - guest u8x4 blend attribs are big-endian per 32-bit word: influence k is
//    guest byte k, i.e. byte (24 - 8k) of the host-order load;
//  - position fmt 57 = float3, 32 = half4 (xyz), 26 = s16x4 with scale
//    2/32767 and the +0.8 Y bias.

#include <bit>
#include <cstddef>
#include <cstdint>

#include "skate3_native_scene.h"

namespace skate3::native_scene {

// One decoded vertex: bind-space position + u8x4 blend attribs (influence k
// = w[k]/bone[k]; all zero when the item carries no blend attributes).
struct SkinSampleVert {
  float p[3];
  uint8_t w[4];
  uint8_t bone[4];
  bool pos_finite;  // every |p| < 1e7 (mid-sim-write garbage detection)
};

// SEH-guarded bulk copy of guest memory (host pointers; callers pass
// base + guest_addr). Returns false when the range faults (streaming
// decommit, bogus candidate pointer). ANY guarded guest read must go
// through this function; see the compiler-trap comment at the definition:
// open-coded __try{memcpy}__except compiles to an UNPROTECTED `jmp memcpy`.
bool GuestTryCopy(void* dst, const void* src, size_t size);

float GuestHalfToFloat(uint16_t h);

// Bind-pose bbox diagonal of the item: the reference size every
// spread-vs-bind coherence gate compares against.
float BindDiag(const DrawItem& item);

// Decode vertex `vtx` (index) of the item's LIVE guest VB. Blend attribs are
// read only when both offsets are non-zero. Returns false on an unsupported
// position format.
bool ReadSkinVertGuest(uint8_t* base, const DrawItem& item, uint32_t vtx,
                       SkinSampleVert* out);

// Decode `n` evenly spaced sample verts (index s*(count-1)/(n-1)) of the
// item's LIVE guest VB. Returns false when the position format is
// unsupported or the VB holds fewer than 2 vertices.
bool ReadSkinSamplesGuest(uint8_t* base, const DrawItem& item, uint32_t n,
                          SkinSampleVert* out);

// Weighted [R | t] skin with HOST palette rows (DrawItem::bones layout:
// 3 float4 rows per bone, column-vector affine). Influences whose rows fall
// outside the palette are skipped. Returns the total u8 weight (0 = nothing
// weighted); q is the weight-normalized skinned position when total > 0.
uint32_t SkinPointHostRows(const SkinSampleVert& sv, const float* rows,
                           size_t rows_floats, float q[3]);

// Weighted skin with the palette staged in a guest VS constant bank at base
// register pb (3 rows per bone). Influences past register 256 are skipped.
// *rows_sane is CLEARED when any weighted rotation row's xyz norm falls
// outside [0.04, 25]; world positions in rotation slots can still project
// on-screen and pass the geometric gates.
uint32_t SkinPointBankRows(uint8_t* base, uint32_t bank, uint32_t pb,
                           const SkinSampleVert& sv, float q[3], bool* rows_sane);

// Spread (bbox diagonal) of the decoded samples skinned with host palette
// rows: the shared core of the three coherence gates. Samples with a
// non-finite position either fail the gate (garbage_fails, the frame-end
// ropa check: mid-sim-write garbage means incoherent) or are skipped (the
// publish-time gate). Returns:
//   -1 = garbage position encountered (garbage_fails only),
//    0 = fewer than min_n weighted samples (nothing to judge),
//    1 = *out_spread measured.
// *out_n (optional) receives the weighted-sample count.
int SkinnedSpreadHostRows(const SkinSampleVert* sv, uint32_t n, const float* rows,
                          size_t rows_floats, int min_n, bool garbage_fails,
                          float* out_spread, int* out_n = nullptr);

}  // namespace skate3::native_scene
