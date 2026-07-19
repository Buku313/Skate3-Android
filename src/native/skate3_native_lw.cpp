// LivingWorld entity store; see skate3_native_lw.h. Verified guest facts
// (from the recompiled game code, generated/skate3_recomp.36.cpp):
//
//  - cLivingWorldPresEntity::Update (sub_827C1188, :43097) computes the
//    spawn ramp (entity+576, += const per tick, clamped [0,1]) and the
//    distance term (EvaluateOpacityDistance 827C1870 / the +612/+616
//    override), stores min(ramp, distance) as the .x of the Vector4 at
//    entity+528 and the "is fading" byte at +580.
//  - cLivingWorldPresEntity::BindConstants (sub_827C1720, :43927) pushes
//    &entity+528 into the material params of EVERY mesh ctx of every
//    instance: idx=[e+16], count=[e+(idx+5)*4], array=[e+(idx+8)*4]
//    stride 0x28; instance: model +0, MeshContext array +4 (end +8,
//    stride 0x50), mesh count = u16 at model+0x32. This is the alpha the
//    character-family shaders output (c13.x / c21.x / c22.x / c20.x).
//
// The walk below mirrors SnapshotLwEntity (skate3_native_palette.cpp),
// whose ctx containment matching against DrawItem::ctx was live-proven
// (measured zero unmapped palettes).

#include "native/skate3_native_lw.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_palette, true, "Skate 3",
    "Snapshot each LivingWorld entity's OWN packed per-mesh palette "
    "(cModelInstance.m_matrices, written by the pack writer every sim "
    "tick) into the LW entity store, and substitute it for GUESSED ortho "
    "caster-bank palettes on edge-of-view vehicles (the mangle/transform "
    "class). Off = the caster heuristics alone.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_lw {
