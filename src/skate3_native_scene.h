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
  uint16_t pos_offset;
  uint8_t stride;
  uint8_t pos_fmt;    // xenos vertex format (26 s16x4, 32 half4, 57 float3)
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

// Called on the guest render thread at frame end with the frame's records.
void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count);

// Registers the native guest output renderer with the SDK. Safe to call once
// at startup regardless of cvar state.
void Install();

}  // namespace skate3::native_scene
