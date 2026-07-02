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
                     "Number of consecutive frames of RenderMesh records to collect before "
                     "writing the snapshot")
    .range(1, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_snapshot_dir, "native_render_snapshots", "Skate 3",
                      "Directory for native-render guest memory snapshots and metadata")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_render {
namespace {

// One per-mesh submission. kind 0 = RenderMesh (dynamic entities: r3 is the
// MeshContext itself, b = VertexProgramState). kind 1 = SceneRenderView draw
// list entry (world geometry: a = MeshContext, b = sort key, c = view).
using RenderMeshRecord = skate3::native_scene::SubmitRecord;

struct FrameRecords {
  uint64_t frame_index;
  std::vector<RenderMeshRecord> records;
};

std::mutex g_mutex;
std::vector<RenderMeshRecord> g_current_frame;
std::vector<FrameRecords> g_collected_frames;
uint64_t g_frame_index = 0;
bool g_collecting = false;
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
      << "\"kinds\":{\"0\":\"RenderMesh a=ctx b=vps\",\"1\":\"SceneDrawList a=ctx "
         "b=sort_key c=view\"}}\n";
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

void OnRenderMesh(uint32_t mesh_context, uint32_t vertex_program_state) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({0, mesh_context, vertex_program_state, 0});
}

// SceneRenderView draw-list renderer sub_827FAF50(view, sort_vec, first, count):
// sort_vec points at an eastl vector whose [0] is the entry array; entries are
// 8 bytes {u32 sort_key, MeshContext*}.
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
    if (mesh_context != 0) {
      g_current_frame.push_back({1, mesh_context, list_offset, view});
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
  REXLOG_INFO("native-render snapshot: {} ({} frames of records, meta_ok={} snap_ok={})",
              stem, g_collected_frames.size(), meta_ok, snap_ok);
  g_collected_frames.clear();
  g_snapshot_written = true;
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
      REXLOG_INFO("native-render snapshot: armed at frame {} ({} meshes)", g_frame_index,
                  mesh_count);
    }
  }
  // Manual trigger (works repeatedly): create <snapshot_dir>\trigger while
  // the game runs.
  if (!g_collecting && g_frame_index % 32 == 0) {
    const std::filesystem::path trigger = SnapshotDir() / "trigger";
    std::error_code ec;
    if (std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      g_collecting = true;
      REXLOG_INFO("native-render snapshot: trigger file, armed at frame {} ({} meshes)",
                  g_frame_index, mesh_count);
    }
  }
  if (g_collecting) {
    g_collected_frames.push_back({g_frame_index, std::move(g_current_frame)});
    const auto wanted =
        static_cast<size_t>(REXCVAR_GET(skate3_native_render_snapshot_frames));
    if (g_collected_frames.size() >= wanted) {
      g_collecting = false;
      WriteSnapshotLocked(base);
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
extern "C" REX_FUNC(sub_82795AD8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnRenderMesh(ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_82795AD8(ctx, base);
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
