// Presentation-entity identity store; see skate3_native_entity.h for the
// verified guest facts behind the binding model.
// The walk below mirrors sub_827A6658 (the base BindConstants)
// exactly: it re-reads the same arrays the game just walked, so a
// conforming entity registers and a non-conforming one self-limits out.

#include "native/skate3_native_entity.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_guest_read.h"

REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_entity_ident, true, "Skate 3",
    "Build the presentation-entity identity store (MeshContext -> owning "
    "entity, from the game's BindConstants walks). The store feeds the "
    "entity serve paths (fade serve, ROPA world serve, palette identity); "
    "off disables those consumers as well.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_entity_ident_log, false, "Skate 3",
    "Log identity-store diagnostics: the periodic ident[] stats line and "
    "rate-limited serve-path lines.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_entity_ropa_world, true, "Skate 3",
    "Serve a refused ROPA garment world matrix from the owner entity's "
    "m_MatLtoWTrans (+416) via the identity store, instead of leaving the "
    "item pending. Only activates where the draw-time bank read refused "
    "(foreign bank / post-shadow clobber / torn state); accepted bank "
    "reads are bit-exact equal to this field, so the accept path is "
    "unchanged. Off = refused captures stay pending (rescue/fixup paths).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_entity_fade_serve, true, "Skate 3",
    "Serve skater-family character fade from the entity's own opacity "
    "field (+496, the exact value the game binds as the shader's alpha "
    "parameter) instead of the captured alpha row. Fixes spawn fade-ins "
    "rendered opaque when the row capture lags or fails. LivingWorld "
    "entities keep the LW store's fade. Off = captured-row fade.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_entity_ropa_world_primary, true, "Skate 3",
    "Serve the ROPA garment world from the owner entity's m_MatLtoWTrans "
    "on ACCEPTED bank captures too (bank value kept as fallback and as a "
    "live bit-compare tripwire: wprim= in the ident[] line). Value-"
    "identical where the bank is healthy (observer-proven bit-exact) and "
    "keeps the garment on its own entity through every bank state. "
    "Lighting-row capture still follows the bank's own acceptance. Off = "
    "entity world only on bank refusal.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_entity {
