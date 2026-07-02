#include "skate3_native_render.h"

#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(skate3_native_render, false, "Skate 3",
                    "Enable the Skate 3 data-driven native renderer hook layer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(skate3_native_render_log_interval, 600, "Skate 3",
                     "Frames between native-render hook liveness log lines (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_snapshot_min_meshes, 0, "Skate 3",
                     "One-shot guest memory snapshot: trigger on the first frame with at "
                     "least this many RenderMesh submissions (0 = disabled)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_snapshot_frames, 4, "Skate 3",
                     "Number of frames of RenderMesh records to collect before writing "
                     "the snapshot")
    .range(1, 600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_snapshot_stride, 1, "Skate 3",
                     "Record every Nth frame while collecting (long viewer recordings: "
                     "e.g. 12 = ~12 recorded frames/sec at 144fps)")
    .range(1, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_snapshot_dir, "native_render_snapshots", "Skate 3",
                      "Directory for native-render guest memory snapshots and metadata")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_render {
namespace {

// One per-mesh submission. kind 0 = RenderMesh (dynamic entities: a = the
// MeshContext, b = VertexProgramState, c = dynitem index+1). kind 1 =
// SceneRenderView sort-list entry (a = MeshContext, b = list offset from the
// view, c = view). kind 2 = world-path capture (skinned / model-space prop:
// a = MeshContext, b = submitting view, c = dynitem index+1). kind 3 =
// quad-list DrawVertices capture (a = synthetic key, c = dynitem index+1).
using RenderMeshRecord = skate3::native_scene::SubmitRecord;

struct FrameRecords {
  uint64_t frame_index;
  std::vector<RenderMeshRecord> records;
};

std::mutex g_mutex;
std::vector<RenderMeshRecord> g_current_frame;
std::vector<FrameRecords> g_collected_frames;
uint64_t g_frame_index = 0;
uint64_t g_collect_counter = 0;
bool g_collecting = false;
// F10 immediate mode: capture exactly one frame regardless of the snapshot
// frames/stride cvars.
bool g_immediate = false;
bool g_snapshot_written = false;
std::atomic<bool> g_announced{false};

bool Enabled() { return REXCVAR_GET(skate3_native_render); }

std::filesystem::path SnapshotDir() {
  std::filesystem::path dir{std::string(REXCVAR_GET(skate3_native_render_snapshot_dir))};
  if (dir.empty()) {
    dir = "native_render_snapshots";
  }
  return dir;
}

std::string SnapshotStem() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  char stem[64];
  std::snprintf(stem, sizeof(stem), "snapshot_%" PRId64, static_cast<int64_t>(seconds));
  return stem;
}

// Guest memory snapshot format (.gsnap):
//   char magic[8] = "SK3GSNP1"
//   repeated regions: u64 guest_offset (little-endian), u64 size, raw bytes
//   terminator region: guest_offset == 0xFFFFFFFFFFFFFFFF, size == 0
// guest_offset is the offset from the guest base mapping. Guest virtual
// address A maps to file region offset A for A < 0xE0000000 and A + 0x1000
// above that (REX_PHYS_HOST_OFFSET physical mirror shift).
bool WriteMemorySnapshot(uint8_t* base, const std::filesystem::path& path) {
#if defined(_WIN32)
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    REXLOG_ERROR("native-render snapshot: cannot open {}", path.string());
    return false;
  }
  out.write("SK3GSNP1", 8);

  uint64_t total_bytes = 0;
  uint32_t region_count = 0;
  uint64_t offset = 0;
  while (offset < REX_MEMORY_SIZE) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(base + offset, &info, sizeof(info)) == 0) {
      break;
    }
    const uint64_t region_size = static_cast<uint64_t>(info.RegionSize);
    const bool readable =
        info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
        (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (readable) {
      const uint64_t clamped =
          region_size > REX_MEMORY_SIZE - offset ? REX_MEMORY_SIZE - offset : region_size;
      out.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
      out.write(reinterpret_cast<const char*>(&clamped), sizeof(clamped));
      out.write(reinterpret_cast<const char*>(base + offset),
                static_cast<std::streamsize>(clamped));
      total_bytes += clamped;
      ++region_count;
    }
    offset += region_size;
  }

  const uint64_t terminator_offset = ~0ull;
  const uint64_t terminator_size = 0;
  out.write(reinterpret_cast<const char*>(&terminator_offset), sizeof(terminator_offset));
  out.write(reinterpret_cast<const char*>(&terminator_size), sizeof(terminator_size));
  out.close();
  if (!out) {
    REXLOG_ERROR("native-render snapshot: write failed for {}", path.string());
    return false;
  }
  REXLOG_INFO("native-render snapshot: {} regions, {} MiB -> {}", region_count,
              total_bytes >> 20, path.string());
  return true;
