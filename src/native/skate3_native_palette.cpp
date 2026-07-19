// Authoritative character bone-palette serve (see the header).

#include "native/skate3_native_palette.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "skate3_native_scene.h"

REXCVAR_DEFINE_BOOL(skate3_native_render_scene_palette_serve, true, "Skate 3",
                    "Serve character bone palettes from the game's own packed "
                    "m_matrices snapshots (taken at pack-function exit: the exact "
                    "bytes the game's VS constant upload consumes) instead of the "
                    "draw-time bank captures. Per-instance and per-piece, one sim "
                    "tick, coherent by construction. Items with no mapped snapshot "
                    "keep the capture pipeline's palette unchanged.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_palette_ident_primary, true, "Skate 3",
                    "Entity-exact palette picks: for items the ctx -> entity "
                    "identity store maps, a snapshot stamped with the SAME owning "
                    "entity is identity-proven; it is exempt from the "
                    "anchor/proximity caps (which exist only to reject other "
                    "characters' packs) and wins over any unstamped candidate the "
                    "content heuristics matched. Unmapped items and unstamped "
                    "candidates keep the content matching unchanged.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_palette_log_interval, 0, "Skate 3",
                     "Frames between palette-serve stats log lines (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_palette {
namespace {

using skate3::native_scene::DrawItem;
using skate3::native_scene::FrameScene;
using skate3::native_scene::GuestTryCopy;

// cModelInstance field offsets (sizeof 0x28).
constexpr uint32_t kInstModel = 0x00;
constexpr uint32_t kInstArrCtx = 0x04;     // m_arrMeshContext begin
constexpr uint32_t kInstArrCtxEnd = 0x08;  // VectorBase end (validated, else 50-slot cap)
constexpr uint32_t kInstMatrices = 0x14;   // packed palette, 48-byte 3x4 rows
constexpr uint32_t kCtxStride = 0x50;
constexpr uint32_t kCtxSlotsFallback = 50;

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

// Plausible guest heap/image pointer (the guest address space tops out under
// 0x84A10000).
bool PlausiblePtr(uint32_t p) { return p >= 0x10000 && p < 0x84A10000 && (p & 3) == 0; }

std::mutex g_mu;  // guards every per-frame collection below

// Pack-exit palette snapshot: header + m_matrices rows copied into the
// per-frame arena the moment the game finished packing them; comparing at
// frame end is too late (m_matrices is repacked with the NEXT animation step
// by then), so the copy happens the moment the pack function returns: those
// bytes are exactly what the VS constant upload reads. An instance can pack
// more than once per frame (multi-pass poses); every snapshot is kept.
struct PackSnap {
  uint32_t inst = 0;
  // Owning presentation entity, stamped from the bracketed pack-owner
  // (every pack call sits inside an entity method whose r3 is the owner;
  // LivingWorld slices carry the writer's entity directly). 0 = the pack
  // fired outside any bracketed owner (unknown).
  uint32_t entity = 0;
  uint32_t arr_begin = 0;  // m_arrMeshContext block (ctx containment key)
  uint32_t arr_bytes = 0;
  uint32_t row_ofs = 0;  // offset into g_pack_rows (floats)
  uint32_t rows = 0;     // 48-byte 3x4 rows copied
  // Sk8::UpdateBoneTransforms part-record snapshot (the player skater's
  // pieces): rows serve only after the per-session layout vote confirms the
  // row interpretation (see ServeAuthoritativePalettes).
  bool ubt = false;
};
std::vector<PackSnap> g_packs;
std::vector<float> g_pack_rows;  // host-order floats, 12 per row (arena)
// Previous frame's snapshots: the draws of frame N may consume the palette
// packed during frame N-1 (pack at frame end, upload next frame).
std::vector<PackSnap> g_packs_prev;
std::vector<float> g_pack_rows_prev;

uint64_t g_frames = 0;  // OnFrameBuilt calls (serve-history clock)

// Self-validation probe health: if the cModelInstance layout fails its
// probes the module turns itself off for the session (logged once); image
// drift must degrade, not misread.
std::atomic<bool> g_pal_ok{true};

// ---- serve stats (window; exchanged to 0 at each stats line) ---------------

std::atomic<uint64_t> c_pal_unreadable{0};
std::atomic<uint64_t> c_pal_served{0};
std::atomic<uint64_t> c_pal_serve_miss{0};
std::atomic<uint64_t> c_pal_hyst{0};
// Identity filter (ctx -> entity store): candidates stamped with a
// DIFFERENT owning entity than the item's are never served (the
// wrong-sibling/wrong-character serve class: garments tracking the wrong
// skater).
std::atomic<uint64_t> c_pal_serve_ident_skip{0};
// Content gate: candidates whose served range contains a degenerate
// (zeroed/garbage) bone row are never served; a partially-written
// m_matrices snapshotted around instance teardown/transition serves rows
// the game would never upload (weighted verts collapse to the origin).
std::atomic<uint64_t> c_pal_serve_bad_rows{0};
// Entity-exact primary picks: serves where the winning candidate carried the
// item's own entity stamp / serves of a mapped item that had to fall back to
// content matching / picks where the entity-exact candidate and the
// content-matched candidate held DIFFERENT rows; each divergence is a serve
// the proximity heuristics would have gotten wrong.
std::atomic<uint64_t> c_pal_ident_exact{0};
std::atomic<uint64_t> c_pal_ident_fallback{0};
std::atomic<uint64_t> c_pal_ident_prim_div{0};
std::atomic<uint64_t> c_ubt_snap_ok{0};
std::atomic<uint64_t> c_ubt_rej_ptrs{0};
std::atomic<uint64_t> c_ubt_arr_fallback{0};

// Every packed 3x4 row in [0, rows) must carry a sane rotation (Frobenius
// norm-squared of the 3x3 within the bank-gate range). Packed-3x4 rows
// only; UBT 16-float rows are gated by the layout vote instead.
bool ServedRowsUsable(const float* r, uint32_t rows) {
  for (uint32_t i = 0; i < rows; ++i) {
    const float* m = r + size_t(i) * 12;
    float n = 0.0f;
    for (int j = 0; j < 3; ++j) {
      n += m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j];
    }
    if (!(n > 0.0016f && n < 2000.0f)) {
      return false;
    }
  }
  return true;
}

// The entity whose pack-producing method is on this thread's call stack
// (the producing entity methods are bracketed by the hook layer).
thread_local uint32_t tl_pack_owner = 0;

// Sk8::UpdateBoneTransforms m_matrices layout vote: fresh bank captures
// score the UBT rows; >=75% near/exact over 24 votes locks the
// interpretation, otherwise UBT serving stays off.
std::atomic<int> g_ubt_vote48{0};
std::atomic<int> g_ubt_vote44{0};
std::atomic<int> g_ubt_vote_other{0};
// 0 = voting, 1 = packed 3x4, 2 = 4x4 Matrix44 (transpose on serve),
// -1 = refused (neither interpretation matched fresh bank captures).
std::atomic<int> g_ubt_layout{0};

// Highest blend index + 1 actually referenced by a mesh's vertices, cached
// per mesh (VB content is stable; the serve needs it to pick the right
// per-piece pack among an instance's multi-pass snapshots). g_mu-guarded.
std::unordered_map<uint32_t, uint32_t> g_needed_rows_cache;

// Rate-limited detail logging: returns true for the first `first` events and
// every `every`-th after.
bool LogGate(std::atomic<uint64_t>& counter, uint64_t first, uint64_t every) {
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  return n < first || (every != 0 && n % every == 0);
}
std::atomic<uint64_t> g_log_prim{0};

// Self-validation probe for the cModelInstance layout: the first packed
// palette row must look like a column-vector affine (sane rotation norms,
// bounded translation). 16 samples; >4 failures turns the module off.
void ProbeInstance(const float* row) {
  static std::atomic<uint32_t> s_seen{0};
  static std::atomic<uint32_t> s_fail{0};
  const uint32_t n = s_seen.fetch_add(1, std::memory_order_relaxed);
  if (n >= 16) {
    return;
  }
  bool ok = true;
  // Packed 4x3 column-vector rows: [ r.x r.y r.z t ] x3; column norms of
  // the 3x3 block land in a scale band, translations stay world-plausible.
  for (int c = 0; c < 3 && ok; ++c) {
    const float norm = std::sqrt(row[c] * row[c] + row[4 + c] * row[4 + c] +
                                 row[8 + c] * row[8 + c]);
    ok = norm > 1e-3f && norm < 1e3f && std::isfinite(norm);
  }
  for (int r = 0; r < 3 && ok; ++r) {
    ok = std::isfinite(row[r * 4 + 3]) && std::fabs(row[r * 4 + 3]) < 1e6f;
  }
  if (!ok) {
    s_fail.fetch_add(1, std::memory_order_relaxed);
  }
  if (n == 15) {
    const uint32_t fails = s_fail.load(std::memory_order_relaxed);
    if (fails > 4) {
      g_pal_ok.store(false, std::memory_order_relaxed);
      REXLOG_WARN(
          "native-palette: cModelInstance layout FAILED validation ({}/16 bad "
          "first rows); palette serve disabled (image drift?)",
          fails);
    }
  }
}

// Copy one instance's header + m_matrices rows. ubt = the record came from
// an UpdateBoneTransforms parts array (stride 0x28, same header layout),
// snapshotted at the hook because the parts arena AND its m_matrices buffers
// zero by frame end.
void SnapshotPack(uint8_t* base, uint32_t inst, bool ubt = false) {
  uint32_t head[0x28 / 4];
  if (!GuestTryCopy(head, base + inst, sizeof(head))) {
    return;
  }
  const uint32_t model = __builtin_bswap32(head[kInstModel / 4]);
  const uint32_t arr = __builtin_bswap32(head[kInstArrCtx / 4]);
  const uint32_t arr_end = __builtin_bswap32(head[kInstArrCtxEnd / 4]);
  const uint32_t matrices = __builtin_bswap32(head[kInstMatrices / 4]);
  const uint32_t count = __builtin_bswap32(head[0x18 / 4]);
  if (!PlausiblePtr(model) || !PlausiblePtr(arr) || !PlausiblePtr(matrices)) {
    if (ubt) {
      c_ubt_rej_ptrs.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  PackSnap snap;
  snap.inst = inst;
  snap.entity = tl_pack_owner;
  snap.arr_begin = arr;
  snap.ubt = ubt;
  if (arr_end > arr && (arr_end - arr) % kCtxStride == 0 &&
      (arr_end - arr) / kCtxStride <= 256) {
    snap.arr_bytes = arr_end - arr;
  } else if (ubt) {
    // Live UBT arena records don't always carry a populated end pointer.
    // Structurally each part owns exactly ONE MeshContext, so a one-slot
    // window is the safe assumption; it can only ever match the piece's
    // own ctx.
    snap.arr_bytes = kCtxStride;
    c_ubt_arr_fallback.fetch_add(1, std::memory_order_relaxed);
  } else {
    snap.arr_bytes = kCtxSlotsFallback * kCtxStride;
  }
  // +0x18 tracked as the pack count: it equals the highest blend index the
  // mesh references, so it is trusted within bounds.
  snap.rows = (count >= 1 && count <= 96) ? count : 84;
  // UBT snapshots copy 16 floats/row: rw::math::vpu::Matrix44 is a 64-byte
  // 4x4, and UpdateBoneTransforms (unlike PackAndMultiply) may leave
  // m_matrices in that shape; the serve's layout vote evaluates BOTH the
  // packed-3x4 (12-float rows over the same contiguous span) and the
  // 4x4-transposed interpretation from this one copy.
  const uint32_t fpr = ubt ? 16u : 12u;
  uint32_t raw[96 * 16];
  if (!GuestTryCopy(raw, base + matrices, size_t(snap.rows) * fpr * 4)) {
    c_pal_unreadable.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_packs.size() >= 512 || g_pack_rows.size() > 512 * 1024) {
    return;  // growth cap (scene path off / frame never built)
  }
  snap.row_ofs = uint32_t(g_pack_rows.size());
  g_pack_rows.resize(g_pack_rows.size() + size_t(snap.rows) * fpr);
  float* dst = g_pack_rows.data() + snap.row_ofs;
  for (uint32_t i = 0; i < snap.rows * fpr; ++i) {
    dst[i] = std::bit_cast<float>(__builtin_bswap32(raw[i]));
  }
  g_packs.push_back(snap);
  if (ubt) {
    c_ubt_snap_ok.fetch_add(1, std::memory_order_relaxed);
  }
  // Layout self-probe on the first pack-hook snapshot only: UBT rows carry
  // their own vote-based validation (a failed probe here would kill the
  // whole module for the session on an unproven layout).
  static std::atomic<bool> s_probed{false};
  if (!ubt && !s_probed.exchange(true)) {
    ProbeInstance(dst);
  }
}

// LOD pedestrians. The LivingWorld pack writer fuses UpdateBoneTransforms +
// PackAndMultiply for cLivingWorldPresEntity-derived classes (their vtable
// slot +16 is an Update thunk, so they never reach the pack/UBT hooks) and
// writes the CONCATENATION of per-mesh packed palettes into m_matrices;
// mesh k's rows start at sum(boneCount(0..k-1)). Each mesh slice becomes
// its own PackSnap keyed to exactly its MeshContext, so the containment
// matching works unchanged.
void SnapshotLwEntity(uint8_t* base, uint32_t entity) {
  uint32_t idx = 0, count = 0, arr = 0;
  if (!LoadU32(base, entity + 16, &idx) || idx > 1024 ||
      !LoadU32(base, entity + (idx + 5) * 4, &count) ||
      !LoadU32(base, entity + (idx + 8) * 4, &arr) || count == 0 || count > 8 ||
      !PlausiblePtr(arr)) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rec = arr + i * 0x28;
    uint32_t head[0x28 / 4];
    if (!GuestTryCopy(head, base + rec, sizeof(head))) {
      continue;
    }
    const uint32_t model = __builtin_bswap32(head[kInstModel / 4]);
    const uint32_t actx = __builtin_bswap32(head[kInstArrCtx / 4]);
    const uint32_t matrices = __builtin_bswap32(head[kInstMatrices / 4]);
    if (!PlausiblePtr(model) || !PlausiblePtr(actx) || !PlausiblePtr(matrices)) {
      continue;
    }
    // Model walk mirrors the writer's own: mesh table at +0x24 (ptr array,
    // stride 4), mesh count u16 at +0x32, bone-count gate u16 at +0x34
    // (<= 80, same gate the writer applies). Every bone count must land in
    // 1..96; a wrong table shape fails here and the instance is skipped.
    uint32_t count_w = 0, table = 0, gate_w = 0;
    if (!LoadU32(base, model + 0x30, &count_w) ||
        !LoadU32(base, model + 0x24, &table) ||
        !LoadU32(base, model + 0x34, &gate_w)) {
      continue;
    }
    const uint32_t nmesh = count_w & 0xFFFF;
    if (nmesh == 0 || nmesh > 16 || (gate_w >> 16) > 80) {
      continue;
    }
    uint32_t bones[16];
    uint32_t total = 0;
    bool ok = true;
    for (uint32_t k = 0; k < nmesh && ok; ++k) {
      uint32_t mesh = 0, bc = 0;
      ok = LoadU32(base, table + k * 4, &mesh) && PlausiblePtr(mesh) &&
           LoadU32(base, mesh + 0x48, &bc) && bc >= 1 && bc <= 96;
      if (ok) {
        bones[k] = bc;
        total += bc;
      }
    }
    if (!ok || total == 0 || total > 96) {
      continue;
    }
    uint32_t raw[96 * 12];
    if (!GuestTryCopy(raw, base + matrices, size_t(total) * 48)) {
      continue;
    }
    // Byteswap OUTSIDE g_mu: this runs on the sim thread, and holding the
    // lock through a multi-KB swap loop stalls the guest render thread's
    // per-frame palette consumers.
    float host[96 * 12];
    for (uint32_t f = 0; f < total * 12; ++f) {
      host[f] = std::bit_cast<float>(__builtin_bswap32(raw[f]));
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_packs.size() + nmesh > 512 || g_pack_rows.size() > 512 * 1024) {
      return;
    }
    const uint32_t ofs = uint32_t(g_pack_rows.size());
    g_pack_rows.resize(g_pack_rows.size() + size_t(total) * 12);
    std::memcpy(g_pack_rows.data() + ofs, host, size_t(total) * 12 * sizeof(float));
    uint32_t row = 0;
    for (uint32_t k = 0; k < nmesh; ++k) {
      PackSnap snap;
      snap.inst = rec;
      snap.entity = entity;
      snap.arr_begin = actx + k * kCtxStride;  // exactly this mesh's ctx
      snap.arr_bytes = kCtxStride;
      snap.row_ofs = ofs + row * 12;
      snap.rows = bones[k];
      g_packs.push_back(snap);
      row += bones[k];
    }
  }
}

// Timing-tolerant pose compare, used by the UBT layout vote: the game packs
// per render frame (interpolated poses), so a bank captured at draw time
// legitimately sits within ~1-2 animation steps of any single-point
// snapshot; translations move up to ~0.2/step at skating speed, rotation
// elements only a few hundredths. Wrong rows/layout show rotation deltas
// O(1) and translation deltas of whole limb offsets.
struct RowScore {
  float max_rot = 1e30f;    // max delta over the 3x3 rotation elements
  float max_trans = 1e30f;  // max delta over the translation lane (f%4==3)
  bool exact = false;
  bool near = false;
};

void ScoreRows(const float* auth, uint32_t nrows, const DrawItem& item,
               RowScore* out) {
  float max_rot = 0.0f, max_trans = 0.0f, max_diff = 0.0f;
  for (uint32_t r = 0; r < nrows; ++r) {
    for (int f = 0; f < 12; ++f) {
      const float diff = std::fabs(auth[r * 12 + f] - item.bones[r * 12 + f]);
      ((f % 4 == 3) ? max_trans : max_rot) =
          std::max((f % 4 == 3) ? max_trans : max_rot, diff);
      max_diff = std::max(max_diff, diff);
    }
  }
  out->max_rot = max_rot;
  out->max_trans = max_trans;
  out->exact = max_diff <= 2.5e-3f;
  // Measured pose-step deltas top out ~0.10 rotation / ~0.2 translation
  // during tricks at 140 fps; wrong rows/layout show rotation deltas
  // O(0.5-2.0) and limb-offset translations; the bands sit between.
  out->near = max_rot <= 0.25f && max_trans <= 1.0f;
}

// Highest blend index + 1 referenced by the mesh's own vertices, cached per
// mesh under g_mu (VB content is stable per mesh asset). 0 = undeterminable.
uint32_t NeededRowsCached(uint8_t* base, const DrawItem& item) {
  const auto it = g_needed_rows_cache.find(item.mesh);
  if (it != g_needed_rows_cache.end()) {
    return it->second;
  }
  skate3::native_scene::SkinSampleVert sv[32];
  uint32_t needed = 0;
  if (skate3::native_scene::ReadSkinSamplesGuest(base, item, 32, sv)) {
    needed = 1;
    for (const auto& s : sv) {
      for (int k = 0; k < 4; ++k) {
        if (s.w[k] > 0 && uint32_t(s.bone[k]) + 1 > needed) {
          needed = uint32_t(s.bone[k]) + 1;
        }
      }
    }
  }
  if (g_needed_rows_cache.size() >= 2048) {
    g_needed_rows_cache.clear();  // mesh addresses recycle across map loads
  }
  g_needed_rows_cache.emplace(item.mesh, needed);
  return needed;
}

// 64-byte row-vector Matrix44 (translation in row 3) -> our column-vector
// [R|t] 3x4 rows (transpose the 3x3, splice the translation).
void Convert44Rows(const float* in, uint32_t rows_n, float* out) {
  for (uint32_t r = 0; r < rows_n; ++r) {
    const float* m = in + r * 16;
    float* o = out + r * 12;
    o[0] = m[0]; o[1] = m[4]; o[2] = m[8];  o[3] = m[12];
    o[4] = m[1]; o[5] = m[5]; o[6] = m[9];  o[7] = m[13];
    o[8] = m[2]; o[9] = m[6]; o[10] = m[10]; o[11] = m[14];
  }
}

uint64_t HashRows(const float* p, size_t nfloats) {
  uint64_t h = 1469598103934665603ull;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
  for (size_t i = 0; i < nfloats * sizeof(float); ++i) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

// Per-instance serve history for the hysteresis (ctx-keyed, g_mu-guarded).
struct ServeHist {
  uint64_t h1 = 0;  // last served palette hash
  uint64_t h2 = 0;  // previous-but-one
  uint64_t frame = 0;
  float t[3] = {};  // last served bone-0 translation (the identity anchor)
};
std::unordered_map<uint32_t, ServeHist> g_serve_hist;

uint32_t ServeAuthoritativePalettesImpl(uint8_t* base,
                                        skate3::native_scene::FrameScene& scene) {
  if (!REXCVAR_GET(skate3_native_render_scene_palette_serve) ||
      !g_pal_ok.load(std::memory_order_relaxed)) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_packs.empty() && g_packs_prev.empty()) {
    return 0;
  }
  uint32_t served = 0;
  for (DrawItem& item : scene.items) {
    // Pending items are half-built (the fixup/rescue machinery owns them);
    // LW-store substitutions (dbg_src 10) are already authoritative.
    if (!item.skinned || item.ctx == 0 || item.bones.size() < 12 ||
        item.pending || item.dbg_src == 10) {
      continue;
    }
    const uint32_t published = uint32_t(item.bones.size() / 12);
    const uint32_t needed = NeededRowsCached(base, item);
    // Identity filter: when the ctx -> entity store maps this item, any
    // candidate stamped with a DIFFERENT owning entity is structurally the
    // wrong character; no containment/content/proximity evidence can
    // rehabilitate it. Unstamped candidates and unmapped items keep the
    // content matching below.
    skate3::native_entity::CtxInfo ident;
    const bool ident_mapped =
        skate3::native_entity::LookupCtx(item.ctx, &ident);
    // Entity-exact primary: a candidate stamped with the item's own entity
    // is identity-proven, so the anchor/proximity caps (which exist to
    // reject OTHER characters' packs) do not apply to it, and it outranks
    // any unstamped candidate the content heuristics matched. Tracked as a
    // separate pick so the content matching below stays bit-identical for
    // unmapped items and as the fallback.
    const bool ident_primary =
        ident_mapped && ident.entity != 0 &&
        REXCVAR_GET(skate3_native_render_scene_palette_ident_primary);
    const PackSnap* pick = nullptr;
    const float* pick_rows = nullptr;
    const PackSnap* pick_x = nullptr;
    const float* pick_x_rows = nullptr;
    // Ring of the last few matching candidates (any pass, any sibling): the
    // hysteresis below may switch the serve to whichever candidate
    // CONTINUES the previous serve bit-exactly.
    struct Cand {
      const float* rows;
      uint32_t nrows;
      bool ubt;
    };
    Cand ring[8];
    uint32_t ring_n = 0;
    // Newest set first: this frame's snapshots, then the previous frame's
    // (an instance that didn't pack this frame holds its last pose anyway).
    // WITHIN a set, the LAST matching pack wins: an instance packs once per
    // PASS (casters first, sometimes a stale-sample pose, main pass
    // last), and first-match picking alternates between passes with the
    // per-frame pack order. The last pack is the main-pass pose, i.e. what
    // the game's own main draw consumes. Same-count sibling pieces of one
    // instance-wide window are disambiguated by bone-0 translation against
    // the item's captured bone-0 (different joints sit
    // decimeters-to-meters apart; near-ties within 5 cm are the same joint,
    // where the later pack still wins).
    // Identity anchor for proximity matching: the last SERVED bone-0
    // translation when fresh (stable across bank-capture corruption), the
    // item's captured bone-0 otherwise.
    float anchor[3] = {item.bones[3], item.bones[7], item.bones[11]};
    {
      const auto hit = g_serve_hist.find(item.ctx);
      if (hit != g_serve_hist.end() && g_frames - hit->second.frame <= 60 &&
          (hit->second.t[0] != 0.0f || hit->second.t[1] != 0.0f ||
           hit->second.t[2] != 0.0f)) {
        // A fresh bank capture is identity ground truth: when it sits meters
        // from the served-history anchor, the anchor has been tracking
        // ANOTHER character's packs (each wrong serve re-stamps the anchor
        // at the wrong joint, so the loop is self-sustaining). Bank-capture
        // corruption the anchor exists to ride out is sub-meter; a multi-
        // meter disagreement with a same-frame capture means the anchor is
        // the wrong side.
        const float ax = item.bones[3] - hit->second.t[0];
        const float ay = item.bones[7] - hit->second.t[1];
        const float az = item.bones[11] - hit->second.t[2];
        if (!(item.dbg_src <= 2 && ax * ax + ay * ay + az * az > 9.0f)) {
          anchor[0] = hit->second.t[0];
          anchor[1] = hit->second.t[1];
          anchor[2] = hit->second.t[2];
        }
      }
    }
    for (int set = 0; set < 2; ++set) {
      // The previous-frame set is searched only while the deciding pick is
      // still open (entity-exact when primary applies, content otherwise). A
      // content pick found in an earlier set is locked; later sets may only
      // add the entity-exact pick on top of it.
      if (ident_primary ? pick_x != nullptr : pick != nullptr) {
        break;
      }
      const bool legacy_open = pick == nullptr;
      const auto& packs = set == 0 ? g_packs : g_packs_prev;
      const auto& rows = set == 0 ? g_pack_rows : g_pack_rows_prev;
      float best_d = 1e30f;
      float best_dx = 1e30f;
      for (const PackSnap& snap : packs) {
        if (ident_mapped && snap.entity != 0 && snap.entity != ident.entity) {
          c_pal_serve_ident_skip.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const bool contains =
            item.ctx >= snap.arr_begin &&
            item.ctx < snap.arr_begin + snap.arr_bytes &&
            (item.ctx - snap.arr_begin) % kCtxStride == 0;
        if (snap.ubt && !contains) {
          // Live UBT arena records reference per-frame ARENA MeshContext
          // copies, never the persistent ctx the scene items carry; so ctx
          // containment cannot map them. Match by CONTENT instead: the
          // game's own packed bone count names the piece, joint proximity
          // to the anchor names the character. Translation is read under
          // BOTH row interpretations while the layout vote is still open
          // (packed-3x4: floats 3/7/11; 4x4 row-vector: floats 12/13/14 of
          // the 16-float row).
          if (needed == 0 || snap.rows != needed) {
            continue;
          }
          const float* r = rows.data() + snap.row_ofs;
          const float da = std::sqrt((r[3] - anchor[0]) * (r[3] - anchor[0]) +
                                     (r[7] - anchor[1]) * (r[7] - anchor[1]) +
                                     (r[11] - anchor[2]) * (r[11] - anchor[2]));
          const float db = std::sqrt((r[12] - anchor[0]) * (r[12] - anchor[0]) +
                                     (r[13] - anchor[1]) * (r[13] - anchor[1]) +
                                     (r[14] - anchor[2]) * (r[14] - anchor[2]));
          const float d = std::min(da, db);
          // UBT rows serve only after the layout vote resolves; until then
          // a UBT candidate must not claim the entity-exact pick (it would
          // displace a servable pack candidate and leave the item on its
          // captured palette while the vote is still open).
          const bool entity_exact =
              ident_primary && snap.entity == ident.entity &&
              g_ubt_layout.load(std::memory_order_relaxed) > 0;
          if (d > 1.5f && !entity_exact) {
            continue;  // another character's piece
          }
          ring[ring_n++ & 7] = {r, snap.rows, true};
          if (entity_exact && d < best_dx + 0.05f) {
            pick_x = &snap;
            pick_x_rows = r;
            best_dx = std::min(best_dx, d);
          }
          if (legacy_open && d <= 1.5f && d < best_d + 0.05f) {
            pick = &snap;
            pick_rows = r;
            best_d = std::min(best_d, d);
          }
          continue;
        }
        if (!contains) {
          continue;
        }
        if (snap.arr_bytes == kCtxStride) {
          // Exact per-piece ctx window (UBT part / LW slice): unambiguous;
          // keep overwriting so the frame's last pack wins.
          const float* r = rows.data() + snap.row_ofs;
          if (!snap.ubt && !ServedRowsUsable(r, snap.rows)) {
            c_pal_serve_bad_rows.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          if (legacy_open) {
            pick = &snap;
            pick_rows = r;
            best_d = 0.0f;
          }
          if (ident_primary && snap.entity == ident.entity &&
              (!snap.ubt ||
               g_ubt_layout.load(std::memory_order_relaxed) > 0)) {
            pick_x = &snap;
            pick_x_rows = r;
            best_dx = 0.0f;
          }
          ring[ring_n++ & 7] = {r, snap.rows, snap.ubt};
          continue;
        }
        // Instance-wide window (PackAndMultiply): the packed bone count
        // names the piece among the instance's per-piece packs.
        if (needed == 0 || snap.rows != needed) {
          continue;
        }
        const float* r = rows.data() + snap.row_ofs;
        if (!ServedRowsUsable(r, snap.rows)) {
          c_pal_serve_bad_rows.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const float dx = r[3] - anchor[0];
        const float dy = r[7] - anchor[1];
        const float dz = r[11] - anchor[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool entity_exact = ident_primary && snap.entity == ident.entity;
        if (d > 1.5f && !entity_exact) {
          // Same identity cap as the content-matched path: ctx containment
          // is not proof of identity when instance arrays are reused across
          // presentation contexts (menu demos), and serving a far pack
          // teleports the piece to the other character. With no in-range
          // candidate the item keeps its captured palette.
          continue;
        }
        ring[ring_n++ & 7] = {r, snap.rows, snap.ubt};
        if (entity_exact && d < best_dx + 0.05f) {
          pick_x = &snap;
          pick_x_rows = r;
          best_dx = std::min(best_dx, d);
        }
        if (legacy_open && d <= 1.5f && d < best_d + 0.05f) {
          pick = &snap;
          pick_rows = r;
          best_d = std::min(best_d, d);
        }
      }
    }
    // Promote the entity-exact pick over the content match. A divergence
    // between the two IS the wrong-sibling serve class the primary mode
    // exists to eliminate; count and (capped) name it.
    bool served_exact = false;
    if (pick_x != nullptr) {
      if (pick != nullptr && pick_rows != pick_x_rows) {
        bool differs = pick->ubt != pick_x->ubt || pick->rows != pick_x->rows;
        if (!differs) {
          const size_t nf = size_t(std::min(published, pick->rows)) *
                            (pick->ubt ? 16 : 12);
          differs =
              std::memcmp(pick_rows, pick_x_rows, nf * sizeof(float)) != 0;
        }
        if (differs) {
          c_pal_ident_prim_div.fetch_add(1, std::memory_order_relaxed);
          if (LogGate(g_log_prim, 8, 4096)) {
            REXLOG_INFO(
                "native-palette: entity-exact pick OVERRODE content match "
                "mesh={:08X} ctx={:08X} ent={:08X} content(ent={:08X} "
                "rows={} ubt={} t=({:.2f},{:.2f},{:.2f})) exact(rows={} "
                "ubt={} t=({:.2f},{:.2f},{:.2f}))",
                item.mesh, item.ctx, ident.entity, pick->entity, pick->rows,
                pick->ubt, pick_rows[3], pick_rows[7], pick_rows[11],
                pick_x->rows, pick_x->ubt, pick_x_rows[3], pick_x_rows[7],
                pick_x_rows[11]);
          }
        }
      }
      pick = pick_x;
      pick_rows = pick_x_rows;
      served_exact = true;
    }
    if (pick == nullptr) {
      c_pal_serve_miss.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    uint32_t n = std::min(published, pick->rows);
    float conv[96 * 12];  // loop-scoped: pick_rows may alias it below
    if (pick->ubt) {
      // UpdateBoneTransforms writes rw::math::vpu::Matrix44 (64-byte 4x4)
      // records; whether m_matrices holds those raw or already-packed 3x4
      // rows is image-dependent; vote BOTH interpretations against FRESH
      // bank captures (the game's own upload bytes) before ever serving.
      // The snapshot copied 16 floats/row, so A = the same span sliced as
      // contiguous 12-float rows, B = 4x4 rows transposed (row-vector M,
      // translation in row 3 -> our column-vector [R|t] rows).
      const int layout = g_ubt_layout.load(std::memory_order_relaxed);
      const uint32_t vn = std::min(n, needed ? needed : n);
      if (layout == 0) {
        if (item.dbg_src <= 2 && !item.caster_bank && vn > 0) {
          RowScore sa, sb;
          ScoreRows(pick_rows, vn, item, &sa);
          Convert44Rows(pick_rows, vn, conv);
          ScoreRows(conv, vn, item, &sb);
          ((sa.exact || sa.near)   ? g_ubt_vote48
           : (sb.exact || sb.near) ? g_ubt_vote44
                                   : g_ubt_vote_other)
              .fetch_add(1, std::memory_order_relaxed);
          const int va = g_ubt_vote48.load(std::memory_order_relaxed);
          const int vb = g_ubt_vote44.load(std::memory_order_relaxed);
          const int vo = g_ubt_vote_other.load(std::memory_order_relaxed);
          if (va + vb + vo >= 24) {
            const int total = va + vb + vo;
            const int lock = va * 4 >= total * 3 ? 1 : vb * 4 >= total * 3 ? 2 : -1;
            g_ubt_layout.store(lock, std::memory_order_relaxed);
            REXLOG_INFO(
                "native-palette: UpdateBoneTransforms m_matrices layout {} "
                "(votes: packed-3x4 {} / 4x4-transposed {} / other {})",
                lock == 1   ? "confirmed packed 3x4; player pieces now served"
                : lock == 2 ? "confirmed 4x4 Matrix44; player pieces now "
                              "served via transpose"
                            : "REFUSED (neither read matches); UBT serving "
                              "disabled this session",
                va, vb, vo);
          }
        }
        continue;
      }
      if (layout < 0) {
        continue;
      }
      if (layout == 2) {
        Convert44Rows(pick_rows, n, conv);
        pick_rows = conv;
      }
      // layout == 1: contiguous packed 3x4; serve pick_rows as-is.
    }
    // Serve-history hysteresis: packs run from sim jobs, so per-frame pack
    // ORDER is scheduler-dependent; no order-based pick is stable, and
    // anchoring on the item's captured bone-0 follows the very bank
    // alternation being replaced. Content rule instead: never serve a
    // bit-exact RETURN to the previous-but-one serve while some candidate
    // CONTINUES the last serve; every candidate is a real pack of this
    // piece, so re-serving the last pose is always valid, and when the sim
    // ticks forward no candidate matches h2 so fresh poses serve
    // immediately (the stream can hold or advance, never step backward).
    uint64_t h = HashRows(pick_rows, size_t(n) * 12);
    if (g_serve_hist.size() > 4096) {
      for (auto it = g_serve_hist.begin(); it != g_serve_hist.end();) {
        it = g_frames - it->second.frame > 1200 ? g_serve_hist.erase(it)
                                                : std::next(it);
      }
    }
    ServeHist& hist = g_serve_hist[item.ctx];
    if (hist.h1 != 0 && h == hist.h2 && h != hist.h1) {
      float cbuf[96 * 12];
      const int layout = g_ubt_layout.load(std::memory_order_relaxed);
      for (uint32_t ci = 0; ci < std::min<uint32_t>(ring_n, 8); ++ci) {
        const Cand& c = ring[ci];
        if (std::min(published, c.nrows) != n) {
          continue;
        }
        const float* cr = c.rows;
        if (c.ubt && layout == 2) {
          Convert44Rows(c.rows, n, cbuf);
          cr = cbuf;
        }
        if (HashRows(cr, size_t(n) * 12) == hist.h1) {
          std::memcpy(item.bones.data(), cr, size_t(n) * 12 * sizeof(float));
          std::fill(item.bones.begin() + size_t(n) * 12, item.bones.end(),
                    0.0f);
          item.caster_bank = false;
          item.dbg_src = 11;
          ++served;
          c_pal_hyst.fetch_add(1, std::memory_order_relaxed);
          hist.frame = g_frames;
          hist.t[0] = cr[3];
          hist.t[1] = cr[7];
          hist.t[2] = cr[11];
          goto next_item;
        }
      }
    }
    if (h != hist.h1) {
      hist.h2 = hist.h1;
      hist.h1 = h;
    }
    hist.frame = g_frames;
    hist.t[0] = pick_rows[3];
    hist.t[1] = pick_rows[7];
    hist.t[2] = pick_rows[11];
    std::memcpy(item.bones.data(), pick_rows, size_t(n) * 12 * sizeof(float));
    // Zero the tail: rows beyond the game's own packed count are never
    // indexed by the mesh, and leftover bank-capture garbage there would
    // read as palette churn to any downstream consumer.
    std::fill(item.bones.begin() + size_t(n) * 12, item.bones.end(), 0.0f);
    // The palette no longer comes from a caster bank; downstream stale-class
    // gates (LW substitution, mesh-cache refusal) must treat it as fresh.
    item.caster_bank = false;
    item.dbg_src = 11;
    if (served_exact) {
      c_pal_ident_exact.fetch_add(1, std::memory_order_relaxed);
    } else if (ident_primary) {
      c_pal_ident_fallback.fetch_add(1, std::memory_order_relaxed);
    }
    ++served;
  next_item:;
  }
  c_pal_served.fetch_add(served, std::memory_order_relaxed);
  return served;
}

void MaybeLog() {
  const int32_t interval =
      REXCVAR_GET(skate3_native_render_scene_palette_log_interval);
  if (interval <= 0 || g_frames % uint64_t(interval) != 0) {
    return;
  }
  const auto x = [](std::atomic<uint64_t>& a) {
    return a.exchange(0, std::memory_order_relaxed);
  };
  REXLOG_INFO(
      "native-palette: frame {} srv[hit={} miss={} hyst={} ident_skip={} "
      "bad_rows={} eprim={}/{}/{} unread={}] ubt[48={} 44={} o={} lay={} "
      "snap={} arr={} rej={}]",
      g_frames, x(c_pal_served), x(c_pal_serve_miss), x(c_pal_hyst),
      x(c_pal_serve_ident_skip), x(c_pal_serve_bad_rows), x(c_pal_ident_exact),
      x(c_pal_ident_fallback), x(c_pal_ident_prim_div), x(c_pal_unreadable),
      g_ubt_vote48.load(std::memory_order_relaxed),
      g_ubt_vote44.load(std::memory_order_relaxed),
      g_ubt_vote_other.load(std::memory_order_relaxed),
      g_ubt_layout.load(std::memory_order_relaxed), x(c_ubt_snap_ok),
      x(c_ubt_arr_fallback), x(c_ubt_rej_ptrs));
}

bool CaptureActive() {
  return REXCVAR_GET(skate3_native_render_scene_palette_serve) &&
         g_pal_ok.load(std::memory_order_relaxed);
}

}  // namespace

uint32_t ServeAuthoritativePalettes(uint8_t* base,
                                    skate3::native_scene::FrameScene& scene) {
  return ServeAuthoritativePalettesImpl(base, scene);
}

uint32_t ExchangePackOwner(uint32_t entity) {
  const uint32_t prev = tl_pack_owner;
  tl_pack_owner = entity;
  return prev;
}

void OnPackPalette(uint8_t* base, uint32_t instance) {
  if (instance == 0 || !CaptureActive()) {
    return;
  }
  SnapshotPack(base, instance);
}

void OnBoneTransforms(uint8_t* base, uint32_t parts, uint32_t count) {
  if (parts == 0 || count == 0 || count > 64 || !CaptureActive()) {
    return;
  }
  // Snapshot every part record NOW: the parts array is per-frame arena
  // scratch (all fields read zero by frame end), so this hook exit is the
  // only moment the player skater's per-piece palettes are readable. Each
  // part becomes a per-piece PackSnap (its ctx window is exactly one
  // MeshContext), which maps the player through the same containment
  // matching as every other instance.
  for (uint32_t k = 0; k < count; ++k) {
    SnapshotPack(base, parts + k * 0x28, /*ubt=*/true);
  }
}

void OnLwPack(uint8_t* base, uint32_t entity) {
  if (entity == 0 || !CaptureActive()) {
    return;
  }
  SnapshotLwEntity(base, entity);
}

void OnFrameBuilt() {
  ++g_frames;
  {
    // Rotate this frame's snapshots into the previous-frame slot: the serve
    // at frame N reads g_packs_prev from frame N-1.
    std::lock_guard<std::mutex> lock(g_mu);
    g_packs_prev = std::move(g_packs);
    g_pack_rows_prev = std::move(g_pack_rows);
    g_packs.clear();
    g_pack_rows.clear();
  }
  MaybeLog();
}

}  // namespace skate3::native_palette