namespace {

using skate3::native_scene::GuestTryCopy;

// PresentationEntity offsets (verified: sub_827A6658 / sub_82783D68 /
// sub_82793F70 / sub_827A68B0 walk + stores in the generated code).
constexpr uint32_t kEntL2W = 352;       // m_MatLtoW (Matrix44)
constexpr uint32_t kEntL2WTrans = 416;  // m_MatLtoWTrans (Matrix44)
constexpr uint32_t kEntOpacitySkater = 496;  // skater family, x = alpha
constexpr uint32_t kEntOpacityLw = 528;      // LivingWorld family
// SkaterPresEntity cloth fields (offsets validated live before the
// shadow-mode reader that proved them was retired).
constexpr uint32_t kRopaGate = 1921;         // u8 m_bSkinningEnabled
constexpr uint32_t kRopaClothTarget = 1040;  // u32 m_pModelClothTarget[i]
constexpr uint32_t kRopaCount = 1096;        // i32 m_numClothModels
constexpr uint32_t kRopaVb = 1936;           // u32 vb pair, + i*8 + cur*4
constexpr uint32_t kInstCtxArr = 0x04;       // cModelInstance MeshContext*
constexpr uint32_t kInstStride = 0x28;
constexpr uint32_t kCtxStride = 0x50;

struct EntityRec {
  EntClass cls = EntClass::kUnknown;
  uint32_t vtable = 0;
  int32_t view_refs = 0;
  uint64_t bind_count = 0;
};

struct CtxEntry {
  uint32_t entity = 0;
  uint32_t instance = 0;
};

std::mutex g_mu;
std::unordered_map<uint32_t, EntityRec> g_entities;
std::unordered_map<uint32_t, CtxEntry> g_ctx;

// Deformed-VB freshness: vb object -> last DoubleBuffer completion (ms).
// Guarded by its own lock (the sim thread stamps at cloth-tick rate; the
// render thread reads per serve).
std::mutex g_vb_mu;
std::unordered_map<uint32_t, uint64_t> g_vb_fresh;

// Telemetry (600-frame window, exchanged to zero at emission).
std::atomic<uint32_t> g_bind_calls{0};
std::atomic<uint32_t> g_add_events{0};
std::atomic<uint32_t> g_rmv_events{0};
std::atomic<uint32_t> g_item_hit{0};
std::atomic<uint32_t> g_item_miss{0};
std::atomic<uint32_t> g_item_lw_overlap{0};
std::atomic<uint32_t> g_fade_ok{0};
std::atomic<uint32_t> g_fade_div{0};
std::atomic<uint32_t> g_fade_bad{0};
// ROPA world vs entity matrices: [0]=e+416 (transposed), [1]=e+352 (raw);
// exact / near / divergent, plus unmapped ctx and unreadable entity.
std::atomic<uint32_t> g_w_exact[2]{};
std::atomic<uint32_t> g_w_near[2]{};
std::atomic<uint32_t> g_w_div[2]{};
std::atomic<uint32_t> g_ropa_unmap{0};
std::atomic<uint32_t> g_ropa_read_fail{0};
// Serve flip 1: entity-world serves on bank refusal / structural rejects /
// mode-authority refusals (the garment table does not claim the VB as a
// live CPU cloth target; asserting rigid there would flip the mode).
std::atomic<uint32_t> g_wserve{0};
std::atomic<uint32_t> g_wserve_rej{0};
std::atomic<uint32_t> g_wserve_nomode{0};
// Primary-serve tripwire: serves on accepted captures / bank divergences.
std::atomic<uint32_t> g_wprim{0};
std::atomic<uint32_t> g_wprim_div{0};
// Per-class item hits: Unknown/LW/Skater/Colorized/Cac/SkaterAux.
std::atomic<uint32_t> g_hit_cls[6]{};

std::atomic<uint32_t> g_frame_counter{0};

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

bool LoadF32Block(uint8_t* base, uint32_t addr, float* out, uint32_t count) {
  if (addr < 0x10000) {
    return false;
  }
  uint32_t raw[16];
  if (count > 16 || !GuestTryCopy(raw, base + addr, size_t(count) * 4)) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
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

const char* ClassName(EntClass cls) {
  switch (cls) {
    case EntClass::kLivingWorld: return "lw";
    case EntClass::kSkater: return "skater";
    case EntClass::kColorized: return "colorized";
    case EntClass::kCac: return "cac";
    case EntClass::kSkaterAux: return "skater-aux";
    default: return "base";
  }
}

// Compare 3 staged rows (translation in component 3) against the first 3
// rows of a guest Matrix44. 0 = exact, 1 = near (within one sim step),
// 2 = divergent.
int CompareRows(const float rows[12], const float mem[12]) {
  bool exact = true;
  bool near_ok = true;
  for (int r = 0; r < 3; ++r) {
    for (int i = 0; i < 4; ++i) {
      const float d = std::fabs(rows[r * 4 + i] - mem[r * 4 + i]);
      const float tol_near = (i == 3) ? 0.5f : 0.05f;
      if (d > 1e-4f) {
        exact = false;
      }
      if (d > tol_near) {
        near_ok = false;
      }
    }
  }
  return exact ? 0 : (near_ok ? 1 : 2);
}

}  // namespace

void OnBindConstants(uint8_t* base, uint32_t entity) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident) || entity == 0) {
    return;
  }
  g_bind_calls.fetch_add(1, std::memory_order_relaxed);
  uint32_t vtable = 0, idx = 0, count = 0, arr = 0;
  if (!LoadU32(base, entity, &vtable) || !LoadU32(base, entity + 16, &idx) ||
      idx > 1024 || !LoadU32(base, entity + (idx + 5) * 4, &count) ||
      !LoadU32(base, entity + (idx + 8) * 4, &arr) || count == 0 ||
      count > 32 || !PlausiblePtr(arr)) {
    return;
  }
  struct CtxRec {
    uint32_t ctx;
    uint32_t instance;
  };
  CtxRec ctxs[32 * 64];
  uint32_t nctx = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rec = arr + i * kInstStride;
    uint32_t cbegin = 0, cend = 0;
    if (!LoadU32(base, rec + kInstCtxArr, &cbegin) ||
        !LoadU32(base, rec + kInstCtxArr + 4, &cend) ||
        !PlausiblePtr(cbegin) || cend <= cbegin ||
        (cend - cbegin) % kCtxStride != 0) {
      continue;
    }
    const uint32_t slots = (cend - cbegin) / kCtxStride;
    if (slots > 64) {
      continue;
    }
    for (uint32_t k = 0; k < slots && nctx < 32 * 64; ++k) {
      ctxs[nctx++] = {cbegin + k * kCtxStride, rec};
    }
  }
  if (nctx == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  // Growth backstop: binds are one-shot per entity, so dead entries never
  // refresh; drop view-dead entities' ctxs when the map gets large.
  if (g_ctx.size() > 16384) {
    for (auto it = g_ctx.begin(); it != g_ctx.end();) {
      const auto ent = g_entities.find(it->second.entity);
      const bool dead = ent == g_entities.end() || ent->second.view_refs <= 0;
      it = dead ? g_ctx.erase(it) : std::next(it);
    }
  }
  if (g_entities.size() > 8192) {
    for (auto it = g_entities.begin(); it != g_entities.end();) {
      it = it->second.view_refs <= 0 ? g_entities.erase(it) : std::next(it);
    }
  }
  EntityRec& er = g_entities[entity];
  er.vtable = vtable;
  er.bind_count++;
  for (uint32_t i = 0; i < nctx; ++i) {
    g_ctx[ctxs[i].ctx] = {entity, ctxs[i].instance};
  }
}

