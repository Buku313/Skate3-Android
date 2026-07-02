#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

#if defined(REX_HAS_D3D12) && REX_HAS_D3D12
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <d3dcompiler.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#endif

REXCVAR_DEFINE_BOOL(skate3_native_render_scene, false, "Skate 3",
                    "Render the game scene natively from the hooked MeshContext stream, "
                    "replacing the emulated GPU output (requires skate3_native_render)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lightmaps, false, "Skate 3",
                    "Sample guest lightmap textures in the native scene renderer. Off by "
                    "default: Skate 3 lightpages are composed at runtime on the GPU, so "
                    "their CPU-side payloads decode black.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_debug, 0, "Skate 3",
                     "Native scene debug: 0=normal, 1=clear only, 2=solid color per item, "
                     "3=limit to 20 items, 4=depth test disabled")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_scene {
namespace {

// Verified guest structure offsets.
constexpr uint32_t kCtxMatrix = 0x10;
constexpr uint32_t kCtxDrawCountU16 = 0x38;
constexpr uint32_t kCtxDrawList = 0x48;
constexpr uint32_t kMeshMaterial = 0x24;
constexpr uint32_t kMeshVertexDescriptor = 0x28;
constexpr uint32_t kMeshIndexBuffer = 0x30;
constexpr uint32_t kMeshVertexBuffer = 0x34;
constexpr uint32_t kBufferPhysAddr = 0x18;
constexpr uint32_t kVbBytes = 0x20;
constexpr uint32_t kIbCount = 0x20;
constexpr uint32_t kViewCameraFromView = 0x08;
constexpr uint32_t kViewCamViewProj = 0xA0;

std::mutex g_scene_mutex;
FrameScene g_scene;
uint64_t g_generation = 0;
std::atomic<uint8_t*> g_guest_base{nullptr};
std::atomic<uint64_t> g_frames_rendered{0};

// guid -> guest renderengine::Texture, from the RegisterTexture hook. Keys
// masked of the top bit: material channel guids carry an extra flag bit
// relative to the registered cAssetIDs.
constexpr uint64_t kGuidMask = 0x7FFFFFFFFFFFFFFFull;
std::mutex g_texture_map_mutex;
std::unordered_map<uint64_t, uint32_t> g_texture_map;

// Bone palette snapshots. The guest uploads palettes as large VS constant
// kicks (4x3 matrices, 3 float4 rows per bone, translation at floats 3/7/11)
// right before the skinned RenderMesh calls that use them; the staging bank
// is reused so the data is copied at hook time. "Sticky": one character's
// palette serves all of its body-part contexts.
struct PaletteSnapshot {
  std::vector<float> rows;  // raw staged float4 rows (LE)
};
// Per-dynamic-entity state captured at RenderMesh hook time: the instance
// matrix (already transposed into row-vector convention) and the palette in
// effect. Both live in transient arenas/banks and are stale by frame end.
struct DynamicState {
  float world[16];
  uint32_t palette_index;  // +1 into g_frame_palettes, 0 = none
};
std::mutex g_palette_mutex;
std::vector<PaletteSnapshot> g_frame_palettes;
std::vector<DynamicState> g_frame_dynstates;
PaletteSnapshot g_pending_palette;
uint32_t g_pending_palette_index = 0;  // index+1 into g_frame_palettes, 0 = none
std::atomic<uint64_t> g_vs_uploads{0};
std::atomic<uint64_t> g_vs_uploads_large{0};
std::atomic<uint64_t> g_palette_snapshots{0};
std::atomic<uint64_t> g_skinned_items{0};
std::atomic<uint64_t> g_skinned_skipped{0};

bool SceneEnabled() { return REXCVAR_GET(skate3_native_render_scene); }

float LoadGuestF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  return std::bit_cast<float>(bits);
}

bool GuestReadableApprox(uint8_t* base, uint32_t addr) {
  // The hook layer only walks pointers the game is actively rendering from;
  // they are mapped. Reject null/small.
  (void)base;
  return addr >= 0x10000;
}

// Walk one MeshContext into a DrawItem. Returns false if any pointer in the
// chain is implausible. dyn: hook-time state snapshot for dynamic entities
// (instance matrix + bone palette; both live in transient arenas and are
// stale by frame end). World geometry passes null and renders with identity
// (absolute coordinates).
bool BuildItem(uint8_t* base, uint32_t ctx, const DynamicState* dyn,
               const PaletteSnapshot* palette, DrawItem& item) {
  const uint32_t record = REX_LOAD_U32(ctx);
  if (!GuestReadableApprox(base, record)) return false;
  const uint32_t mesh = REX_LOAD_U32(record);
  if (!GuestReadableApprox(base, mesh)) return false;

  const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
  const uint32_t ib = REX_LOAD_U32(mesh + kMeshIndexBuffer);
  const uint32_t vb = REX_LOAD_U32(mesh + kMeshVertexBuffer);
  if (!GuestReadableApprox(base, vdesc) || !GuestReadableApprox(base, ib) ||
      !GuestReadableApprox(base, vb)) {
    return false;
  }

  // Vertex descriptor: find the stream-0 position element and the first
  // stream-0 texcoord (D3DDECLUSAGE 5) for the diffuse map.
  const uint32_t num_elements = REX_LOAD_U16(vdesc + 8);
  if (num_elements == 0 || num_elements > 32) return false;
  bool have_pos = false;
  bool have_bw = false;
  bool have_bi = false;
  item.uv_offset = 0;
  item.uv_fmt = 0;
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.skinned = false;
  for (uint32_t i = 0; i < num_elements; ++i) {
    const uint32_t e = vdesc + 0x10 + i * 16;
    const uint32_t stream = REX_LOAD_U16(e);
    const uint32_t usage = REX_LOAD_U8(e + 9);
    if (stream != 0) continue;
    if (usage == 0 && !have_pos) {
      item.pos_offset = REX_LOAD_U16(e + 2);
      item.pos_fmt = uint8_t(REX_LOAD_U32(e + 4) & 0x3F);
      have_pos = true;
    } else if (usage == 5 && item.uv_fmt == 0) {
      item.uv_offset = REX_LOAD_U16(e + 2);
      item.uv_fmt = uint8_t(REX_LOAD_U32(e + 4) & 0x3F);
    } else if (usage == 5 && item.uv2_fmt == 0) {
      item.uv2_offset = REX_LOAD_U16(e + 2);
      item.uv2_fmt = uint8_t(REX_LOAD_U32(e + 4) & 0x3F);
    } else if (usage == 1 && !have_bw) {  // blend weights u8x4
      item.bw_offset = REX_LOAD_U16(e + 2);
      have_bw = (REX_LOAD_U32(e + 4) & 0x3F) == 6;
    } else if (usage == 2 && !have_bi) {  // blend indices u8x4
      item.bi_offset = REX_LOAD_U16(e + 2);
      have_bi = (REX_LOAD_U32(e + 4) & 0x3F) == 6;
    }
  }
  if (!have_pos) return false;
  item.skinned = have_bw && have_bi;
  item.stride = REX_LOAD_U8(vdesc + (num_elements + 1) * 16);
  if (item.stride == 0) return false;

  // Mesh BBox = two Vector4s at +0x00/+0x10. Reject NaN/absurd bounds.
  for (int axis = 0; axis < 3; ++axis) {
    const float lo = LoadGuestF32(base, mesh + axis * 4);
    const float hi = LoadGuestF32(base, mesh + 0x10 + axis * 4);
    if (!(hi - lo < 50000.0f)) {
      return false;
    }
    item.bbox_min[axis] = lo;
    item.bbox_max[axis] = hi;
  }

  item.mesh = mesh;
  item.vb_addr = REX_LOAD_U32(vb + kBufferPhysAddr) & 0xFFFFFFFC;
  item.vb_bytes = REX_LOAD_U32(vb + kVbBytes);
  item.ib_addr = REX_LOAD_U32(ib + kBufferPhysAddr) & 0xFFFFFFFC;
  item.ib_count = REX_LOAD_U32(ib + kIbCount);
  if (item.vb_addr == 0 || item.ib_addr == 0 || item.vb_bytes == 0 ||
      item.ib_count == 0 || item.vb_bytes % item.stride != 0) {
    return false;
  }

  // Culled island draw list from the context.
  const uint32_t draw_count = REX_LOAD_U16(ctx + kCtxDrawCountU16);
  const uint32_t draw_list = REX_LOAD_U32(ctx + kCtxDrawList);
  if (draw_count == 0 || draw_count > 512 || !GuestReadableApprox(base, draw_list)) {
    return false;
  }
  item.draws.reserve(draw_count);
  for (uint32_t i = 0; i < draw_count; ++i) {
    const uint32_t d = draw_list + i * 16;
    DrawEntry entry{REX_LOAD_U32(d), REX_LOAD_U32(d + 4), REX_LOAD_U32(d + 8),
                    REX_LOAD_U32(d + 12)};
    if (entry.index_count == 0 || entry.index_count > item.ib_count) continue;
    item.draws.push_back(entry);
  }
  if (item.draws.empty()) return false;

  // Material channels: resolve the "diffuse" and "lightmap" texture guids to
  // registered texture objects.
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  const uint32_t material = REX_LOAD_U32(mesh + kMeshMaterial);
  if (GuestReadableApprox(base, material)) {
    const uint32_t num_channels = REX_LOAD_U32(material);
    const uint32_t channels = REX_LOAD_U32(material + 8);
    if (num_channels <= 32 && GuestReadableApprox(base, channels)) {
      for (uint32_t i = 0; i < num_channels; ++i) {
        const uint32_t chan = channels + i * 0x20;
        const uint32_t name = REX_LOAD_U32(chan);
        if (!GuestReadableApprox(base, name)) continue;
        char text[10] = {};
        for (int k = 0; k < 9; ++k) {
          text[k] = char(REX_LOAD_U8(name + k));
          if (text[k] == '\0') break;
        }
        uint32_t* slot = nullptr;
        if (std::memcmp(text, "diffuse", 8) == 0) {
          slot = &item.diffuse_tex;
        } else if (std::memcmp(text, "lightmap", 9) == 0) {
          slot = &item.lightmap_tex;
        }
        if (slot != nullptr && *slot == 0) {
          const uint64_t guid =
              (uint64_t(REX_LOAD_U32(chan + 0x10)) << 32) | REX_LOAD_U32(chan + 0x14);
          std::lock_guard<std::mutex> lock(g_texture_map_mutex);
          auto it = g_texture_map.find(guid & kGuidMask);
          if (it != g_texture_map.end()) {
            *slot = it->second;
          }
        }
        if (item.diffuse_tex != 0 && item.lightmap_tex != 0) {
          break;
        }
      }
    }
  }

  // Payload fingerprint (FNV-1a over bytes sampled across the whole VB/IB)
  // so the renderer re-decodes when streaming replaces or fills in the data
  // at this address, including middle-of-buffer fills.
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h = (h ^ v) * 1099511628211ull;
  };
  mix(item.vb_addr);
  mix(item.ib_addr);
  mix(item.vb_bytes);
  mix(item.ib_count);
  if (item.vb_bytes >= 8 && item.ib_count >= 4) {
    for (uint32_t k = 0; k < 16; ++k) {
      const uint32_t vb_off = uint32_t(uint64_t(item.vb_bytes - 8) * k / 15u) & ~7u;
      mix(REX_LOAD_U64(item.vb_addr + vb_off));
      const uint32_t ib_off = uint32_t(uint64_t(item.ib_count * 2 - 8) * k / 15u) & ~7u;
      mix(REX_LOAD_U64(item.ib_addr + ib_off));
    }
  }
  item.fingerprint = h;

  // Transform (identity for world geometry, instance matrix for props;
  // characters get their bone array's first matrix, which roughly places
  // them until skinning is implemented).
  if (dyn != nullptr) {
    std::memcpy(item.world, dyn->world, sizeof(item.world));
    if (item.skinned && palette != nullptr && !palette->rows.empty()) {
      // Convert the staged 4x3 palette (3 float4 rows per bone, column-vector
      // affine) into 4x4 row-vector matrices. The palette maps model space to
      // world space directly, so world stays identity.
      const uint32_t num_bones = uint32_t(palette->rows.size() / 12);
      item.bones.resize(size_t(num_bones) * 16);
      for (uint32_t b = 0; b < num_bones; ++b) {
        const float* m = palette->rows.data() + size_t(b) * 12;
        float* out = item.bones.data() + size_t(b) * 16;
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            out[i * 4 + j] = m[j * 4 + i];
          }
          out[i * 4 + 3] = 0.0f;
        }
        out[12] = m[3];
        out[13] = m[7];
        out[14] = m[11];
        out[15] = 1.0f;
      }
      std::memset(item.world, 0, sizeof(item.world));
      item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
      g_skinned_items.fetch_add(1, std::memory_order_relaxed);
    } else if (item.skinned) {
      // A skinned mesh without its palette renders as bind-pose limbs
      // sprawling from the model origin; skip it instead.
      g_skinned_skipped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  } else {
    std::memset(item.world, 0, sizeof(item.world));
    item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
    item.skinned = false;
  }
  return true;
}

}  // namespace