#else
  (void)base;
  REXLOG_WARN("native-render snapshot: only implemented on Windows, skipping {}",
              path.string());
  return false;
#endif
}

bool WriteMetadata(const std::filesystem::path& path,
                   const std::vector<FrameRecords>& frames) {
  std::ofstream out(path);
  if (!out) {
    REXLOG_ERROR("native-render snapshot: cannot open {}", path.string());
    return false;
  }
  out << "{\"type\":\"header\",\"image_base\":\"0x82000000\","
      << "\"phys_mirror_note\":\"guest addr A -> file offset A, plus 0x1000 for A >= "
         "0xE0000000\","
      << "\"record_fields\":[\"kind\",\"a\",\"b\",\"c\"],"
      << "\"kinds\":{\"0\":\"RenderMesh a=ctx b=vps c=dyn\",\"1\":\"SceneDrawList a=ctx "
         "b=list_offset c=view\",\"2\":\"WorldPathCapture a=ctx b=view c=dyn\","
         "\"3\":\"QuadListDraw a=key c=dyn\"}}\n";
  for (const FrameRecords& frame : frames) {
    out << "{\"type\":\"frame\",\"index\":" << frame.frame_index << ",\"records\":[";
    for (size_t i = 0; i < frame.records.size(); ++i) {
      const RenderMeshRecord& r = frame.records[i];
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s[%u,\"%08X\",\"%08X\",\"%08X\"]", i ? "," : "",
                    r.kind, r.a, r.b, r.c);
      out << buf;
    }
    out << "]}\n";
  }
  out.close();
  return static_cast<bool>(out);
}

void OnRenderMesh(uint8_t* base, uint32_t mesh_context, uint32_t vertex_program_state,
                  bool drew_inside) {
  const uint32_t dyn = skate3::native_scene::CaptureDynamicState(
      base, mesh_context, /*world_path=*/false, drew_inside);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({0, mesh_context, vertex_program_state, dyn});
}


// SceneRenderView draw-list renderer sub_827FAF50(view, sort_vec, first, count):
// sort_vec points at an eastl vector whose [0] is the entry array; entries are
// 8 bytes {u32 sort_key, MeshContext*}.
//
// Skinned entries and rigid MODEL-SPACE props (vending machines and other
// movables that never reach RenderMesh) are captured HERE, before the
// dispatcher draws the list. The captures are transform-pending; the
// post-draw fixup attaches the palette / world matrix at whichever draw
// eventually consumes the mesh's buffers.
void OnSceneDrawList(uint8_t* base, uint32_t view, uint32_t sort_vec, uint32_t first,
                     uint32_t count) {
  if (count == 0 || count > 100000) {
    return;
  }
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return;
  }
  // b = which of the view's sort lists this came from (sort_vec - view), so
  // the scene builder can select the primary opaque list (+20160).
  const uint32_t list_offset = sort_vec - view;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = entries + (first + i) * 8;
    const uint32_t mesh_context = REX_LOAD_U32(entry + 4);
    if (mesh_context == 0) {
      continue;
    }
    g_current_frame.push_back({1, mesh_context, list_offset, view});
    const uint32_t dyn =
        skate3::native_scene::CaptureDynamicState(base, mesh_context, /*world_path=*/true);
    if (dyn != 0) {
      // b = the submitting view: shadow-cascade views submit their own
      // contexts for the same NPCs; rendering those creates ghost
      // duplicates (torso-less: their deferred skin passes never run).
      g_current_frame.push_back({2, mesh_context, view, dyn});
    }
  }
}

void WriteSnapshotLocked(uint8_t* base) {
  const std::filesystem::path dir = SnapshotDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string stem = SnapshotStem();
  const bool meta_ok = WriteMetadata(dir / (stem + ".meta.jsonl"), g_collected_frames);
  const bool snap_ok = WriteMemorySnapshot(base, dir / (stem + ".gsnap"));
  skate3::native_scene::WriteRecording(dir.string().c_str(), stem.c_str());
  REXLOG_INFO("native-render snapshot: {} ({} frames of records, meta_ok={} snap_ok={})",
              stem, g_collected_frames.size(), meta_ok, snap_ok);
  g_collected_frames.clear();
  g_snapshot_written = true;
}

