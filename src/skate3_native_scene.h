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
  uint32_t vb_obj;    // guest renderengine::VertexBuffer object (draw matching)
  uint32_t ib_obj;    // guest renderengine::IndexBuffer object (draw matching)
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
  uint16_t normal_offset;  // usage-3 vertex normal (0 fmt = none)
  uint8_t stride;
  uint8_t pos_fmt;    // xenos vertex format (26 s16x4, 32 half4, 57 float3)
  uint8_t uv_fmt;     // xenos vertex format of the first texcoord (0 = none)
  uint8_t uv2_fmt;    // xenos vertex format of the second texcoord (0 = none)
  uint8_t normal_fmt;  // 16 = k_10_11_11 packed (0 = none, face-normal fallback)
  // sky.* materials draw fullbright: per-facet lighting turns the multi-km
  // sky dome into visibly shaded rectangular panels.
  bool unlit;
  bool skinned;
  // Grayscale-tinted material (CAS hair): the shader multiplies the diffuse
  // by a per-character color staged in the PIXEL constant bank. Detected via
  // the AttribulatorMaterialName channel; tint captured with the palette.
  bool hair;
  float tint[4];  // rgb + enable flag in w
  // The item's per-draw state (bone palette for skinned, world matrix for
  // rigid model-space props) was not available at capture time; deferred
  // meshes draw AFTER the submit call, and world sort-list captures happen
  // before any draw. Cleared by the post-draw (ib,vb) fixup; items still
  // pending at frame end are dropped (wrong state renders garbage).
  bool pending;
  // Cloth patch (non-indexed quad list, CPU-simulated absolute world-space
  // float3 verts): the renderer synthesizes quad->triangle indices instead
  // of reading a guest index buffer.
  bool cloth_quads;
  // Bone palette snapshot taken on the game thread: raw staged rows, 3
  // float4s per bone (column-vector affine [R | t], model -> world). The
  // guest staging bank is reused draw to draw, hence the copy.
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

// Called from the D3D::SetPending_AluConstants hook (guest render thread,
// invoked from inside the Draw* functions). Records the location of the
// device's positional 256-register VS constant shadow bank.
void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device);

// Guest buffer-binding trackers + post-draw state fixup: each
// DrawIndexedVertices whose bound IB/VB match a pending item fills that
// item's bone palette / world matrix from the constant bank (FIFO one-shot
// per buffer key; clones share mesh assets and draw in submit order).
void OnSetIndices(uint32_t ib_obj);
void OnSetStreamSource(uint32_t stream, uint32_t vb_obj, uint32_t offset, uint32_t stride);
// func: 0 = DrawIndexedVertices (also runs the pending fixup),
// 1 = DrawVertices, 2 = BeginVertices (inline/cloth vertex path).
void OnDrawDone(uint8_t* base, uint32_t func, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7);

// Offline-analysis recording, armed alongside the guest-memory snapshot:
// captures every draw's bound buffers + full VS constant bank, plus the
// per-frame captured items and final scene, every `stride` frames. Written
// as <stem>.scene.jsonl and <stem>.draws.bin next to the .gsnap.
void StartRecording(uint32_t stride);
void WriteRecording(const char* dir, const char* stem);

// Called post-call from the DrawVertices hook for cloth patch draws
// (r6 == 0x80000000 signature): reads the bound dynamic buffer's vertex
// fetch block and builds a world-space quad-list item. Returns a
// dynamic-state index (+1), 0 when the draw is not a cloth patch;
// *out_key receives the garment's stable identity (the buffer object).
uint32_t CaptureClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6,
                          uint32_t r7, uint32_t* out_key);

// Total guest draws completed (any draw function). The RenderMesh hook
// samples this around the original call to detect deferred meshes: if no
// draw ran inside, the constant bank still belongs to an earlier mesh and
// the item's transform/palette must come from the post-draw fixup.
uint64_t DrawSequence();

// Called from the RenderMesh hook (dynamic entities) and the world per-mesh
// draw hook (sub_827FAEA8), AFTER the original call returns (the mesh's
// constants are only flushed to the shadow bank by its own draws). Builds
// the complete DrawItem: geometry from the transient per-frame arena plus
// world matrix / bone palette read live from the shadow bank. drew_inside
// says whether any guest draw ran during the original call; when false the
// bank is stale and the transform is left pending for the post-draw fixup.
// With world_path, bails out early for meshes that need no per-draw
// transform (absolute fmt-57 geometry, handled at frame end) and captures
// only skinned meshes and rigid model-space props (vending machines and
// other movables reach the frame ONLY through the sort lists). Returns a
// dynamic-state index (+1), 0 on failure/skip.
uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx, bool world_path = false,
                             bool drew_inside = false);

// Called on the guest render thread at frame end with the frame's records.
void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count);

// Registers the native guest output renderer with the SDK. Safe to call once
// at startup regardless of cvar state.
void Install();

}  // namespace skate3::native_scene
