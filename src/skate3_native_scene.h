#pragma once

// Native scene renderer (data-driven): consumes the per-frame MeshContext
// stream collected by skate3_native_render.cpp, walks the verified guest
// structures on the game thread into a
// compact NativeFrameScene, and renders it with our own shaders into the
// guest output texture via the SDK native-guest-output hook, replacing the
// emulated frame when active.

#include <cstdint>
#include <vector>

namespace skate3::native_scene {

struct DrawEntry {
  uint32_t prim;
  uint32_t base_vertex;
  uint32_t start_index;
  uint32_t index_count;
};

struct DrawItem {
  uint32_t mesh;      // guest mesh object address (resource cache key)
  uint32_t vb_addr;   // guest address of raw vertex data
  uint32_t ib_addr;   // guest address of raw index data (u16 big-endian)
  uint32_t vb_bytes;
  uint32_t ib_count;
  uint32_t diffuse_tex;   // guest renderengine::Texture address (0 = none)
  uint32_t lightmap_tex;  // guest renderengine::Texture address (0 = none)
  uint16_t pos_offset;
  uint16_t uv_offset;
  uint16_t uv2_offset;
  uint16_t bw_offset;  // blend weights (u8x4), valid when skinned
  uint16_t bi_offset;  // blend indices (u8x4), valid when skinned
  uint8_t stride;
  uint8_t pos_fmt;    // xenos vertex format (26 s16x4, 32 half4, 57 float3)
  uint8_t uv_fmt;     // xenos vertex format of the first texcoord (0 = none)
  uint8_t uv2_fmt;    // xenos vertex format of the second texcoord (0 = none)
  bool skinned;
  // Bone palette snapshot (row-vector 4x4 matrices) taken on the game thread
  // - the guest bone array lives in a transient per-frame arena.
  std::vector<float> bones;
  // Content fingerprint of the guest payloads. Streaming reuses arena
  // addresses and pages fill in after the mesh is first submitted, so cached
  // GPU copies must be revalidated against this every frame.
  uint64_t fingerprint;
  float world[16];    // row-vector convention, translation in row 3
  float bbox_min[3];  // mesh-local bounds, for decode validation
  float bbox_max[3];
  std::vector<DrawEntry> draws;
};

struct FrameScene {
  uint64_t generation = 0;
  float view_proj[16] = {};
  float cam_pos[3] = {};
  std::vector<DrawItem> items;
};

// One frame submission record from the hook layer (see RenderMeshRecord).
struct SubmitRecord {
  uint32_t kind;
  uint32_t a;
  uint32_t b;
  uint32_t c;
};

bool Enabled();

// Called from the cProcessArenaAsset::RegisterTexture hook: guid -> guest
// renderengine::Texture object.
void OnRegisterTexture(uint64_t guid, uint32_t texture);

// Called from the D3D::SetPending_AluConstants hook (guest render thread).
// Large vertex-bank uploads are bone palettes staged for the next skinned
// RenderMesh; the staging bank is reused, so contents are snapshotted here.
void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr);

// Called from the RenderMesh hook for dynamic entities. Snapshots the
// context's instance matrix (ctx+0x10 points into a transient per-frame
// arena; it must be read now, not at frame end) and the pending bone
// palette. Returns a dynamic-state index (+1), 0 on failure.
uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx);

// Called on the guest render thread at frame end with the frame's records.
void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count);

// Registers the native guest output renderer with the SDK. Safe to call once
// at startup regardless of cvar state.
void Install();

}  // namespace skate3::native_scene