// Non-indexed cloth patch draws (see native_scene::CaptureClothDraw). The
// synthetic "context" key is the dynamic buffer object, stable per garment.
void OnClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
  uint32_t key = 0;
  const uint32_t dyn = skate3::native_scene::CaptureClothDraw(base, r4, r5, r6, r7, &key);
  if (dyn == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({3, key, 0, dyn});
}

void OnFrameEnd(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frame_index;
  const size_t mesh_count = g_current_frame.size();

  skate3::native_scene::BuildFrameScene(base, g_current_frame.data(),
                                        g_current_frame.size());

  const int32_t log_interval = REXCVAR_GET(skate3_native_render_log_interval);
  if (log_interval > 0 && g_frame_index % static_cast<uint64_t>(log_interval) == 0) {
    REXLOG_INFO("native-render frame={} meshes={} snapshot_done={}", g_frame_index,
                mesh_count, g_snapshot_written);
  }

  if (!g_snapshot_written) {
    const int32_t min_meshes = REXCVAR_GET(skate3_native_render_snapshot_min_meshes);
    if (!g_collecting && min_meshes > 0 &&
        mesh_count >= static_cast<size_t>(min_meshes)) {
      g_collecting = true;
      g_collect_counter = 0;
      skate3::native_scene::StartRecording(
          uint32_t(REXCVAR_GET(skate3_native_render_snapshot_stride)));
      REXLOG_INFO("native-render snapshot: armed at frame {} ({} meshes)", g_frame_index,
                  mesh_count);
    }
  }
  // Manual triggers (work repeatedly): press F9 (window recording per the
  // snapshot cvars), F10 (IMMEDIATE single-frame capture: full memory
  // snapshot + this frame's records/draws, for catching a broken object the
  // moment it is on screen), or create <snapshot_dir>\trigger.
#if defined(_WIN32)
  if (!g_collecting) {
    static bool f9_was_down = false;
    const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_down && !f9_was_down) {
      g_collecting = true;
      g_collect_counter = 0;
      skate3::native_scene::StartRecording(
          uint32_t(REXCVAR_GET(skate3_native_render_snapshot_stride)));
      REXLOG_INFO("native-render snapshot: F9, armed at frame {} ({} meshes)",
                  g_frame_index, mesh_count);
    }
    f9_was_down = f9_down;
    static bool f10_was_down = false;
    const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !f10_was_down) {
      g_collecting = true;
      g_immediate = true;
      g_collect_counter = 0;
      skate3::native_scene::StartRecording(1);
      REXLOG_INFO("native-render snapshot: F10, immediate single-frame capture at frame {}",
                  g_frame_index);
    }
    f10_was_down = f10_down;
  }
#endif
  if (!g_collecting && g_frame_index % 32 == 0) {
    const std::filesystem::path trigger = SnapshotDir() / "trigger";
    std::error_code ec;
    if (std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      g_collecting = true;
      g_collect_counter = 0;
      skate3::native_scene::StartRecording(
          uint32_t(REXCVAR_GET(skate3_native_render_snapshot_stride)));
      REXLOG_INFO("native-render snapshot: trigger file, armed at frame {} ({} meshes)",
                  g_frame_index, mesh_count);
    }
  }
  if (g_collecting) {
    // Immediate (F10) captures skip the arming frame: the scene-side
    // recording was only armed after this frame's BuildFrameScene ran, so
    // the first fully-recorded frame is the next one.
    const auto stride = g_immediate
        ? uint64_t(2)
        : static_cast<uint64_t>(REXCVAR_GET(skate3_native_render_snapshot_stride));
    if (++g_collect_counter % stride == 0) {
      g_collected_frames.push_back({g_frame_index, std::move(g_current_frame)});
      const auto wanted = g_immediate
          ? size_t(1)
          : static_cast<size_t>(REXCVAR_GET(skate3_native_render_snapshot_frames));
      if (g_collected_frames.size() >= wanted) {
        g_collecting = false;
        g_immediate = false;
        WriteSnapshotLocked(base);
      }
    }
  }

  g_current_frame.clear();
}

}  // namespace

void Install() {
  if (!Enabled()) {
    return;
  }
  if (!g_announced.exchange(true)) {
    REXLOG_INFO(
        "native-render hook layer enabled (snapshot_min_meshes={}, snapshot_frames={})",
        REXCVAR_GET(skate3_native_render_snapshot_min_meshes),
        REXCVAR_GET(skate3_native_render_snapshot_frames));
  }
  skate3::native_scene::Install();
}

}  // namespace skate3::native_render