bool Enabled() { return SceneEnabled(); }

void OnRegisterTexture(uint64_t guid, uint32_t texture) {
  if (texture == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_texture_map_mutex);
  g_texture_map[guid & kGuidMask] = texture;
}

void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr) {
  if (!SceneEnabled() || bank != 0x4000 || ptr == 0) {
    return;
  }
  g_vs_uploads.fetch_add(1, std::memory_order_relaxed);
  const uint32_t registers = uint32_t(std::popcount(mask)) * 4;
  if (registers < 30) {
    return;
  }
  g_vs_uploads_large.fetch_add(1, std::memory_order_relaxed);

  // ptr is the positional 256-register staging bank (partial uploads leave
  // the rest of the bank intact). The bone palette is a run of consecutive
  // 4x3 affine matrices (3 registers per bone: rotation rows of plausible
  // scale, translation in component 3). Find the longest such run.
  const auto valid_bone = [&](uint32_t reg) -> bool {
    float m[12];
    for (int i = 0; i < 12; ++i) {
      m[i] = LoadGuestF32(base, ptr + (reg * 4 + i) * 4);
      if (!(m[i] > -1e6f && m[i] < 1e6f)) return false;
    }
    for (int r = 0; r < 3; ++r) {
      const float n =
          m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
      if (!(n > 0.05f && n < 20.0f)) return false;
      if (!(m[r * 4 + 3] > -20000.f && m[r * 4 + 3] < 20000.f)) return false;
    }
    return true;
  };
  uint32_t best_start = 0;
  uint32_t best_len = 0;
  uint32_t reg = 0;
  while (reg + 3 <= 256) {
    if (!valid_bone(reg)) {
      ++reg;
      continue;
    }
    uint32_t run_start = reg;
    uint32_t run_len = 0;
    while (reg + 3 <= 256 && valid_bone(reg)) {
      ++run_len;
      reg += 3;
    }
    if (run_len > best_len) {
      best_len = run_len;
      best_start = run_start;
    }
  }
  if (best_len < 8) {
    return;
  }
  const uint32_t bones = best_len < 96 ? best_len : 96;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  g_pending_palette.rows.resize(size_t(bones) * 12);
  for (uint32_t i = 0; i < bones * 12; ++i) {
    g_pending_palette.rows[i] = LoadGuestF32(base, ptr + (best_start * 4 + i) * 4);
  }
  g_pending_palette_index = 0;  // not yet published
  g_palette_snapshots.fetch_add(1, std::memory_order_relaxed);
}

uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx) {
  const uint32_t mtx = REX_LOAD_U32(ctx + kCtxMatrix);
  if (mtx == 0 || !GuestReadableApprox(base, mtx)) {
    return 0;
  }
  DynamicState state;
  // Guest matrices store translation in column 3; transpose to row-vector.
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      state.world[r * 4 + c] = LoadGuestF32(base, mtx + (c * 4 + r) * 4);
    }
  }
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  if (!g_pending_palette.rows.empty() && g_pending_palette_index == 0) {
    g_frame_palettes.push_back(g_pending_palette);
    g_pending_palette_index = uint32_t(g_frame_palettes.size());
  }
  state.palette_index = g_pending_palette_index;
  g_frame_dynstates.push_back(state);
  return uint32_t(g_frame_dynstates.size());
}

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  if (!SceneEnabled() || count == 0) {
    return;
  }
  g_guest_base.store(base, std::memory_order_relaxed);

  // Multiple SceneRenderViews can submit per frame (main, shadow cascades,
  // reflections). Pick the perspective one (proj[2][3] == 1 in row-vector
  // convention) and only take its items, deduplicated (the same context can
  // appear in several of the view's sort lists).
  uint32_t view = 0;
  uint32_t viewcam = 0;
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    if (r.kind != 1 || r.c == 0) {
      continue;
    }
    const uint32_t cam = REX_LOAD_U32(r.c + kViewCameraFromView);
    if (!GuestReadableApprox(base, cam)) {
      continue;
    }
    const float persp_w = LoadGuestF32(base, cam + 0x60 + (2 * 4 + 3) * 4);
    if (persp_w == 1.0f) {
      view = r.c;
      viewcam = cam;
      break;
    }
  }
  if (view == 0) {
    return;
  }

  FrameScene scene;
  scene.items.reserve(count);
  std::unordered_set<uint32_t> seen;
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    // Primary opaque list of the chosen view only; other lists (shadow
    // culling, transparents, z-prepass) duplicate the same geometry through
    // different MeshContext objects and z-fight.
    if (r.kind == 1 && (r.c != view || r.b != 20160)) {
      continue;
    }
    if (!seen.insert(r.a).second) {
      continue;
    }
    const DynamicState* dyn = nullptr;
    const PaletteSnapshot* palette = nullptr;
    if (r.kind == 0) {
      if (r.c == 0) {
        continue;  // no hook-time state captured; transform unknown
      }
      std::lock_guard<std::mutex> lock(g_palette_mutex);
      if (r.c <= g_frame_dynstates.size()) {
        dyn = &g_frame_dynstates[r.c - 1];
        if (dyn->palette_index != 0 && dyn->palette_index <= g_frame_palettes.size()) {
          palette = &g_frame_palettes[dyn->palette_index - 1];
        }
      } else {
        continue;
      }
    }
    DrawItem item;
    if (BuildItem(base, r.a, dyn, palette, item)) {
      scene.items.push_back(std::move(item));
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    g_frame_palettes.clear();
    g_frame_dynstates.clear();
    g_pending_palette_index = 0;
  }
  if (scene.items.empty()) {
    return;
  }
  for (int i = 0; i < 16; ++i) {
    scene.view_proj[i] = LoadGuestF32(base, viewcam + kViewCamViewProj + i * 4);
  }
  // Camera position from the view matrix (+0x20, row-vector convention):
  // cam = -t * R^T.
  {
    float view[16];
    for (int i = 0; i < 16; ++i) {
      view[i] = LoadGuestF32(base, viewcam + 0x20 + i * 4);
    }
    for (int j = 0; j < 3; ++j) {
      scene.cam_pos[j] = -(view[12] * view[j * 4 + 0] + view[13] * view[j * 4 + 1] +
                           view[14] * view[j * 4 + 2]);
    }
  }
  // The game's projection uses a negative x scale which already yields
  // correct D3D NDC orientation; use the view*proj matrix as captured.
  // (Negating column 0 here mirrors the image left-right.)

  std::lock_guard<std::mutex> lock(g_scene_mutex);
  scene.generation = ++g_generation;
  g_scene = std::move(scene);
}

}  // namespace skate3::native_scene