void OnBindClass(uint32_t entity, EntClass cls) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident) || entity == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_entities.find(entity);
  if (it == g_entities.end()) {
    return;  // base walk failed its guards; keep the entity out entirely
  }
  // Most-derived override returns last, so plain assignment converges.
  it->second.cls = cls;
  // One capped line per distinct (vtable, class) pair: the live class
  // table, for promoting vtable-based classification later.
  static std::unordered_map<uint64_t, bool> s_seen;
  const uint64_t key = (uint64_t(it->second.vtable) << 8) | uint8_t(cls);
  if (REXCVAR_GET(skate3_native_render_scene_entity_ident_log) &&
      s_seen.size() < 64 && s_seen.emplace(key, true).second) {
    REXLOG_INFO("native-entity: class {} vtbl={:08X} entity={:08X}",
                ClassName(cls), it->second.vtable, entity);
  }
}

void OnEntityViewAdd(uint32_t entity) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident) || entity == 0) {
    return;
  }
  g_add_events.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mu);
  // Entities can register with views before their first bind; the record
  // gets its ctxs (and class) when BindConstants fires.
  g_entities[entity].view_refs++;
}

void OnRopaDoubleBuffer(uint8_t* base, uint32_t skater, uint32_t index) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident) || skater == 0 ||
      index >= 8) {
    return;
  }
  uint32_t vb0 = 0, vb1 = 0;
  if (!LoadU32(base, skater + kRopaVb + index * 8, &vb0) ||
      !LoadU32(base, skater + kRopaVb + index * 8 + 4, &vb1)) {
    return;
  }
  const uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_vb_mu);
  if (g_vb_fresh.size() > 512) {
    for (auto it = g_vb_fresh.begin(); it != g_vb_fresh.end();) {
      it = now - it->second > 2000 ? g_vb_fresh.erase(it) : std::next(it);
    }
  }
  if (vb0 != 0) {
    g_vb_fresh[vb0] = now;
  }
  if (vb1 != 0) {
    g_vb_fresh[vb1] = now;
  }
}

void OnEntityViewRemove(uint32_t entity) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident) || entity == 0) {
    return;
  }
  g_rmv_events.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_entities.find(entity);
  if (it != g_entities.end()) {
    it->second.view_refs = std::max(0, it->second.view_refs - 1);
  }
}