namespace {

using skate3::native_scene::GuestTryCopy;

// Entity/instance layout (verified: SnapshotLwEntity + sub_827C1720).
constexpr uint32_t kLwOpacity = 528;       // Vector4, x = alpha
constexpr uint32_t kInstModel = 0x00;      // cModelInstance model ptr
constexpr uint32_t kInstArrCtx = 0x04;     // m_arrMeshContext begin
constexpr uint32_t kInstArrCtxEnd = 0x08;  // VectorBase end
constexpr uint32_t kInstMatrices = 0x14;   // packed palette, 48-byte rows
constexpr uint32_t kInstStride = 0x28;
constexpr uint32_t kCtxStride = 0x50;

struct LwCtxEntry {
  float alpha = 1.0f;
  uint32_t entity = 0;
  uint64_t stamp_ms = 0;
  // The entity's own packed palette slice for this ctx's mesh (host-order,
  // 3 float4 rows per bone), snapshotted at the same tick as the alpha.
  uint32_t pal_rows = 0;
  uint64_t pal_stamp_ms = 0;
  std::vector<float> palette;
};

std::mutex g_mu;
std::unordered_map<uint32_t, LwCtxEntry> g_ctx;  // MeshContext -> entity state

// Distinct-entity rate counter (stats line): entities ticked in the
// current 1 s window.
std::atomic<uint32_t> g_ents_window{0};
std::atomic<uint32_t> g_ents_last{0};
std::atomic<uint64_t> g_ents_win_start{0};

bool LoadU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint32_t raw;
  if (!GuestTryCopy(&raw, base + addr, 4)) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool PlausiblePtr(uint32_t p) {
  return p >= 0x10000 && p < 0x84A10000 && (p & 3) == 0;
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

void OnLwEntityTick(uint8_t* base, uint32_t entity) {
  // Fresh opacity first: an unreadable/insane alpha means teardown or a
  // non-conforming subclass; skip the entity, never store garbage.
  uint32_t alpha_u = 0;
  if (!LoadU32(base, entity + kLwOpacity, &alpha_u)) {
    return;
  }
  const float alpha = std::bit_cast<float>(alpha_u);
  if (!(alpha >= -0.01f && alpha <= 1.01f)) {
    return;
  }
  uint32_t idx = 0, count = 0, arr = 0;
  if (!LoadU32(base, entity + 16, &idx) || idx > 1024 ||
      !LoadU32(base, entity + (idx + 5) * 4, &count) ||
      !LoadU32(base, entity + (idx + 8) * 4, &arr) || count == 0 ||
      count > 8 || !PlausiblePtr(arr)) {
    return;
  }
  const uint64_t now = NowMs();
  {
    uint64_t win = g_ents_win_start.load(std::memory_order_relaxed);
    if (now - win >= 1000) {
      if (g_ents_win_start.compare_exchange_strong(win, now,
                                                   std::memory_order_relaxed)) {
        g_ents_last.store(g_ents_window.exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
      }
    }
    g_ents_window.fetch_add(1, std::memory_order_relaxed);
  }
  const bool want_pal = REXCVAR_GET(skate3_native_render_scene_lw_palette);
  struct CtxRec {
    uint32_t ctx;
    uint32_t pal_ofs;   // float offset into pal[] (UINT32_MAX = no slice)
    uint32_t pal_rows;  // bone rows
  };
  CtxRec ctxs[8 * 64];
  uint32_t nctx = 0;
  // Palette staging: copied + byteswapped OUTSIDE the lock (the sim thread
  // holding g_mu through a multi-KB swap loop stalled the render thread in
  // the shadow reader's history, same rule here).
  float pal[8 * 96 * 12];
  uint32_t pal_used = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rec = arr + i * kInstStride;
    uint32_t cbegin = 0, cend = 0;
    if (!LoadU32(base, rec + kInstArrCtx, &cbegin) ||
        !LoadU32(base, rec + kInstArrCtxEnd, &cend) || !PlausiblePtr(cbegin) ||
        cend <= cbegin || (cend - cbegin) % kCtxStride != 0) {
      continue;
    }
    const uint32_t slots = (cend - cbegin) / kCtxStride;
    if (slots > 64) {
      continue;
    }
    // The entity's own packed palette (m_matrices): per-mesh bone counts
    // from the model walk (mesh table +0x24, count u16 +0x32, bone-count
    // gate u16 +0x34 <= 80, mesh bone count +0x48 - the verified
    // SnapshotLwEntity conventions); mesh k's slice starts after the
    // preceding meshes' rows and pairs with ctx slot k.
    uint32_t bones[16];
    uint32_t nmesh = 0;
    uint32_t inst_pal_ofs = UINT32_MAX;
    if (want_pal) {
      uint32_t model = 0, matrices = 0, count_w = 0, table = 0, gate_w = 0;
      if (LoadU32(base, rec + kInstModel, &model) &&
          LoadU32(base, rec + kInstMatrices, &matrices) &&
          PlausiblePtr(model) && PlausiblePtr(matrices) &&
          LoadU32(base, model + 0x30, &count_w) &&
          LoadU32(base, model + 0x24, &table) &&
          LoadU32(base, model + 0x34, &gate_w) && (gate_w >> 16) <= 80) {
        const uint32_t nm = count_w & 0xFFFF;
        if (nm >= 1 && nm <= 16) {
          uint32_t total = 0;
          bool ok = true;
          for (uint32_t k = 0; k < nm && ok; ++k) {
            uint32_t mesh = 0, bc = 0;
            ok = LoadU32(base, table + k * 4, &mesh) && PlausiblePtr(mesh) &&
                 LoadU32(base, mesh + 0x48, &bc) && bc >= 1 && bc <= 96;
            if (ok) {
              bones[k] = bc;
              total += bc;
            }
          }
          uint32_t raw[96 * 12];
          if (ok && total >= 1 && total <= 96 &&
              pal_used + total * 12 <= 8 * 96 * 12 &&
              GuestTryCopy(raw, base + matrices, size_t(total) * 48)) {
            for (uint32_t f = 0; f < total * 12; ++f) {
              pal[pal_used + f] =
                  std::bit_cast<float>(__builtin_bswap32(raw[f]));
            }
            inst_pal_ofs = pal_used;
            pal_used += total * 12;
            nmesh = nm;
          }
        }
      }
    }
    uint32_t row = 0;
    for (uint32_t k = 0; k < slots && nctx < 8 * 64; ++k) {
      CtxRec& c = ctxs[nctx++];
      c.ctx = cbegin + k * kCtxStride;
      if (k < nmesh && inst_pal_ofs != UINT32_MAX) {
        c.pal_ofs = inst_pal_ofs + row * 12;
        c.pal_rows = bones[k];
        row += bones[k];
      } else {
        c.pal_ofs = UINT32_MAX;
        c.pal_rows = 0;
      }
    }
  }
  if (nctx == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  // Growth backstop: entries stop refreshing when their entity despawns;
  // sweep stale ones (2 s) before the map can grow past the cap.
  if (g_ctx.size() > 8192) {
    for (auto it = g_ctx.begin(); it != g_ctx.end();) {
      it = now - it->second.stamp_ms > 2000 ? g_ctx.erase(it) : std::next(it);
    }
  }
  for (uint32_t i = 0; i < nctx; ++i) {
    LwCtxEntry& e = g_ctx[ctxs[i].ctx];
    e.alpha = alpha;
    e.entity = entity;
    e.stamp_ms = now;
    if (ctxs[i].pal_ofs != UINT32_MAX) {
      e.pal_rows = ctxs[i].pal_rows;
      e.pal_stamp_ms = now;
      e.palette.assign(pal + ctxs[i].pal_ofs,
                       pal + ctxs[i].pal_ofs + size_t(ctxs[i].pal_rows) * 12);
    }
  }
}

bool LookupLwCtx(uint32_t ctx, float* out_alpha, uint32_t* out_entity) {
  if (ctx == 0) {
    return false;
  }
  const uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_ctx.find(ctx);
  if (it == g_ctx.end() || now - it->second.stamp_ms > 500) {
    return false;
  }
  if (out_alpha != nullptr) {
    *out_alpha = it->second.alpha;
  }
  if (out_entity != nullptr) {
    *out_entity = it->second.entity;
  }
  return true;
}

uint32_t LookupLwPalette(uint32_t ctx, float* out, uint32_t max_floats) {
  if (ctx == 0 || out == nullptr) {
    return 0;
  }
  const uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_ctx.find(ctx);
  // 100 ms freshness: a palette older than a few sim ticks is a FROZEN
  // pose, worse than the caster heuristics it would replace.
  if (it == g_ctx.end() || it->second.pal_rows == 0 ||
      now - it->second.pal_stamp_ms > 100 ||
      it->second.palette.size() != size_t(it->second.pal_rows) * 12 ||
      it->second.palette.size() > max_floats) {
    return 0;
  }
  std::memcpy(out, it->second.palette.data(),
              it->second.palette.size() * sizeof(float));
  return it->second.pal_rows;
}

void QueryLwStats(uint32_t* out_ctxs, uint32_t* out_entities) {
  if (out_entities != nullptr) {
    *out_entities = g_ents_last.load(std::memory_order_relaxed);
  }
  if (out_ctxs != nullptr) {
    std::lock_guard<std::mutex> lock(g_mu);
    *out_ctxs = uint32_t(g_ctx.size());
  }
}

}  // namespace skate3::native_lw