#if defined(REX_HAS_D3D12) && REX_HAS_D3D12

namespace skate3::native_scene {
namespace {

using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;
namespace xenos = rex::graphics::xenos;

struct MeshBuffers {
  ID3D12Resource* vb = nullptr;
  ID3D12Resource* ib = nullptr;
  D3D12_VERTEX_BUFFER_VIEW vb_view{};
  D3D12_INDEX_BUFFER_VIEW ib_view{};
  uint64_t fingerprint = 0;
};

struct GuestTexture {
  ID3D12Resource* texture = nullptr;
  ID3D12Resource* upload = nullptr;  // kept alive; copy recorded in deferred list
  uint32_t fetch_words[6] = {};      // big-endian words as read for revalidation
  uint32_t srv_slot = 0;
  // For failed decodes: frame number for periodic retry (payload may stream
  // in after the fetch constant is already valid).
  uint64_t retry_after_frame = 0;
  bool valid = false;
};

// Host format mapping for the formats Skate 3 uses (mirrors the SDK's
// D3D12 texture cache table). host_swizzle remaps guest data components
// before the fetch-constant swizzle composes on top.
struct HostTextureFormat {
  DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
};

bool GetHostTextureFormat(xenos::TextureFormat format, HostTextureFormat& out) {
  switch (rex::graphics::GetBaseFormat(format)) {
    case xenos::TextureFormat::k_DXT1:
      out = {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT2_3:
      out = {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT4_5:
      out = {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_DXT5A:
      out = {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case xenos::TextureFormat::k_DXN:
      out = {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
      out = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case xenos::TextureFormat::k_8:
      out = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case xenos::TextureFormat::k_8_8:
      out = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case xenos::TextureFormat::k_5_6_5:
      // Red/blue swapped CPU-side while uploading.
      out = {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
             xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB};
      return true;
    default:
      return false;
  }
}

UINT ComposeSrvSwizzle(uint32_t fetch_swizzle, uint32_t host_swizzle) {
  uint32_t mapping[4];
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t guest = (fetch_swizzle >> (3 * c)) & 7u;
    mapping[c] = guest >= 4 ? guest : ((host_swizzle >> (3 * guest)) & 7u);
  }
  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2],
                                                 mapping[3]);
}

void SwapGuestEndian(uint8_t* data, uint32_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i + 2 <= size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i + 4 <= size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i + 4 <= size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t size) {
#if defined(_WIN32)
  uint8_t* p = base + addr;
  uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0) {
      return false;
    }
    p = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  (void)base;
  (void)addr;
  (void)size;
  return false;
#endif
}

struct RendererState {
  ID3D12Device* device = nullptr;
  ID3D12RootSignature* root_signature = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12PipelineState* pso_nodepth = nullptr;
  DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  ID3D12DescriptorHeap* dsv_heap = nullptr;
  ID3D12Resource* depth = nullptr;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  ID3D12Resource* rtv_resource = nullptr;
  std::unordered_map<uint32_t, MeshBuffers> meshes;
  // Buffers replaced by re-decode, kept alive until the GPU has finished the
  // submission that last referenced them.
  std::vector<std::pair<ID3D12Resource*, uint64_t>> retired;
  // Texture SRV staging: CPU-only heap; slots copied into the command
  // processor's one-use shader-visible descriptors per draw.
  ID3D12DescriptorHeap* srv_heap = nullptr;
  uint32_t srv_size = 0;
  uint32_t srv_next = 0;
  std::unordered_map<uint32_t, GuestTexture> textures;
  GuestTexture white;
  // Bone palette ring: persistent-mapped upload buffer, one region per
  // in-flight frame.
  static constexpr uint32_t kBoneRegionSize = 1u << 20;
  static constexpr uint32_t kBoneRegions = 4;
  ID3D12Resource* bone_ring = nullptr;
  uint8_t* bone_ring_cpu = nullptr;
  uint32_t bone_ring_offset = 0;
  bool failed = false;
  bool announced = false;
};

RendererState g_r;

// Face-normal shading uses the camera-relative world position: interpolating
// absolute world coordinates (hundreds of meters) destroys ddx/ddy precision
// and produces per-pixel noise.
const char kShaderSource[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 world;
  row_major float4x4 mvp;
  float4 tint;
  float4 cam_pos;
};
Texture2D<float4> diffuse : register(t0);
Texture2D<float4> lightmap : register(t1);
StructuredBuffer<float4x4> bones : register(t2);
SamplerState smp : register(s0);
struct VSOut {
  float4 pos : SV_Position;
  float3 rpos : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
};
VSOut vs_main(float3 p : POSITION, float2 uv : TEXCOORD0, float2 uv2 : TEXCOORD1,
              float4 bw : BLENDWEIGHT0, uint4 bi : BLENDINDICES0) {
  VSOut o;
  float4 mp = float4(p, 1.0);
  // tint.g > 0 marks a skinned item: the bone palette (row-vector matrices)
  // maps model space to world space; mvp is then just view*proj.
  float wsum = dot(bw, float4(1, 1, 1, 1));
  if (tint.g > 0.0 && wsum > 0.001) {
    float4 skinned = float4(0, 0, 0, 0);
    // Guest blend indices are pre-multiplied by 3 (VS constant register
    // index; one bone = 3 float4 registers).
    [unroll] for (int k = 0; k < 4; ++k) {
      skinned += bw[k] * mul(mp, bones[bi[k] / 3u]);
    }
    mp = float4(skinned.xyz / wsum, 1.0);
  }
  o.pos = mul(mp, mvp);
  o.rpos = mul(mp, world).xyz - cam_pos.xyz;
  o.uv = uv;
  o.uv2 = uv2;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  if (tint.a > 0.0) {
    return tint;
  }
  float4 albedo = diffuse.Sample(smp, i.uv);
  // Alpha-tested foliage/fences; opaque formats sample alpha = 1. Skinned
  // characters pack gloss in alpha; never clip them.
  if (tint.g == 0.0) {
    clip(albedo.a - 0.35);
  }
  // tint.r > 0 marks items with a lightmap bound (2x baked lighting);
  // otherwise fall back to derivative face shading.
  float3 lit;
  if (tint.r > 0.0) {
    lit = albedo.rgb * lightmap.Sample(smp, i.uv2).rgb * 2.0;
  } else {
    float3 n = normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float l = abs(dot(n, normalize(float3(0.4, 0.8, 0.3)))) * 0.35 + 0.75;
    lit = albedo.rgb * l;
  }
  return float4(lit, 1.0);
}
)";

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  if (exp == 0) {
    if (mant == 0) return std::bit_cast<float>(sign);
    // subnormal
    while (!(mant & 0x400)) {
      mant <<= 1;
      --exp;
    }
    ++exp;
    mant &= 0x3FF;
  } else if (exp == 31) {
    return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
  }
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (mant << 13));
}