bool LookupCtx(uint32_t ctx, CtxInfo* out) {
  if (ctx == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_ctx.find(ctx);
  if (it == g_ctx.end()) {
    return false;
  }
  if (out != nullptr) {
    out->entity = it->second.entity;
    out->instance = it->second.instance;
    const auto ent = g_entities.find(it->second.entity);
    out->cls = ent != g_entities.end() ? ent->second.cls : EntClass::kUnknown;
    out->view_live = ent != g_entities.end() && ent->second.view_refs > 0;
  }
  return true;
}

void ObserveRopaWorld(uint8_t* base, uint32_t ctx, const float rows[12]) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident)) {
    return;
  }
  CtxInfo info;
  if (!LookupCtx(ctx, &info)) {
    g_ropa_unmap.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  float m416[12], m352[12];
  const bool ok416 = LoadF32Block(base, info.entity + kEntL2WTrans, m416, 12);
  const bool ok352 = LoadF32Block(base, info.entity + kEntL2W, m352, 12);
  if (!ok416 || !ok352) {
    g_ropa_read_fail.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int c416 = CompareRows(rows, m416);
  const int c352 = CompareRows(rows, m352);
  (c416 == 0 ? g_w_exact[0] : c416 == 1 ? g_w_near[0] : g_w_div[0])
      .fetch_add(1, std::memory_order_relaxed);
  (c352 == 0 ? g_w_exact[1] : c352 == 1 ? g_w_near[1] : g_w_div[1])
      .fetch_add(1, std::memory_order_relaxed);
  if (c416 == 2 && c352 == 2) {
    static std::atomic<uint32_t> s_logged{0};
    const uint32_t ln = s_logged.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(skate3_native_render_scene_entity_ident_log) &&
        (ln < 8 || (ln & 1023u) == 0)) {
      REXLOG_INFO(
          "native-entity: ropa world DIVERGES both fields ctx={:08X} "
          "cls={} entity={:08X} bank=({:.3f},{:.3f},{:.3f},{:.2f}) "
          "e416=({:.3f},{:.3f},{:.3f},{:.2f}) "
          "e352=({:.3f},{:.3f},{:.3f},{:.2f}) (n={})",
          ctx, ClassName(info.cls), info.entity, rows[0], rows[1], rows[2],
          rows[3], m416[0], m416[1], m416[2], m416[3], m352[0], m352[1],
          m352[2], m352[3], ln);
    }
  }
}

void ObserveCharItem(uint8_t* base, uint32_t ctx, uint32_t family,
                     bool lw_mapped, float used_alpha) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident)) {
    return;
  }
  CtxInfo info;
  if (!LookupCtx(ctx, &info)) {
    g_item_miss.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_item_hit.fetch_add(1, std::memory_order_relaxed);
  g_hit_cls[std::min<uint32_t>(uint32_t(info.cls), 5u)].fetch_add(
      1, std::memory_order_relaxed);
  if (lw_mapped) {
    g_item_lw_overlap.fetch_add(1, std::memory_order_relaxed);
  }
  const bool skater_family =
      info.cls == EntClass::kSkater || info.cls == EntClass::kColorized ||
      info.cls == EntClass::kCac || info.cls == EntClass::kSkaterAux;
  if (!skater_family) {
    return;  // LW opacity is already proven/served; base classes untracked
  }
  float alpha = 0.0f;
  if (!LoadF32Block(base, info.entity + kEntOpacitySkater, &alpha, 1) ||
      !(alpha >= -0.01f && alpha <= 1.01f)) {
    g_fade_bad.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (std::fabs(alpha - used_alpha) <= 0.05f) {
    g_fade_ok.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_fade_div.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_logged{0};
    const uint32_t ln = s_logged.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(skate3_native_render_scene_entity_ident_log) &&
        (ln < 8 || (ln & 2047u) == 0)) {
      REXLOG_INFO(
          "native-entity: fade divergence ctx={:08X} cls={} fam={} "
          "e496={:.3f} used={:.3f} (n={})",
          ctx, ClassName(info.cls), family, alpha, used_alpha, ln);
    }
  }
}