// "RenderMesh" per-visible-mesh submission for dynamic entities (characters,
// props). Actual convention (verified via recompiled code + snapshot):
// r3 = MeshContext*, r4 = renderengine::VertexProgramState*.
//
// The dynamic state snapshot must be taken AFTER the original call: the
// mesh's VS constants (instance world matrix, bone palette) are only flushed
// through D3D::SetPending_AluConstants from inside DrawIndexedVertices
// (sub_82B7AD68), i.e. during the RenderMesh body. Capturing on entry reads
// the PREVIOUS draw's constants and renders every dynamic entity with the
// previous entity's transform/palette.
// Deferred (multi-pass) meshes draw nothing inside the call, detected via
// the draw sequence counter so their transforms are left for the post-draw
// fixup instead of being read from a stale constant bank.
extern "C" REX_FUNC(sub_82795AD8) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t mesh_context = ctx.r3.u32;
  const uint32_t vertex_program_state = ctx.r4.u32;
  const uint64_t draws_before = skate3::native_scene::DrawSequence();
  __imp__sub_82795AD8(ctx, base);
  if (enabled) {
    const bool drew_inside = skate3::native_scene::DrawSequence() != draws_before;
    skate3::native_render::OnRenderMesh(base, mesh_context, vertex_program_state,
                                        drew_inside);
  }
}

// SceneRenderView sorted draw-list renderer (world geometry):
// sub_827FAF50(r3 = SceneRenderView*, r4 = eastl vector of 8-byte
// {sort_key, MeshContext*} entries, r5 = first, r6 = count). Called from
// SceneRenderView::Render (82 7FB158) for each of the view's key lists.
extern "C" REX_FUNC(sub_827FAF50) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnSceneDrawList(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                           ctx.r6.u32);
  }
  __imp__sub_827FAF50(ctx, base);
}


// Guest D3D Swap: frame boundary.
extern "C" REX_FUNC(sub_82B82E08) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnFrameEnd(base);
  }
  __imp__sub_82B82E08(ctx, base);
}

// cProcessArenaAsset::RegisterTexture(cAssetList*, cAssetID,
// renderengine::Texture*, rw::Resource&): r4 = 64-bit asset guid,
// r5 = texture object.
extern "C" REX_FUNC(sub_82C9A618) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnRegisterTexture(ctx.r4.u64, ctx.r5.u32);
  }
  __imp__sub_82C9A618(ctx, base);
}

// D3D::SetPending_AluConstants(device, u64 dirty_group_mask, bank, ptr):
// bank 0x4000 = vertex constants. Called from inside the Draw* functions;
// ptr is the device's positional constant shadow bank.
extern "C" REX_FUNC(sub_82B83FE0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnVsConstantUpload(base, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                             ctx.r3.u32);
  }
  __imp__sub_82B83FE0(ctx, base);
}

// D3DDevice_SetIndices(device, ib) / D3DDevice_SetStreamSource(device,
// stream, vb, offset, stride): track the currently bound guest buffers so
// draws can be matched back to captured skinned items.
extern "C" REX_FUNC(sub_82B79190) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetIndices(ctx.r4.u32);
  }
  __imp__sub_82B79190(ctx, base);
}

extern "C" REX_FUNC(sub_82B78FF0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetStreamSource(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                            ctx.r7.u32);
  }
  __imp__sub_82B78FF0(ctx, base);
}

// D3DDevice_DrawIndexedVertices: post-call, the draw's VS constants are now
// in the shadow bank; refresh any pending skinned item bound to these
// buffers (deferred multi-pass meshes only draw here).
extern "C" REX_FUNC(sub_82B7AD68) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7AD68(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 0, r4, r5, r6, r7);
  }
}

// D3DDevice_DrawVertices, non-indexed draw path: cloth-simulated garments
// (captured live as world-space quad items) and character shadow proxies.
extern "C" REX_FUNC(sub_82B7A970) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7A970(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 1, r4, r5, r6, r7);
    skate3::native_render::OnClothDraw(base, r4, r5, r6, r7);
  }
}

// Fourth SetPending_AluConstants caller (symbol mislabeled as
// D3DQuery_GetData: it stages draw constants, so it is a draw variant;
// never observed firing in gameplay). Hooked so the draw-sequence counter
// and recording stay complete if it ever does.
extern "C" REX_FUNC(sub_82B7A458) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7A458(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 3, r4, r5, r6, r7);
  }
}

// D3DDevice_BeginVertices: inline (write-through-ring) vertex path; the
// CPU writes computed vertices directly. Post-call r3 = guest write pointer.
extern "C" REX_FUNC(sub_82B79FC0) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B79FC0(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 2, r4, r5, r6, ctx.r3.u32 != 0 ? ctx.r3.u32 : r7);
  }
}