ID3D12Resource* CreateUploadBuffer(ID3D12Device* device, size_t size) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* resource = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  return resource;
}

uint16_t SwapU16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
uint32_t SwapU32(uint32_t v) {
#if defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

// Decode guest vertices into {float3 position, float2 uv, float2 uv2,
// unorm4 blend weights, u8x4 blend indices} (36-byte stride).
bool DecodeMesh(ID3D12Device* device, uint8_t* base, const DrawItem& item,
                MeshBuffers& out) {
  const uint32_t num_verts = item.vb_bytes / item.stride;
  if (num_verts == 0) return false;
  ID3D12Resource* vb = CreateUploadBuffer(device, size_t(num_verts) * 36);
  ID3D12Resource* ib = CreateUploadBuffer(device, size_t(item.ib_count) * 2);
  if (!vb || !ib) {
    if (vb) vb->Release();
    if (ib) ib->Release();
    return false;
  }

  const uint8_t* src_vb = base + item.vb_addr;
  float* dst = nullptr;
  uint32_t garbage = 0;
  vb->Map(0, nullptr, reinterpret_cast<void**>(&dst));
  for (uint32_t v = 0; v < num_verts; ++v) {
    const uint8_t* p = src_vb + size_t(v) * item.stride + item.pos_offset;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    switch (item.pos_fmt) {
      case 57: {  // k_32_32_32_FLOAT
        x = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p)));
        y = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p + 4)));
        z = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(p + 8)));
        break;
      }
      case 32: {  // k_16_16_16_16_FLOAT
        x = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p)));
        y = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p + 2)));
        z = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(p + 4)));
        break;
      }
      case 26: {  // k_16_16_16_16 snorm character dequant
        const auto s16 = [&](int off) {
          return int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(p + off)));
        };
        constexpr float kScale = 2.0f / 32767.0f;
        x = s16(0) * kScale;
        y = s16(2) * kScale + 0.8f;
        z = s16(4) * kScale;
        break;
      }
      default:
        vb->Unmap(0, nullptr);
        vb->Release();
        ib->Release();
        return false;
    }
    if (!(x == x && y == y && z == z) ||
        x < item.bbox_min[0] - 2.f || x > item.bbox_max[0] + 2.f ||
        y < item.bbox_min[1] - 2.f || y > item.bbox_max[1] + 2.f ||
        z < item.bbox_min[2] - 2.f || z > item.bbox_max[2] + 2.f) {
      ++garbage;
    }
    const auto decode_uv = [&](uint32_t fmt, uint32_t offset, float& u, float& w) {
      u = 0.0f;
      w = 0.0f;
      if (fmt == 0) return;
      const uint8_t* q = src_vb + size_t(v) * item.stride + offset;
      switch (fmt) {
        case 31:  // k_16_16_FLOAT
        case 32:  // k_16_16_16_16_FLOAT (use xy)
          u = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          w = HalfToFloat(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          break;
        case 38:  // k_32_32_FLOAT
          u = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q)));
          w = std::bit_cast<float>(SwapU32(*reinterpret_cast<const uint32_t*>(q + 4)));
          break;
        case 25: {  // k_16_16 (snorm; UVs span the full s16 range for [0,1])
          const int16_t su = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
          const int16_t sv = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
          u = su / 32767.0f;
          w = sv / 32767.0f;
          break;
        }
        default:
          break;
      }
    };
    dst[v * 9 + 0] = x;
    dst[v * 9 + 1] = y;
    dst[v * 9 + 2] = z;
    decode_uv(item.uv_fmt, item.uv_offset, dst[v * 9 + 3], dst[v * 9 + 4]);
    decode_uv(item.uv2_fmt, item.uv2_offset, dst[v * 9 + 5], dst[v * 9 + 6]);
    // Blend weights (unorm bytes) and indices (raw bytes). Guest u8x4
    // attributes are stored big-endian per 32-bit word; swap so component k
    // in the shader matches guest component k.
    uint32_t bw = 0;
    uint32_t bi = 0;
    if (item.skinned) {
      bw = SwapU32(*reinterpret_cast<const uint32_t*>(src_vb + size_t(v) * item.stride +
                                                      item.bw_offset));
      bi = SwapU32(*reinterpret_cast<const uint32_t*>(src_vb + size_t(v) * item.stride +
                                                      item.bi_offset));
    }
    std::memcpy(&dst[v * 9 + 7], &bw, 4);
    std::memcpy(&dst[v * 9 + 8], &bi, 4);
  }
  vb->Unmap(0, nullptr);
  if (garbage != 0) {
    REXLOG_WARN(
        "native-scene: mesh {:08X} decoded {} of {} verts outside bbox "
        "({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) fmt {} stride {} vb {:08X}",
        item.mesh, garbage, num_verts, item.bbox_min[0], item.bbox_min[1], item.bbox_min[2],
        item.bbox_max[0], item.bbox_max[1], item.bbox_max[2], item.pos_fmt, item.stride,
        item.vb_addr);
  }

  const uint16_t* src_ib = reinterpret_cast<const uint16_t*>(base + item.ib_addr);
  uint16_t* dst_ib = nullptr;
  ib->Map(0, nullptr, reinterpret_cast<void**>(&dst_ib));
  for (uint32_t i = 0; i < item.ib_count; ++i) {
    dst_ib[i] = SwapU16(src_ib[i]);
  }
  ib->Unmap(0, nullptr);

  out.vb = vb;
  out.ib = ib;
  out.vb_view = {vb->GetGPUVirtualAddress(), num_verts * 36u, 36u};
  out.ib_view = {ib->GetGPUVirtualAddress(), item.ib_count * 2u, DXGI_FORMAT_R16_UINT};
  return true;
}