void NoteAcceptedWorldCompare(const float bank_rows[12],
                              const float ent_rows[12]) {
  g_wprim.fetch_add(1, std::memory_order_relaxed);
  for (int i = 0; i < 12; ++i) {
    if (std::fabs(bank_rows[i] - ent_rows[i]) > 1e-4f) {
      g_wprim_div.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_logged{0};
      const uint32_t ln = s_logged.fetch_add(1, std::memory_order_relaxed);
      if (REXCVAR_GET(skate3_native_render_scene_entity_ident_log) &&
          (ln < 8 || (ln & 1023u) == 0)) {
        REXLOG_INFO(
            "native-entity: PRIMARY world diverges from accepted bank "
            "i={} bank={:.4f} ent={:.4f} (n={})",
            i, bank_rows[i], ent_rows[i], ln);
      }
      return;
    }
  }
}

namespace {

// Shared structural validation: 16-float Matrix44 with a (0,0,0,1) tail
// row and bank-gate row norms; copies the 3 affine rows on success.
bool ReadWorldRowsChecked(uint8_t* base, uint32_t entity, float out_rows[12]) {
  float m[16];
  if (!LoadF32Block(base, entity + kEntL2WTrans, m, 16)) {
    return false;
  }
  const bool tail_ok = std::fabs(m[12]) <= 1e-4f && std::fabs(m[13]) <= 1e-4f &&
                       std::fabs(m[14]) <= 1e-4f &&
                       std::fabs(m[15] - 1.0f) <= 1e-4f;
  bool rows_ok = tail_ok;
  for (int r = 0; r < 3 && rows_ok; ++r) {
    float n = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float f = m[r * 4 + i];
      rows_ok = rows_ok && f > -1e7f && f < 1e7f;
      n += f * f;
    }
    rows_ok = rows_ok && n > 0.0025f && n < 400.0f && m[r * 4 + 3] > -20000.f &&
              m[r * 4 + 3] < 20000.f;
  }
  if (!rows_ok) {
    return false;
  }
  std::memcpy(out_rows, m, 12 * sizeof(float));
  return true;
}

}  // namespace

bool ReadEntityWorldRows(uint8_t* base, uint32_t ctx, float out_rows[12]) {
  CtxInfo info;
  return LookupCtx(ctx, &info) &&
         ReadWorldRowsChecked(base, info.entity, out_rows);
}

bool ReadSkaterFade(uint8_t* base, uint32_t ctx, float* out_alpha) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_fade_serve)) {
    return false;
  }
  CtxInfo info;
  if (!LookupCtx(ctx, &info)) {
    return false;
  }
  const bool skater_family =
      info.cls == EntClass::kSkater || info.cls == EntClass::kColorized ||
      info.cls == EntClass::kCac || info.cls == EntClass::kSkaterAux;
  if (!skater_family) {
    return false;
  }
  float alpha = 0.0f;
  if (!LoadF32Block(base, info.entity + kEntOpacitySkater, &alpha, 1) ||
      !(alpha >= -0.01f && alpha <= 1.01f)) {
    return false;
  }
  *out_alpha = alpha;
  return true;
}