// Decode a guest texture (v1-verified path: fetch constant at renderengine::
// Texture words [7..12], CPU untile block by block through the 0xA0000000
// physical mirror, endian swap) and create its SRV in the staging heap.
bool EnsureGuestTexture(const NativeGuestOutputRenderContext& context, uint8_t* base,
                        uint32_t tex_ptr, GuestTexture& out) {
  uint32_t words[6] = {};
  if (!GuestRangeReadable(base, tex_ptr + 7 * 4, 6 * 4)) {
    return false;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    words[i] = REX_LOAD_U32(tex_ptr + (7 + i) * 4);
  }
  std::memcpy(out.fetch_words, words, sizeof(words));

  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dword_0 = words[0];
  fetch.dword_1 = words[1];
  fetch.dword_2 = words[2];
  fetch.dword_3 = words[3];
  fetch.dword_4 = words[4];
  fetch.dword_5 = words[5];
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.base_address == 0) {
    return false;
  }
  rex::graphics::TextureInfo info;
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
    return false;
  }
  const uint32_t guest_data_address = 0xA0000000u | info.memory.base_address;
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u ||
      !GuestRangeReadable(base, guest_data_address, info.memory.base_size)) {
    return false;
  }

  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
    return false;
  }
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_columns =
      (width + format_info->block_width - 1) / format_info->block_width;
  const uint32_t block_rows =
      (height + format_info->block_height - 1) / format_info->block_height;
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t host_width = block_columns * format_info->block_width;
  const uint32_t host_height = block_rows * format_info->block_height;

  ID3D12Device* device = context.d3d12.device;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = host_width;
  desc.Height = host_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = host.resource_format;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&out.texture)))) {
    return false;
  }
  const uint32_t row_bytes = block_columns * bytes_per_block;
  const uint32_t upload_pitch = (row_bytes + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) &
                                ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
  out.upload = CreateUploadBuffer(device, size_t(upload_pitch) * block_rows);
  if (!out.upload) {
    out.texture->Release();
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = nullptr;
  out.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapping));
  const uint8_t* guest = base + guest_data_address;
  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  for (uint32_t by = 0; by < block_rows; ++by) {
    uint8_t* out_row = mapping + size_t(by) * upload_pitch;
    for (uint32_t bx = 0; bx < block_columns; ++bx) {
      uint32_t source_offset;
      if (info.is_tiled) {
        source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(bx), int32_t(by), pitch_blocks, bytes_per_block_log2));
      } else {
        source_offset = (by * pitch_blocks + bx) * bytes_per_block;
      }
      if (source_offset + bytes_per_block > info.memory.base_size) {
        std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
        continue;
      }
      std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + source_offset,
                  bytes_per_block);
    }
    SwapGuestEndian(out_row, row_bytes, info.endianness);
    if (swap_rb_565) {
      for (uint32_t i = 0; i + 2 <= row_bytes; i += 2) {
        uint16_t value;
        std::memcpy(&value, out_row + i, sizeof(value));
        value = uint16_t((value & 0x07E0u) | ((value >> 11) & 0x1Fu) |
                         ((value & 0x1Fu) << 11));
        std::memcpy(out_row + i, &value, sizeof(value));
      }
    }
  }
  out.upload->Unmap(0, nullptr);

  // Record the upload copy into the deferred command list.
  auto* command_processor = context.d3d12.command_processor;
  auto& list = command_processor->GetDeferredCommandList();
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = out.texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = out.upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint.Format = host.resource_format;
  src.PlacedFootprint.Footprint.Width = host_width;
  src.PlacedFootprint.Footprint.Height = host_height;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = upload_pitch;
  list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        out.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // SRV in the staging heap with the composed swizzle.
  if (g_r.srv_next >= 8192) {
    return false;
  }
  out.srv_slot = g_r.srv_next++;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = host.srv_format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
  srv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
  device->CreateShaderResourceView(out.texture, &srv, slot);
  out.valid = true;
  return true;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) return false;
  ID3D12Device* device = context.d3d12.device;
  g_r.device = device;

  if (!g_r.root_signature) {
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 40;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE srv_range[2] = {};
    srv_range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range[0].NumDescriptors = 1;
    srv_range[0].BaseShaderRegister = 0;
    srv_range[1] = srv_range[0];
    srv_range[1].BaseShaderRegister = 1;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range[0];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2] = params[1];
    params[2].DescriptorTable.pDescriptorRanges = &srv_range[1];
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 2;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!context.d3d12.create_root_signature(context.d3d12.command_processor_user_data,
                                             &desc, &g_r.root_signature)) {
      REXLOG_ERROR("native-scene: root signature creation failed");
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.pso || g_r.rtv_format != context.d3d12.guest_output_format) {
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "native_scene", nullptr,
                          nullptr, "vs_main", "vs_5_0", 0, 0, &vs, &errors)) ||
        FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "native_scene", nullptr,
                          nullptr, "ps_main", "ps_5_0", 0, 0, &ps, &errors))) {
      REXLOG_ERROR("native-scene: shader compile failed: {}",
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      g_r.failed = true;
      return false;
    }
    D3D12_INPUT_ELEMENT_DESC input[5] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_r.root_signature;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.InputLayout = {input, 5};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = context.d3d12.guest_output_format;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    const HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso));
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    const HRESULT hr2 =
        device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_nodepth));
    vs->Release();
    ps->Release();
    if (errors) errors->Release();
    if (FAILED(hr) || FAILED(hr2)) {
      REXLOG_ERROR("native-scene: PSO creation failed {:08X}/{:08X}", uint32_t(hr),
                   uint32_t(hr2));
      g_r.failed = true;
      return false;
    }
    g_r.rtv_format = context.d3d12.guest_output_format;
  }

  if (!g_r.rtv_heap) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_r.rtv_heap)))) {
      g_r.failed = true;
      return false;
    }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_r.dsv_heap)))) {
      g_r.failed = true;
      return false;
    }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 8192;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_r.srv_heap)))) {
      g_r.failed = true;
      return false;
    }
    g_r.srv_size =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  if (!g_r.bone_ring) {
    g_r.bone_ring = CreateUploadBuffer(
        device, size_t(RendererState::kBoneRegionSize) * RendererState::kBoneRegions);
    if (!g_r.bone_ring ||
        FAILED(g_r.bone_ring->Map(0, nullptr,
                                  reinterpret_cast<void**>(&g_r.bone_ring_cpu)))) {
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.white.valid) {
    // 1x1 white fallback for items without a resolved diffuse texture.
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&g_r.white.texture)))) {
      g_r.failed = true;
      return false;
    }
    g_r.white.upload = CreateUploadBuffer(device, 256);
    if (!g_r.white.upload) {
      g_r.failed = true;
      return false;
    }
    uint8_t* mapping = nullptr;
    g_r.white.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapping));
    std::memset(mapping, 0xFF, 4);
    g_r.white.upload->Unmap(0, nullptr);
    auto& list = context.d3d12.command_processor->GetDeferredCommandList();
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_r.white.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_r.white.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 256;
    list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          g_r.white.texture,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_r.white.srv_slot = g_r.srv_next++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(g_r.white.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(g_r.white.texture, &srv, slot);
    g_r.white.valid = true;
  }

  const uint32_t width = context.guest_output_width;
  const uint32_t height = context.guest_output_height;
  if (!g_r.depth || g_r.depth_width != width || g_r.depth_height != height) {
    if (g_r.depth) {
      g_r.depth->Release();
      g_r.depth = nullptr;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    if (FAILED(g_r.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&g_r.depth)))) {
      g_r.failed = true;
      return false;
    }
    g_r.depth_width = width;
    g_r.depth_height = height;
    device->CreateDepthStencilView(g_r.depth, nullptr,
                                   g_r.dsv_heap->GetCPUDescriptorHandleForHeapStart());
  }

  if (g_r.rtv_resource != context.d3d12.guest_output_resource) {
    device->CreateRenderTargetView(context.d3d12.guest_output_resource, nullptr,
                                   g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart());
    g_r.rtv_resource = context.d3d12.guest_output_resource;
  }
  return true;
}

bool RenderScene(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  if (!SceneEnabled() || context.backend != NativeGuestOutputBackend::kD3D12) {
    return false;
  }
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return false;
  }

  FrameScene scene;
  {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    if (g_scene.items.empty()) {
      return false;
    }
    scene = g_scene;
  }

  if (!EnsurePipeline(context)) {
    return false;
  }
  // Flush any barriers pushed by lazy resource creation (white texture).
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  if (!g_r.announced) {
    g_r.announced = true;
    REXLOG_INFO("native-scene: rendering natively ({} items, {}x{})", scene.items.size(),
                context.guest_output_width, context.guest_output_height);
  }

  auto* command_processor = context.d3d12.command_processor;
  auto& list = command_processor->GetDeferredCommandList();

  // Free retired buffers whose last-referencing submission has completed.
  if (!g_r.retired.empty()) {
    const uint64_t completed = command_processor->GetCompletedSubmission();
    std::erase_if(g_r.retired, [completed](const auto& entry) {
      if (entry.second < completed) {
        entry.first->Release();
        return true;
      }
      return false;
    });
  }

  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        context.d3d12.guest_output_resource,
                                        context.d3d12.guest_output_initial_state,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  const int32_t debug_mode_early = REXCVAR_GET(skate3_native_render_scene_debug);
  const bool use_depth = debug_mode_early != 4;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_r.dsv_heap->GetCPUDescriptorHandleForHeapStart();
  const FLOAT clear_color[4] = {0.25f, 0.35f, 0.55f, 1.0f};
  list.D3DClearRenderTargetView(rtv, clear_color, 0, nullptr);
  if (use_depth) {
    list.D3DClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list.D3DOMSetRenderTargets(1, &rtv, FALSE, &dsv);
  } else {
    list.D3DOMSetRenderTargets(1, &rtv, FALSE, nullptr);
  }

  D3D12_VIEWPORT viewport{0.0f,
                          0.0f,
                          float(context.guest_output_width),
                          float(context.guest_output_height),
                          0.0f,
                          1.0f};
  list.RSSetViewport(viewport);
  D3D12_RECT scissor{0, 0, LONG(context.guest_output_width),
                     LONG(context.guest_output_height)};
  list.RSSetScissorRect(scissor);
  list.D3DSetGraphicsRootSignature(g_r.root_signature);
  list.D3DSetPipelineState(use_depth ? g_r.pso : g_r.pso_nodepth);
  // Our own persistent shader-visible SRV heap for the whole pass. These are
  // the last commands of the submission, so displacing the emulated GPU's
  // heap binding is safe.
  list.SetDescriptorHeaps(g_r.srv_heap, nullptr);

  const int32_t debug_mode = REXCVAR_GET(skate3_native_render_scene_debug);
  // Reset this frame's bone ring region.
  const uint64_t frame_number = g_frames_rendered.load(std::memory_order_relaxed);
  const uint32_t bone_region =
      uint32_t(frame_number % RendererState::kBoneRegions) *
      RendererState::kBoneRegionSize;
  g_r.bone_ring_offset = 0;
  uint32_t drawn = 0;
  uint32_t item_index = 0;
  for (const DrawItem& item : scene.items) {
    const uint32_t index = item_index++;
    if (debug_mode == 1) {
      break;
    }
    if (debug_mode == 3 && index >= 20) {
      break;
    }
    auto it = g_r.meshes.find(item.mesh);
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint) {
      const uint64_t submission = command_processor->GetCurrentSubmission();
      g_r.retired.emplace_back(it->second.vb, submission);
      g_r.retired.emplace_back(it->second.ib, submission);
      g_r.meshes.erase(it);
      it = g_r.meshes.end();
    }
    if (it == g_r.meshes.end()) {
      MeshBuffers buffers;
      if (!DecodeMesh(g_r.device, base, item, buffers)) {
        continue;
      }
      buffers.fingerprint = item.fingerprint;
      it = g_r.meshes.emplace(item.mesh, buffers).first;
    }
    const MeshBuffers& buffers = it->second;

    // Resolve guest textures (white fallback). Cached decodes revalidate
    // against the live fetch words; streaming reuses texture objects. The
    // object addresses were readable on the game thread this frame.
    const auto resolve_texture = [&](uint32_t tex_ptr) -> const GuestTexture* {
      if (tex_ptr == 0) {
        return &g_r.white;
      }
      auto tit = g_r.textures.find(tex_ptr);
      if (tit != g_r.textures.end()) {
        uint32_t live[6];
        for (uint32_t k = 0; k < 6; ++k) {
          live[k] = REX_LOAD_U32(tex_ptr + (7 + k) * 4);
        }
        const bool retry_failed =
            !tit->second.valid && frame_number >= tit->second.retry_after_frame;
        if (retry_failed ||
            std::memcmp(live, tit->second.fetch_words, sizeof(live)) != 0) {
          const uint64_t submission = command_processor->GetCurrentSubmission();
          if (tit->second.texture) g_r.retired.emplace_back(tit->second.texture, submission);
          if (tit->second.upload) g_r.retired.emplace_back(tit->second.upload, submission);
          g_r.textures.erase(tit);
          tit = g_r.textures.end();
        }
      }
      if (tit == g_r.textures.end()) {
        GuestTexture gt;
        EnsureGuestTexture(context, base, tex_ptr, gt);
        // Remember the live words even for failed decodes so they are not
        // re-attempted every frame; schedule a periodic retry instead.
        if (!gt.valid) {
          if (gt.fetch_words[0] == 0 && gt.fetch_words[1] == 0) {
            for (uint32_t k = 0; k < 6; ++k) {
              gt.fetch_words[k] = REX_LOAD_U32(tex_ptr + (7 + k) * 4);
            }
          }
          gt.retry_after_frame = frame_number + 120;
        }
        // Flush the COPY_DEST -> PIXEL_SHADER_RESOURCE barrier before the
        // draw that samples the new texture.
        context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
        tit = g_r.textures.emplace(tex_ptr, gt).first;
      }
      return tit->second.valid ? &tit->second : &g_r.white;
    };
    const GuestTexture* diffuse = resolve_texture(item.diffuse_tex);
    const GuestTexture* lightmap =
        item.lightmap_tex != 0 && REXCVAR_GET(skate3_native_render_scene_lightmaps)
            ? resolve_texture(item.lightmap_tex)
            : nullptr;
    if (lightmap == &g_r.white) {
      lightmap = nullptr;
    }

    // constants = world + mvp (world * view_proj, row-vector) + tint + cam.
    // tint.a > 0 selects debug solid colors; tint.r > 0 marks a bound
    // lightmap in the normal path.
    float constants[40];
    std::memcpy(constants, item.world, sizeof(item.world));
    float* mvp = constants + 16;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += item.world[r * 4 + k] * scene.view_proj[k * 4 + c];
        }
        mvp[r * 4 + c] = sum;
      }
    }
    // Bone palette upload for skinned items; tint.g flags skinning.
    bool bones_bound = false;
    if (item.skinned && !item.bones.empty()) {
      const uint32_t bytes = uint32_t(item.bones.size() * sizeof(float));
      const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
      if (offset + bytes <= RendererState::kBoneRegionSize) {
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.bones.data(), bytes);
        g_r.bone_ring_offset = offset + bytes;
        list.D3DSetGraphicsRootShaderResourceView(
            3, g_r.bone_ring->GetGPUVirtualAddress() + bone_region + offset);
        bones_bound = true;
      }
    }
    if (!bones_bound) {
      list.D3DSetGraphicsRootShaderResourceView(3, g_r.bone_ring->GetGPUVirtualAddress());
    }

    if (debug_mode >= 2) {
      constants[32] = float((index * 37u) % 255u) / 255.0f;
      constants[33] = float((index * 73u) % 255u) / 255.0f;
      constants[34] = float((index * 151u) % 255u) / 255.0f;
      constants[35] = 1.0f;
    } else {
      constants[32] = lightmap != nullptr ? 1.0f : 0.0f;
      constants[33] = bones_bound ? 1.0f : 0.0f;
      constants[34] = constants[35] = 0.0f;
    }
    constants[36] = scene.cam_pos[0];
    constants[37] = scene.cam_pos[1];
    constants[38] = scene.cam_pos[2];
    constants[39] = 0.0f;
    list.D3DSetGraphicsRoot32BitConstants(0, 40, constants, 0);

    const D3D12_GPU_DESCRIPTOR_HANDLE heap_start =
        g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = heap_start;
    srv_gpu.ptr += size_t(diffuse->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 1, srv_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE lm_gpu = heap_start;
    lm_gpu.ptr +=
        size_t((lightmap != nullptr ? lightmap : &g_r.white)->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 2, lm_gpu);
    list.D3DIASetVertexBuffers(0, 1, &buffers.vb_view);
    list.D3DIASetIndexBuffer(&buffers.ib_view);
    for (const DrawEntry& draw : item.draws) {
      if (draw.prim == 4) {
        list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      } else if (draw.prim == 6) {
        list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      } else {
        continue;
      }
      list.D3DDrawIndexedInstanced(draw.index_count, 1, draw.start_index, draw.base_vertex,
                                   0);
      ++drawn;
    }
  }

  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        context.d3d12.guest_output_resource,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        context.d3d12.guest_output_initial_state);
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  const uint64_t frames = g_frames_rendered.fetch_add(1) + 1;
  if (frames % 600 == 0) {
    REXLOG_INFO(
        "native-scene: frame {} items={} draws={} cached_meshes={} textures={} "
        "vs_uploads={} large={} palettes={} skinned={} skinned_skipped={}",
        frames, scene.items.size(), drawn, g_r.meshes.size(), g_r.textures.size(),
        g_vs_uploads.load(), g_vs_uploads_large.load(), g_palette_snapshots.load(),
        g_skinned_items.load(), g_skinned_skipped.load());
  }
  return true;
}

}  // namespace

void Install() {
  if (!SceneEnabled()) {
    return;
  }
  rex::graphics::SetNativeGuestOutputRenderer(&RenderScene, nullptr);
  REXLOG_INFO("native-scene: guest output renderer registered");
}

}  // namespace skate3::native_scene

#else  // !REX_HAS_D3D12

namespace skate3::native_scene {
void Install() {}
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12