uint32_t ServeInstancePalette(uint8_t* base, uint32_t ctx, float* out,
                              uint32_t max_rows) {
  CtxInfo info;
  if (!LookupCtx(ctx, &info) || info.instance == 0) {
    return 0;
  }
  uint32_t matrices = 0, count = 0;
  if (!LoadU32(base, info.instance + 0x14, &matrices) ||
      !LoadU32(base, info.instance + 0x18, &count) || !PlausiblePtr(matrices) ||
      count < 1 || count > 96 || count > max_rows) {
    return 0;
  }
  uint32_t raw[96 * 12];
  if (!GuestTryCopy(raw, base + matrices, size_t(count) * 48)) {
    return 0;
  }
  for (uint32_t i = 0; i < count * 12; ++i) {
    out[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
  // Row sanity (packed 3x4 rotation norms in the bank-gate range): a
  // stale/unpacked buffer fails and the caller keeps its fallback.
  for (uint32_t r = 0; r < count; ++r) {
    float n = 0.0f;
    for (int j = 0; j < 3; ++j) {
      const float* m = out + r * 12;
      n += m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j];
    }
    if (!(n > 0.0016f && n < 2000.0f)) {
      return 0;
    }
  }
  return count;
}

bool RopaGarmentDropped(uint8_t* base, uint32_t ctx, uint32_t vb_obj) {
  if (vb_obj == 0) {
    return false;
  }
  CtxInfo info;
  if (!LookupCtx(ctx, &info)) {
    return false;
  }
  uint32_t gate_w = 0, n = 0;
  if (!LoadU32(base, info.entity + (kRopaGate & ~3u), &gate_w) ||
      !LoadU32(base, info.entity + kRopaCount, &n) || n > 8) {
    return false;  // unreadable/implausible: never suppress on uncertainty
  }
  const uint8_t gate = uint8_t(gate_w >> ((3 - (kRopaGate & 3u)) * 8));
  if (gate != 0 && n >= 1) {
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t target = 0, vb0 = 0, vb1 = 0;
      if (LoadU32(base, info.entity + kRopaClothTarget + i * 4, &target) &&
          target != 0 &&
          LoadU32(base, info.entity + kRopaVb + i * 8, &vb0) &&
          LoadU32(base, info.entity + kRopaVb + i * 8 + 4, &vb1) &&
          (vb0 == vb_obj || vb1 == vb_obj)) {
        return false;  // still a live cloth target
      }
    }
  }
  return true;  // mapped entity, garment table does not claim this vb
}

bool ServeRopaWorld(uint8_t* base, uint32_t ctx, uint32_t vb_obj,
                    float out_rows[12]) {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ropa_world)) {
    return false;
  }
  CtxInfo info;
  if (!LookupCtx(ctx, &info)) {
    return false;
  }
  // Mode authority: the entity's own garment table must claim this VB as
  // a live CPU cloth target this tick. Sim-inactive garments (distance/
  // activity toggles) draw SKINNED; serving them a rigid world would
  // flip the mode against the post-draw fixup every frame.
  bool cpu_garment = false;
  {
    uint8_t gate = 0;
    uint32_t n = 0;
    uint32_t gate_w = 0;
    if (vb_obj != 0 &&
        LoadU32(base, info.entity + (kRopaGate & ~3u), &gate_w) &&
        LoadU32(base, info.entity + kRopaCount, &n) && n >= 1 && n <= 8) {
      gate = uint8_t(gate_w >> ((3 - (kRopaGate & 3u)) * 8));
      for (uint32_t i = 0; i < n && !cpu_garment && gate != 0; ++i) {
        uint32_t target = 0, vb0 = 0, vb1 = 0;
        cpu_garment =
            LoadU32(base, info.entity + kRopaClothTarget + i * 4, &target) &&
            target != 0 &&
            LoadU32(base, info.entity + kRopaVb + i * 8, &vb0) &&
            LoadU32(base, info.entity + kRopaVb + i * 8 + 4, &vb1) &&
            (vb0 == vb_obj || vb1 == vb_obj);
      }
    }
  }
  // Tick freshness: the garment-table fields persist after the game stops
  // running the cloth job (sleeping/paused sims keep their last target and
  // VB pointers), so the table alone misclassifies a sleeping garment as
  // CPU-simulated, and its VB holds a STALE drape (the floating shirt).
  // DoubleBuffer runs once per garment per COMPLETED sim tick; require it
  // within the last few ticks.
  if (cpu_garment) {
    std::lock_guard<std::mutex> lock(g_vb_mu);
    const auto it = g_vb_fresh.find(vb_obj);
    cpu_garment = it != g_vb_fresh.end() && NowMs() - it->second <= 150;
  }
  if (!cpu_garment) {
    g_wserve_nomode.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Structural proof of a live Matrix44 ((0,0,0,1) tail + bank-gate row
  // norms): a despawned entity's recycled memory does not pass both.
  if (!ReadWorldRowsChecked(base, info.entity, out_rows)) {
    g_wserve_rej.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_wserve.fetch_add(1, std::memory_order_relaxed);
  static std::atomic<uint32_t> s_logged{0};
  const uint32_t ln = s_logged.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_native_render_scene_entity_ident_log) &&
      (ln < 8 || (ln & 2047u) == 0)) {
    REXLOG_INFO(
        "native-entity: ropa world SERVED ctx={:08X} cls={} entity={:08X} "
        "t=({:.2f},{:.2f},{:.2f}) (n={})",
        ctx, ClassName(info.cls), info.entity, out_rows[3], out_rows[7],
        out_rows[11], ln);
  }
  return true;
}

void EmitStats() {
  if (!REXCVAR_GET(skate3_native_render_scene_entity_ident) ||
      !REXCVAR_GET(skate3_native_render_scene_entity_ident_log)) {
    return;
  }
  const uint32_t frame =
      g_frame_counter.fetch_add(1, std::memory_order_relaxed);
  if (frame == 0 || frame % 600 != 0) {
    return;
  }
  size_t ents = 0, ctxs = 0, live = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    ents = g_entities.size();
    ctxs = g_ctx.size();
    for (const auto& [addr, rec] : g_entities) {
      live += rec.view_refs > 0 ? 1 : 0;
    }
  }
  REXLOG_INFO(
      "native-entity: ident[ents={}/{} ctx={} bind={} add={} rmv={} "
      "hit={} miss={} lw_ovl={} cls(b/lw/sk/co/cac/aux)={}/{}/{}/{}/{}/{} "
      "fade ok/div/bad={}/{}/{} w416 e/n/d={}/{}/{} w352 e/n/d={}/{}/{} "
      "ropa unmap={} rfail={} wserve={}/{}/{} wprim={}/{}]",
      live, ents, ctxs, g_bind_calls.exchange(0, std::memory_order_relaxed),
      g_add_events.exchange(0, std::memory_order_relaxed),
      g_rmv_events.exchange(0, std::memory_order_relaxed),
      g_item_hit.exchange(0, std::memory_order_relaxed),
      g_item_miss.exchange(0, std::memory_order_relaxed),
      g_item_lw_overlap.exchange(0, std::memory_order_relaxed),
      g_hit_cls[0].exchange(0, std::memory_order_relaxed),
      g_hit_cls[1].exchange(0, std::memory_order_relaxed),
      g_hit_cls[2].exchange(0, std::memory_order_relaxed),
      g_hit_cls[3].exchange(0, std::memory_order_relaxed),
      g_hit_cls[4].exchange(0, std::memory_order_relaxed),
      g_hit_cls[5].exchange(0, std::memory_order_relaxed),
      g_fade_ok.exchange(0, std::memory_order_relaxed),
      g_fade_div.exchange(0, std::memory_order_relaxed),
      g_fade_bad.exchange(0, std::memory_order_relaxed),
      g_w_exact[0].exchange(0, std::memory_order_relaxed),
      g_w_near[0].exchange(0, std::memory_order_relaxed),
      g_w_div[0].exchange(0, std::memory_order_relaxed),
      g_w_exact[1].exchange(0, std::memory_order_relaxed),
      g_w_near[1].exchange(0, std::memory_order_relaxed),
      g_w_div[1].exchange(0, std::memory_order_relaxed),
      g_ropa_unmap.exchange(0, std::memory_order_relaxed),
      g_ropa_read_fail.exchange(0, std::memory_order_relaxed),
      g_wserve.exchange(0, std::memory_order_relaxed),
      g_wserve_rej.exchange(0, std::memory_order_relaxed),
      g_wserve_nomode.exchange(0, std::memory_order_relaxed),
      g_wprim.exchange(0, std::memory_order_relaxed),
      g_wprim_div.exchange(0, std::memory_order_relaxed));
}

}  // namespace skate3::native_entity
