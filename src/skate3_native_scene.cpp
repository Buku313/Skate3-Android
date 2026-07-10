#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/graphics/ultrawide_debug.h>
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
                    "replacing the emulated GPU output (requires skate3_native_render). "
                    "Hot-toggles live between the native and emulated renderers (F5).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lightmaps, true, "Skate 3",
                    "Sample guest lightmap textures in the native scene renderer. The "
                    "old 'lightpages decode black' finding is stale: in gameplay the "
                    "composed atlas payloads are CPU-readable (offline-validated: "
                    "lightmap x2 at the |uv2| unwrap reproduces the emulated baked "
                    "sun/shadow/AO structure), and the texture payload revalidation "
                    "re-decodes pages that were captured before composition.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_debug, 0, "Skate 3",
                     "Native scene debug: 0=normal, 1=clear only, 2=solid color per item, "
                     "3=limit to 20 items, 4=depth test disabled")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_msaa, 4, "Skate 3",
                     "MSAA sample count for the native scene (1 = off, 2/4/8). Distant "
                     "thin geometry (railings, wires) shimmers without it; mipmaps only "
                     "fix texture aliasing.")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_2d, true, "Skate 3",
                    "Replay the game's 2D/APT (Flash HUD) draws as a native overlay pass "
                    "on top of the 3D scene")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_splines, true, "Skate 3",
                    "Replay the game's in-world neon spline draws (waypoint arrows, "
                    "marker beams; spline_darken/spline_default shaders) inside the "
                    "native scene pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_selection_outline, true, "Skate 3",
                    "Replay the park-editor / object-mover selected-object outline: the "
                    "game stencil-marks the selected object after the sky and a postfx "
                    "edge-detect adds the blue contour (postfx_edgedetectstencil port)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_quadlists, false, "Skate 3",
                    "Render captured non-indexed quad-list draws. Off by default: every "
                    "quad-list capture seen so far is a PARTICLE system (disjoint 2-4cm "
                    "sprites), which renders as floating white squares without the game's "
                    "sprite textures and blending.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_mesh_decode_budget, 0, "Skate 3",
                     "Max INLINE mesh decodes per rendered frame (0 = unlimited). Only "
                     "dynamic payloads (skinned/cloth/ropa, whose buffers change every "
                     "frame) decode inline on the render thread; static world meshes "
                     "and all textures decode asynchronously on the prewarm workers.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_warmup_budget_ms, 8, "Skate 3",
                     "Per-frame milliseconds of the post-takeover settle decode pass: "
                     "after a loading screen the native output takes over on the first "
                     "substantial post-load scene (the registration prewarm decoded the "
                     "world behind the load), and for a short window this budget mops "
                     "up whatever prewarm missed while the draw path's miss budgets "
                     "are clamped; leftovers render white/skip for a frame instead of "
                     "freezing. 0 disables the takeover gates + settle pass entirely "
                     "(legacy immediate behavior, stale-scene flash and all).")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_warmup_min_items, 32, "Skate 3",
                     "Scene item count below which warmup keeps yielding to the "
                     "emulated output: right after the gameplay flip the capture holds "
                     "only a handful of items while the game is still fading in, and "
                     "taking over then shows a black/empty world. Every real gameplay "
                     "scene measures 100+ items.")
    .range(0, 10000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_prewarm_budget_ms, 32, "Skate 3",
                     "Per-frame milliseconds spent decoding freshly REGISTERED world "
                     "meshes (AddRenderInstance hook) and their textures while the "
                     "loading screen is up; the heavy lifting happens behind the load, "
                     "so gameplay starts with hot caches and takeover is immediate. "
                     "The loading screen renders emulated at hundreds of fps, so even "
                     "32 ms/frame keeps it ~30 fps; map-change loads register most of "
                     "the world in their FINAL seconds, so the drain rate in that "
                     "window decides how much pop-in survives into gameplay. "
                     "0 disables registration prewarm.")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_snapshot_all_draws, false, "Skate 3",
                    "Record the draw stream on every recorded frame instead of 2 of every "
                    "60 (large .draws.bin; for targeted investigations)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_shadows, true, "Skate 3",
                    "Render the game's dynamic CSM shadows natively (skater, NPCs, "
                    "movable props onto the world). "
                    "Cascade matrices are captured per frame from the game's own "
                    "material constants.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_shadow_tile, 1024, "Skate 3",
                     "Shadow cascade tile resolution. The game renders 512, but the "
                     "emulated baseline renders the pass resolution-scaled; 1024 "
                     "matches the edge crispness players currently see.")
    .range(256, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Fine-grained feature gates for the F12 native-render debug dialog: each
// isolates one subsystem so regressions (flicker, wrong shading) can be
// bisected live without rebuilds. All hot-reload, default on.
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_macro, true, "Skate 3",
                    "Apply the macrooverlay grime/crack multiply on world materials")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_decals, true, "Skate 3",
                    "Composite environment.decal art (graffiti/paint) over base diffuse")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_transparents, true, "Skate 3",
                    "Draw environment.transparent items (mist/glass/fences) in the "
                    "alpha-blended sub-pass")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_world_items, true, "Skate 3",
                    "Publish world sort-list items (static geometry)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dynamic_items, true, "Skate 3",
                    "Publish dynamic entities (characters, props, cloth)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_revalidate, true, "Skate 3",
                    "Re-fingerprint cached texture payloads every 16 frames and "
                    "re-decode on change (heals late-composed lightmap pages)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_mesh_revalidate, true, "Skate 3",
                    "Re-decode cached meshes when their payload fingerprint changes "
                    "(streaming arena reuse; also picks up CPU-animated buffers)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_smooth_camera, true, "Skate 3",
                    "Re-time the camera on the host clock: the guest publishes new "
                    "camera poses on its own sim tick (~170-240 Hz, irregular: "
                    "measured streaks of 10 rendered frames on one pose at 400 fps), "
                    "which reads as the world juddering/skipping while panning. "
                    "Interpolates between the last two distinct guest poses, one "
                    "sim-interval behind (a few ms of added camera latency, no "
                    "overshoot). Teleports/cuts snap.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(
    skate3_native_render_scene_smooth_camera_filter_ms, 50.0, "Skate 3",
    "Boxcar filter window (ms) applied to the smoothed camera pose, centered "
    "on the playback point. The game's camera pose VALUES advance in 60 "
    "Hz-quantized lumps at high render rates (measured: +-2.2 deg off a "
    "constant-rate stick pan, velocity CV 0.84); 50 ms = three 60 Hz "
    "periods nulls the quantization at any render rate (measured 185 -> 7 "
    "deg/s rms frame-to-frame velocity jitter) for ~25 ms extra camera "
    "latency. 0 = off (raw sample interpolation).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Synthetic camera pan: a judder-isolation probe, not a feature. Replaces
// the camera with a host-generated constant-rate horizontal pan so each
// pipeline stage can be judged against a mathematically smooth ground truth
// (the guest's own camera path is irregular; smooth output can never be
// eyeballed against it). Modes pick the injection stage:
//   1 = time-based: pose is a pure function of the host clock at scene-build
//       time, bypassing guest pose sampling AND the smoother entirely. Still
//       judders => the fault is downstream (frame pacing / present / display).
//   2 = fixed-step: pose advances a constant angle per PUBLISHED FRAME
//       (ignores time). Smooth only if displayed frames appear at even
//       intervals, the complement of mode 1 (irregular pacing that mode 1's
//       time base compensates for shows up here, and vice versa).
//   3 = through-smoother: the ~1 kHz sampler thread synthesizes guest-like
//       pose samples (~200 Hz, deliberately irregular) and the normal
//       SmoothCamera reconstruction runs on them; reconstruction error vs
//       the known ideal is measured numerically (err_deg in the log line).
//       Judders here but not in mode 1 => the smoother is at fault.
REXCVAR_DEFINE_INT32(skate3_native_render_scene_synthetic_pan, 0, "Skate 3",
                     "Synthetic constant-rate camera pan (judder isolation): 0 = off, "
                     "1 = time-based at scene build, 2 = fixed angle step per frame, "
                     "3 = synthetic samples through the camera smoother. Hotkey P "
                     "cycles the modes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_synthetic_pan_rate, 90.0, "Skate 3",
                      "Synthetic pan rate in degrees/second")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_synthetic_pan_amp, 0.0, "Skate 3",
                      "Synthetic pan amplitude in degrees: 0 = continuous full 360 "
                      "rotation (sweep the REAL camera around once with the stick "
                      "after engaging; the probe accumulates a union of every static "
                      "item the game submits, filling in the full surround); > 0 = "
                      "triangle-wave +-amp around the engage heading.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_sort_opaque, true, "Skate 3",
                    "Draw opaque scene items front-to-back (bbox-center camera "
                    "distance). Early-z rejects occluded pixels before the heavy "
                    "material shading runs; the game's own sort order is by "
                    "render state, not depth.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_mips, true, "Skate 3",
                    "Upload guest texture MIP CHAINS (off = mip 0 only; distant "
                    "surfaces alias but mip-related artifacts disappear). Flush the "
                    "texture cache after toggling.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Hook layer master switch, defined in skate3_native_render.cpp. The runtime
// toggle refuses to flip the scene on without it: the hooks that feed the
// scene are only installed when it was set at boot.
REXCVAR_DECLARE(bool, skate3_native_render);

namespace skate3::native_scene {
namespace {

// Verified guest structure offsets.
constexpr uint32_t kCtxEffectList = 0x04;
constexpr uint32_t kEffectListPassCount = 0x44;
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
// Published scene: immutable once published (BuildFrameScene builds a fresh
// one per guest frame). shared_ptr so the render thread grabs a reference
// under the mutex instead of deep-copying ~800 items (with per-item heap
// vectors) every frame.
std::shared_ptr<const FrameScene> g_scene;
uint64_t g_generation = 0;
// Debug-dialog cache flushes: consumed at the top of RenderScene so texture/
// mesh-affecting toggles (mip chains, 565 fixes, ...) take effect immediately
// instead of only for newly streamed content.
std::atomic<bool> g_flush_textures{false};
std::atomic<bool> g_flush_meshes{false};

// ---- Camera-signal recorder (judder diagnosis) -----------------------------
// Records the guest camera SIGNAL during a real stick pan: every distinct
// pose the ~1 kHz sampler accepts (kind 0, precise host timestamps), the raw
// per-frame pose at scene build (kind 2) and the smoother's output (kind 1,
// with its playback time). Written as logs/cam_signal_<ts>.csv when the
// window closes; offline analysis computes tick-by-tick angular velocity to
// show whether the game's own pose sequence is irregular at the source
// (interpolation faithfully reproduces jerky inputs; the synthetic-pan probe
// already exonerated everything downstream).
struct CamSigEntry {
  double t;       // host time (sampler push / frame build)
  double play_t;  // kind 1 only: the smoother's playback time
  float yaw;      // heading, degrees
  uint8_t kind;   // 0 = raw sampler pose, 1 = smoothed frame, 2 = raw frame
};
std::mutex g_camsig_mutex;
std::vector<CamSigEntry> g_camsig;
std::atomic<double> g_camsig_deadline{0.0};  // record until this host time

// Heading from a row-vector view matrix: camera forward in world space is
// the view rotation's third column (v[2], v[6], v[10]).
float YawFromViewRows(const float view[16]) {
  return std::atan2(view[2], view[10]) * float(180.0 / 3.14159265358979323846);
}

// ---- Bone-signal recorder (wheel-guard diagnosis) --------------------------
// Records the raw per-entity pose stream (every ring push: bone palettes /
// rigid worlds with timestamps, kind 0) plus what the interpolation actually
// output each frame (kind 1, with the playback time) to
// logs/bone_signal_<ts>.bin. Offline analysis replays candidate filter/guard
// strategies against the REAL board data and scores wheel-vs-deck lag/orbit
// numerically before anything ships (the cam_signal playbook). Binary
// records after a "BSIG1\n" header: u8 kind, u64 key, f64 t, f64 play,
// u32 n, float[n]. Guest render thread only (written off-thread at close).
std::vector<uint8_t> g_bonesig;
std::atomic<double> g_bonesig_deadline{0.0};
// Armed from the UI thread (F12 button), consumed on the guest thread; the
// blob itself is guest-thread-only.
std::atomic<double> g_bonesig_request{0.0};

void BoneSigAppend(uint8_t kind, uint64_t key, double t, double play, const float* v,
                   uint32_t n) {
  if (g_bonesig.size() > (200u << 20)) {
    return;  // runaway cap
  }
  const size_t off = g_bonesig.size();
  g_bonesig.resize(off + 29 + size_t(n) * 4);
  uint8_t* p = g_bonesig.data() + off;
  *p = kind;
  std::memcpy(p + 1, &key, 8);
  std::memcpy(p + 9, &t, 8);
  std::memcpy(p + 17, &play, 8);
  std::memcpy(p + 25, &n, 4);
  std::memcpy(p + 29, v, size_t(n) * 4);
}
std::atomic<uint8_t*> g_guest_base{nullptr};
std::atomic<uint64_t> g_frames_rendered{0};

// Loading -> gameplay takeover. Armed while the game reports menus/loading
// (and by the F5 enable toggle, hence the atomic); the loading-screen
// prewarm (below) does the decode heavy lifting behind the load, and the
// FIRST substantial post-load scene renders natively immediately, no
// emulated-gameplay stretch. The frames right after takeover run a budgeted
// "settle" decode pass for whatever prewarm missed (dynamic entities,
// late-registered textures); the draw path's miss budgets are clamped while
// settling so leftovers render white/skip for a frame instead of freezing
// the takeover frame. Everything except the armed flag is touched on the
// render thread only, inside RenderScene.
std::atomic<bool> g_warmup_armed{true};
// BuildFrameScene stops publishing during loading (no submissions / no
// perspective view), so g_scene holds the PREVIOUS map's scene through the
// whole loading screen. Only scenes published at or after this generation
// (stamped while the loading screen shows) are eligible for takeover;
// rendering the stale scene shows old-map garbage at the flip.
uint64_t g_warmup_fresh_generation = 0;
// Settle window: frames (of the native frame counter) still running the
// per-frame settle decode pass after a takeover.
uint64_t g_settle_until_frame = 0;

// Loading-screen prewarm queue: world meshes pushed by the
// tROptiMeshData::Unfix hook (the rw-arena LOAD-time pointer resolve; it
// fires once per world mesh as its arena streams in, i.e. exactly during
// loading screens and gameplay world streaming). The render thread drains
// the queue in RenderScene: with the prewarm budget behind the loading
// screen (where the heavy lifting belongs), with the warmup budget's
// leftover while warming, and with a small fixed slice during gameplay
// (streamed-in areas decode before their first draw instead of hitching
// it). Entries whose buffer objects are not initialized yet retry a bounded
// number of drains.
struct PrewarmEntry {
  uint32_t mesh;
  uint16_t retries;
  // Texture-only entry (mesh == 0): a steady-state draw-path texture miss
  // routed to the workers (see EnqueueTexMiss).
  uint32_t tex = 0;
  // Fetch-words entry (mesh == 0, tex == 0, wkey != 0): a words-keyed
  // texture miss (streamed-artwork posters/event ads: no guest texture
  // object to key on; see EnqueueWordsMiss).
  uint64_t wkey = 0;
  uint32_t words[6] = {};
  // Environment-cube entry (tex != 0 && cube): a cube-cache miss: a single
  // cube decode measured up to ~100 ms, the largest remaining traversal
  // hitch when a vehicle / reflective area streamed in.
  bool cube = false;
};
std::mutex g_prewarm_mutex;
std::condition_variable g_prewarm_cv;  // wakes the decode workers
std::vector<PrewarmEntry> g_prewarm_queue;
// Steady-state draw-path misses currently being decoded on the workers (one
// in-flight decode per key; the commit erases). Guarded by g_prewarm_mutex.
// This is the panning-hitch fix: a texture decode averages ~10 ms (max ~70 ms
// measured) and a pan surfaces dozens of new textures/meshes in one frame;
// decoding them inline on the render thread was the lag spike. Misses now
// render white / keep the previous decode for the 1-3 frames the workers
// need.
std::unordered_set<uint32_t> g_miss_inflight_mesh;
std::unordered_set<uint32_t> g_miss_inflight_tex;
std::unordered_set<uint64_t> g_miss_inflight_words;
// Dynamic-payload decode jobs: cloth-sim garments (ropa) have their vertex
// buffers rewritten by the CPU sim EVERY frame, so their mesh decode used to
// run inline on the render thread every frame (~0.7 ms per garment; a
// 4-garment outfit = ~2.9 ms = the 160 fps cap during real play once
// everything else was fixed; the demo character wears no ropa, which hid
// it). The guest thread snapshots the changed payload at publish time; a
// worker converts it into fresh upload buffers; PrewarmCommit swaps the mesh
// cache entry. The render thread keeps drawing the previous decode, one
// frame of cloth lag, invisible (the ropa sim is frame-paced anyway).
struct DynDecodeJob {
  DrawItem item;            // offsets/formats/fingerprint (bones not needed)
  uint64_t seq = 0;         // enqueue order; the commit drops stale results
  std::vector<uint8_t> vb;  // big-endian guest payload snapshots
  std::vector<uint8_t> ib;
};
std::vector<DynDecodeJob> g_dyn_jobs;  // FIFO; guarded by g_prewarm_mutex
// Push-side dedupe: clone instances share one tRModelData, so the same mesh
// registers many times per load. Cleared when the game enters menus/loading
// (arena addresses are reused across map loads).
std::unordered_set<uint32_t> g_prewarm_seen;
// Texture-object dedupe for the workers (they cannot read the render
// thread's g_r caches); same lifetime as g_prewarm_seen.
std::unordered_set<uint32_t> g_prewarm_tex_seen;
std::atomic<uint64_t> g_prewarm_done{0};     // entries fully warm-decoded
std::atomic<uint64_t> g_prewarm_dropped{0};  // entries given up on
std::atomic<bool> g_prewarm_workers_started{false};
// tInstance::m_pRModel offset, confirmed at runtime by the material
// cross-check probe in OnAddRenderInstance (Skate 2 layout: +0x78).
std::atomic<uint32_t> g_instance_rmodel_offset{0};

// guid -> guest renderengine::Texture, from the RegisterTexture hook. Keys
// masked of the top bit: material channel guids carry an extra flag bit
// relative to the registered cAssetIDs.
constexpr uint64_t kGuidMask = 0x7FFFFFFFFFFFFFFFull;
std::mutex g_texture_map_mutex;
std::unordered_map<uint64_t, uint32_t> g_texture_map;

// VS constant staging bank (bank id 0x4000, device+1920).
// D3D::SetPending_AluConstants(device, dirty group mask, bank, ptr) is
// called from inside the guest Draw* functions; ptr is the device's
// positional 256-register shadow (partial uploads leave the rest intact).
// Two verified layouts: pre-pass keeps the per-draw transform / bone
// palette at c4; main-pass keeps camera at c4, palette at c7, rigid world
// at c8..c11 (see BankPaletteBase / BankRigidWorld). Because the dynamic
// capture runs right after RenderMesh returns, the bank holds exactly that
// mesh's constants when its draws ran inline.
std::atomic<uint32_t> g_vs_bank{0};
// Pixel constant shadow bank (bank id 0x4400, device+6016): material colors
// (e.g. the CAS hair tint) are staged here per draw.
std::atomic<uint32_t> g_ps_bank{0};
// Global per-frame fog rows (see FrameScene::fog_ramp): main-pass VS banks
// keep the camera at c4 and the frame's fog params at c5 (ramp) / c6 (color)
// - identical across every main-pass draw of a frame (verified from a
// recorded draw stream). OnDrawDone grabs them from the first 3D draw whose
// c4 matches the last built scene's camera; BuildFrameScene publishes them
// and re-arms the capture. All on the guest render thread.
float g_fog_cam[3] = {};
float g_fog_rows[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
bool g_fog_have = false;
bool g_fog_frame_done = false;
// Sky-dome viewpos height (see FrameScene::sky_height): sky.fx's VS adds
// g_vViewPos to every dome vertex, but the game feeds the SKY draw a viewpos
// whose Y is a fixed level elevation (165.0 in every Port Carverton capture)
// instead of the camera's; the baked skyline must not bob with the skater.
// Captured from the sky bank's unique signature: c4.xz matches the camera
// while c4.y sits far above it (every other main-pass draw has c4 == camera;
// water-reflection passes mirror y DOWNWARD and are excluded by dy > 0).
float g_sky_height = 165.0f;  // every Port Carverton capture to date
bool g_sky_have = false;
bool g_sky_frame_done = false;
// UI background blur (see FrameScene::ui_blur and kBlurShaderSource): while
// a frontend popup is up the game appends blur_hBlur/vBlur + basictex passes
// after the postfx uber. g_ui_blur holds the captured kernel scale (PS c0.x,
// 8 in every capture); g_ui_blur_seen latches per frame on the blur_hBlurPS
// draw and is cleared at publish; the pass chain only exists while the
// popup is actually up, so this can never stick on.
float g_ui_blur = 8.0f;
bool g_ui_blur_seen = false;
// Hold the blur across publishes that carry no blur draw: the game does not
// issue the pass chain on every swap while the popup is up, and publishing
// the raw per-swap flag alternated blur on/off: a visible brightness
// shimmer. Two publishes of hysteresis bridges the gaps; on popup close the
// blur lingers ~2 guest frames, imperceptible.
int g_ui_blur_hold = 0;
// Selected-object outline capture (see DrawItem::selected): in the park
// editor / object mover the game EXCLUDES the selected object from the main
// color pass and re-draws it right after the sky, twice, back to back, the
// stencil-marking passes the postfx edge-detect turns into the blue contour
// (verified in capture: consecutive environmentpark decal +
// diffuse parts, identical VS banks, vs the same mesh at OTHER placements in
// the main pass). Hook side records each post-sky environmentpark/
// dynamicobject draw's (ib, vb, world translation); entries seen >= 2 times
// mark the matching published items. Guest render thread only, like the fog
// capture state.
struct SelectedDrawKey {
  uint32_t ib;
  uint32_t vb;
  float t[3];
  uint32_t count;
};
bool g_sky_seen_this_frame = false;
std::vector<SelectedDrawKey> g_frame_selected;
// The guest issued its postfx_edgedetectstencil draw this frame: the game's
// own "an outline is being drawn" signal (park editor / object mover with an
// active selection). Post-sky double-draws alone are NOT sufficient evidence
// of a selection: normal gameplay draws some small props twice after the sky
// (far LOD/imposter passes), which used to sprout phantom outlines on distant
// objects. Guest render thread only; reset per frame in BuildFrameScene.
bool g_outline_edge_seen = false;
// Outline color, refreshed from the guest postfx_edgedetectstencil draw's
// PS c0 (the park-editor blue in every capture) whenever that pass runs.
float g_outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
// PIXEL banks keep the CSM constants at c0..c8, pass-global (identical on
// every environment-family draw of the pass; character/hair/tree PSes
// allocate differently and are rejected by the sanity gate). Captured on the
// same camera-keyed main-pass draws as the fog rows.
float g_shadow_rows[48] = {};
bool g_shadow_have = false;
bool g_shadow_frame_done = false;
// Frame-global rows of the tree / proxyworld shader families (see
// FrameScene::family_rows), captured from their PS banks when a draw with
// the matching debug path runs. Guest render thread only.
float g_family_rows[4] = {0.3435f, 0.02f, 1.0f, 0.45f};
bool g_tree_frame_done = false;
bool g_proxy_frame_done = false;
// dynamicobject.fx frame-global lighting rows (see FrameScene::dynobj_rows),
// captured from a dynamicobject/alphatestdynamicobject PS bank (debug-path
// classified). Guest render thread only.
float g_dynobj_rows[10] = {};
bool g_dynobj_have = false;
bool g_dynobj_frame_done = false;
// Dynamic entities (characters, movable props) live entirely in transient
// per-frame arenas: the context, its record, mesh pointers, transforms and
// bone palettes are all recycled before frame end. The complete DrawItem is
// therefore built at RenderMesh hook time and stored here.
std::mutex g_palette_mutex;
std::vector<DrawItem> g_frame_dynitems;
// (ib_obj << 32 | vb_obj) -> indices of pending items in g_frame_dynitems,
// for the post-draw state fixup (deferred / world-path captures draw after
// their capture; the fixup fills palette or world from the actual draw).
std::unordered_multimap<uint64_t, size_t> g_frame_pending_by_buffers;
// (ib_obj << 32 | vb_obj) -> dynitem indices whose character-lighting rows
// should refresh on every later matching draw this frame. The one-shot
// palette fixup usually pops on the shadow-CASTER draw, whose PS bank is
// stale (shadowPS uploads no PS constants); re-capturing on the main-pass
// draw (last writer) lands the real per-instance rows (stamp tints etc.).
std::unordered_multimap<uint64_t, size_t> g_frame_char_refresh;
// (ib_obj << 32 | vb_obj) -> device fetch-shadow slots 3+4 (12 dwords) at
// this frame's LAST indexed 3D draw with those buffers (the z-prepass draws
// first, so the main pass wins). Consumed at frame end by the
// streamed-artwork diffuse override (DrawItem::diffuse_fetch): poster/advert
// materials keep a 16x16 min-mip placeholder in the material channel while
// the engine binds the streamed art via the fetch constants at draw time.
// Guarded by g_palette_mutex; cleared at the end of BuildFrameScene.
std::unordered_map<uint64_t, std::array<uint32_t, 12>> g_frame_draw_fetch;
// mesh -> last VALIDATED character-lighting rows (see CaptureCharLighting).
// The per-frame capture depends on a fragile draw-order chain (capture at
// submit-exit -> pending fixup -> per-draw refresh, commit-on-success); on
// frames where no draw in the chain carried this character's main-pass PS
// bank the item ends with INVALID rows and the renderer falls back to the
// legacy empirical shading; the player visibly flickered between the two
// looks while moving, and the hair left the blended sub-pass (opaque "hair
// helmet" covering the cap crown). The rows are frame-coherent (sun/ambient
// move slowly), so reusing the previous valid capture is visually seamless.
// TRAP: NPC clones share a mesh with PER-INSTANCE rows (stamp tints, the
// teal-vest twins), so the fallback applies only to meshes with a single
// instance in the frame. Guest-render-thread only (capture hooks +
// BuildFrameScene), no lock needed.
std::unordered_map<uint32_t, std::array<float, 60>> g_char_rows_cache;
std::atomic<uint64_t> g_char_rows_reused{0};
// mesh -> last published bone palette (skinned single-instance meshes).
// Rescue for frames whose palette capture was REFUSED by the acceptance
// gates (RefinePaletteBase returning 0 on a stale/ambiguous bank): garments
// that draw only once per frame (the trucker cap) have no later draw for
// the fixup to pop on, so a refused capture used to mean a one-frame
// disappearance (the "momentary blip" while rotating the camera). One frame
// of pose lag is invisible; a missing hat is not. Single-instance only;
// clones share meshes with per-instance poses. Guest-render-thread only.
std::unordered_map<uint32_t, std::vector<float>> g_bones_cache;
std::atomic<uint64_t> g_bones_rescued{0};
// mesh -> last RESOLVED ropa garment state (rigid world OR skinned palette).
// Ropa items must NOT ride the g_bones_cache rescue above: a ropa capture is
// refused exactly when the bank is stale (g_ropa_stale), and while the cloth
// sim is ACTIVE the VB holds sim-deformed root-local positions; skinning
// those with a cached palette is the mangled-ribbon interpretation (verified
// 0/31 in-clip; on-screen as map-length stretched strips, with matching
// shadows since the caster pass shares the item). And with no rescue at all
// a refused frame drops the garment (the momentary invisible torso). The
// dyn decode workers already keep the drawn cloth VB one frame behind the
// sim, so re-publishing last frame's resolved MODE + transform is exactly
// age-consistent with the vertices being drawn. Single-instance meshes only,
// like g_bones_cache; clones share meshes with per-instance transforms.
// Guest-render-thread only.
struct RopaResolvedState {
  bool skinned = false;
  float world[16] = {};
  std::vector<float> bones;
};
std::unordered_map<uint32_t, RopaResolvedState> g_ropa_state_cache;
std::atomic<uint64_t> g_ropa_rescued{0};
std::atomic<uint32_t> g_cur_ib{0};
std::atomic<uint32_t> g_cur_vb{0};
std::atomic<uint32_t> g_cur_vb_offset{0};
std::atomic<uint32_t> g_cur_vb_stride{0};
// Guest D3D::CDevice: the fetch-constant shadow lives at device+0x480,
// 6 dwords per fetch group (from recompiled SetPending_FetchConstants).
std::atomic<uint32_t> g_device{0};
// 2D/APT phase depth counters (see On2dPhase). All HUD/menu 2D elements are
// Flash SWFs converted to APT; draws issued inside these brackets are the 2D
// draw stream.
std::atomic<uint32_t> g_phase2d_depth[6];
std::atomic<uint64_t> g_draws_2d{0};
// 2D draws seen through paths the overlay does not replay yet
// (DrawIndexedVertices / DrawVertices in the 2D phase; gameplay HUD uses
// only the BeginVertices inline path, verified by capture).
std::atomic<uint64_t> g_draws_2d_other{0};
std::atomic<uint64_t> g_draws_2d_dropped{0};

// One captured 2D draw (verified layout): BeginVertices
// inline vertices, stride 24 = {float x, y, z, w; float u, v} in 1280x720
// APT movie space; VS c0..c3 = ortho projection rows, c4..c7 = the
// element's 2D transform, c8 = color multiplier; diffuse texture in fetch
// shadow slot 0. Vertex payload is written by the CPU after BeginVertices
// returns, so it is read at frame end (addr) rather than at capture.
// NOTE on render-to-texture: in gameplay the ENTIRE HUD renders inside
// AptRenderingIntegration::UpdateRenderToTexture (bracket bit 3) into a
// screen-sized overlay texture, at true screen coordinates; the game
// composites that overlay over the 3D frame in a later (emulated,
// suppressed) pass. Replaying the draws directly onto the native output IS
// that composite, so bit 3 is diagnostic only, not a routing signal. The
// overlay content is straight-alpha art (verified by decoding the clock
// face/needle/sheen textures offline).
struct Draw2d {
  uint32_t prim;    // xenos primitive: 4 trilist, 5 tristrip, 13 quadlist
  uint32_t count;   // vertex count
  uint32_t stride;  // bytes per vertex at capture (see publish normalization)
  uint32_t addr;    // guest inline-ring write pointer
  uint32_t flags;   // bracket bits at capture (layout disambiguation)
  uint32_t fetch[6];  // texture fetch constant (shadow slot 0)
  float consts[36];   // VS c0..c8
  std::vector<uint8_t> verts;  // filled at frame end (little-endian dwords)
};
std::mutex g_2d_mutex;
std::vector<Draw2d> g_frame_2d;  // capture in submission order
std::vector<Draw2d> g_scene_2d;  // published at frame end
uint64_t g_scene_2d_generation = 0;

// One captured in-world neon spline draw (waypoint arrows / marker beams;
// decoded from a live capture + the game's own spline.fx source): a
// DrawVertices triangle strip whose 12-byte float3 "vertices" are only
// parameters: x = control-point index + fractional t, y = U texcoord,
// z = side flag (0/1) selecting the extrusion offset and V texcoord. The
// guest VS (43309A8C) evaluates a uniform cubic B-spline through i_cp[]
// (VS c7..) transformed by world columns c4..c6, adds the world-rotated
// extrusion offset c[151 + z], projects by VP columns c0..c3, and fades by
// clip-z against i_clipvalues (c150) with i_intensity (c149) as the pass
// gain. Two passes per element: spline_darkenPS (straight alpha) then
// spline_defaultPS (additive glow), both depth-tested with no z-write,
// replayed inside the native scene pass with the spline evaluated on the
// CPU at publish time.
struct SplineDraw {
  uint32_t pass;      // 1 = darken (straight alpha), 2 = default (additive)
  uint32_t count;     // strip vertex count
  uint32_t fetch[6];  // texture fetch slot 3 (the neon gradient)
  float consts[153 * 4];  // VS c0..c152 as staged
  // capture: raw big-endian float3 params; publish: evaluated little-endian
  // {float4 clip_pos, float2 uv, float fade} (28-byte stride).
  std::vector<uint8_t> verts;
};
std::vector<SplineDraw> g_frame_spline;  // guarded by g_2d_mutex
std::vector<SplineDraw> g_scene_spline;
std::atomic<uint64_t> g_draws_spline{0};
// Current guest shader objects and render-state shadow bank, for the 2D
// recon recording (grouping the stream by shader / reading blend state).
std::atomic<uint32_t> g_cur_ps_obj{0};
std::atomic<uint32_t> g_cur_vs_obj{0};
std::atomic<uint32_t> g_rs_bank{0};
std::atomic<uint32_t> g_cur_viewport[6];
std::atomic<uint32_t> g_cur_scissor[4];
// All four vertex streams (vb, offset, stride); cloth binds its simulated
// vertices on a stream other than 0.
std::atomic<uint32_t> g_cur_streams[4][3];

// Offline-analysis recording (see StartRecording/WriteRecording): the full
// per-draw constant bank stream is the ground truth for what the game's
// shaders saw; the per-frame item lists are what our pipeline made of it.
struct RecordedDraw {
  uint32_t frame;
  uint32_t func;  // 0 = DrawIndexedVertices, 1 = DrawVertices, 2 = BeginVertices
  uint32_t ib;
  uint32_t vb;
  uint32_t vb_offset;      // stream-0 OffsetInBytes at bind time
  uint32_t vb_stride;      // stream-0 stride at bind time
  uint32_t streams[4][3];  // all streams: {vb, offset, stride}
  uint32_t vfetch[12];     // fetch-constant shadow groups 0-1 (device+0x480)
  uint32_t args[4];
  float bank[1024];  // VS c0..c255
  float ps[256];     // PS c0..c63 (material colors, e.g. CAS hair tint)
  // 2D recon fields (SK3DRAW7):
  uint32_t flags2d;      // bit per active 2D bracket (see On2dPhase)
  uint32_t ps_obj;       // current guest pixel/vertex shader objects
  uint32_t vs_obj;
  uint32_t viewport[6];  // last SetViewport payload (raw guest dwords)
  uint32_t scissor[4];   // last SetScissorRect payload
  uint32_t rstates[256];    // render-state shadow snapshot (blend/depth)
  uint32_t vfetch_all[192];  // full 32-slot texture fetch shadow
  uint32_t vb_dump;  // index into buffers.bin for this draw's payloads
  uint32_t ib_dump;  // (~0u = none)
};
struct RecordedFrame {
  uint64_t generation;
  float view_proj[16];
  float cam_pos[3];
  std::vector<DrawItem> dynitems;
  std::vector<DrawItem> items;
};
// Dynamic meshes live in streaming arenas that are recycled DURING a long
// recording; the end-of-window .gsnap holds garbage for them. Dump each
// captured buffer payload once per content fingerprint so offline tools
// decode exactly what the game used.
struct RecordedBuffer {
  uint32_t vb_addr;
  uint32_t ib_addr;
  uint64_t fingerprint;
  std::vector<uint8_t> vb;
  std::vector<uint8_t> ib;
};
std::mutex g_record_mutex;
std::atomic<bool> g_recording{false};
uint32_t g_record_frame = 0;   // index of the NEXT recorded frame
uint32_t g_record_stride = 1;  // record every Nth frame
uint32_t g_frames_seen = 0;    // frames completed since StartRecording
std::vector<std::unique_ptr<RecordedDraw>> g_recorded_draws;
std::vector<RecordedFrame> g_recorded_frames;
std::vector<RecordedBuffer> g_recorded_buffers;
std::unordered_set<uint64_t> g_recorded_buffer_keys;
size_t g_recorded_buffer_bytes = 0;
// BeginVertices (inline ring) payloads are written by the CPU AFTER the
// call returns; dump them at frame end, when the writes are complete but
// the ring has not yet wrapped. draw_index links the dump back to its
// RecordedDraw.
struct PendingInlineDump {
  uint32_t addr;
  uint32_t bytes;
  size_t draw_index;
};
std::vector<PendingInlineDump> g_pending_inline_dumps;
// One payload dump per (guest address, frame): 2D dynamic buffers hold many
// draws' vertices; the repeated full-buffer dump would be pure duplication.
std::unordered_map<uint64_t, uint32_t> g_frame_dump_ids;
std::atomic<uint64_t> g_vs_uploads{0};
std::atomic<uint64_t> g_palette_snapshots{0};
// Palettes captured at base+1 (the cloth/morph VS layout with an extra
// parameter row before the palette, see RefinePaletteBase).
std::atomic<uint64_t> g_palette_base_plus1{0};
// Character-lighting capture telemetry: attempts vs validated captures per
// family (see CaptureCharLighting).
std::atomic<uint64_t> g_char_attempts{0};
std::atomic<uint64_t> g_char_valid{0};
std::atomic<uint64_t> g_char_drawn{0};
std::atomic<uint64_t> g_dynobj_drawn{0};
// character.cloth_ropa items captured in the sim-active RIGID mode (world
// from c188/c191 instead of a bone palette, see CaptureSkinnedState).
std::atomic<uint64_t> g_ropa_rigid{0};
// ropa rigid captures REFUSED because the bank's c188/c191 was implausible
// or projected the garment off-clip (stale bank from another mesh's draw);
// the item stays pending for the post-draw fixup.
std::atomic<uint64_t> g_ropa_stale{0};
std::atomic<uint64_t> g_skinned_items{0};
std::atomic<uint64_t> g_skinned_skipped{0};
// Rigid items whose transform had to wait for the post-draw fixup (deferred
// draws), and those still pending at frame end (dropped).
std::atomic<uint64_t> g_rigid_pending{0};
std::atomic<uint64_t> g_rigid_dropped{0};
// Model-space props in the world sort lists, excluded from the identity
// world path (their placed copies come from the kind-2 capture).
std::atomic<uint64_t> g_world_props{0};
std::atomic<uint64_t> g_rej_no_dynstate{0};
std::atomic<uint64_t> g_rej_dyn_range{0};
// Render-side silent-skip diagnostics: a scene item that decodes badly or
// loses its bone binding produces no (or wrong) pixels with no other trace.
std::atomic<uint64_t> g_rr_decode_fail{0};
std::atomic<uint64_t> g_rr_no_bones{0};
// Decodes pushed past the per-frame budget (item/texture appears a few
// frames late instead of hitching the render thread).
std::atomic<uint64_t> g_rr_mesh_deferred{0};
std::atomic<uint64_t> g_rr_tex_deferred{0};
std::atomic<uint64_t> g_rej_chain{0};
std::atomic<uint64_t> g_rej_geom{0};
std::atomic<uint64_t> g_rej_draws{0};
std::atomic<uint64_t> g_rej_bbox{0};

// ---- Perf telemetry (windowed; logged + reset with the 600-frame stats) ----
// Written by the guest render thread (capture/build) and the command
// processor thread (render segments); read by the render thread's stats log.
// Atomics with relaxed ordering: telemetry, not synchronization.
using PerfClock = std::chrono::steady_clock;
struct PerfWindow {
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::atomic<uint64_t> count{0};
  void Add(uint64_t v) {
    ns.fetch_add(v, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = max_ns.load(std::memory_order_relaxed);
    while (v > prev &&
           !max_ns.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
  }
  double AvgMs() const {
    const uint64_t c = count.load(std::memory_order_relaxed);
    return c != 0 ? double(ns.load(std::memory_order_relaxed)) / double(c) * 1e-6
                  : 0.0;
  }
  double MaxMs() const { return double(max_ns.load(std::memory_order_relaxed)) * 1e-6; }
  void Reset() {
    ns.store(0, std::memory_order_relaxed);
    max_ns.store(0, std::memory_order_relaxed);
    count.store(0, std::memory_order_relaxed);
  }
};
// Guest frame counter (guest render thread only; incremented at
// BuildFrameScene entry): paces the world-item cache's revalidation.
uint64_t g_guest_frame = 0;

// Guest render thread: per-frame sums (one Add per guest frame).
PerfWindow g_pw_capture;   // hook-time capture work (sort lists + RenderMesh)
PerfWindow g_pw_build;     // BuildFrameScene
PerfWindow g_pw_guest_dt;  // guest frame interval (guest fps + spike max)
uint64_t g_capture_frame_ns = 0;  // guest render thread only; folded per frame
// Command processor thread: per-RenderScene-call segments.
PerfWindow g_pw_render;       // whole native render
PerfWindow g_pw_items;        // opaque + transparent item loop (incl. decodes)
PerfWindow g_pw_shadow;       // dynamic-shadow atlas pass
PerfWindow g_pw_mesh_decode;  // inline DecodeMesh calls (count = decodes)
PerfWindow g_pw_tex_decode;   // inline texture-ish decodes (cubes, HUD words)
PerfWindow g_pw_commit;       // PrewarmCommit batches (count = non-empty runs)
// Camera-update cadence (the "world judders while panning at 320 fps"
// question): if the guest updates its camera on a fixed sim tick while the
// render loop free-runs, view_proj repeats for several rendered frames and
// then steps, visible as the surroundings skipping to catch up. Counted per
// published scene on the guest render thread.
std::atomic<uint64_t> g_cam_changes{0};
std::atomic<uint64_t> g_cam_repeats{0};
std::atomic<uint64_t> g_cam_max_streak{0};

bool SceneEnabled() { return REXCVAR_GET(skate3_native_render_scene); }

float LoadGuestF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  return std::bit_cast<float>(bits);
}

uint32_t BSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Draws completed since startup (any guest draw function). The RenderMesh
// hook samples it around the original call: an unchanged sequence means the
// mesh's draws were deferred by the dispatcher and the constant bank still
// belongs to some earlier mesh; its transform must come from the post-draw
// fixup, never from the bank at submit-exit (a leftover identity matrix at
// c4 validates as a plausible world and renders the prop at the origin).
std::atomic<uint64_t> g_draw_seq{0};

// Where does this bank keep its bone palette? Pre-pass layout: c4 (bone 0's
// affine rows right after viewproj). Main-pass layout: camera position at
// c4, two parameter rows, palette at c7. A camera-position row is easily
// told from a bone rotation row by its norm (hundreds vs ~1). The ropa-cloth
// skinned VS variant (character.cloth_ropa, player tees) keeps one extra
// parameter row (0,0,0,1) in front of the palette; its zero xyz norm fails
// the bone check outright (unlike the NPC cloth/morph variant's (1,0,0,0),
// which passes at c4 and is corrected by RefinePaletteBase), so the palette
// really starts at c5 (pre-pass) / c8 (main-pass); reading c7 there lands
// mid-palette and scrambles every bone by two rows (the mangled player-shirt
// bug). Returns the base register, or 0 when no location holds plausible
// bone rows.
uint32_t BankPaletteBase(uint8_t* base, uint32_t bank) {
  const auto bone_at = [&](uint32_t reg) -> bool {
    for (int r = 0; r < 3; ++r) {
      float f[4];
      for (int i = 0; i < 4; ++i) {
        f[i] = LoadGuestF32(base, bank + ((reg + r) * 4 + i) * 4);
        if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
      }
      const float n = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
      if (!(n > 0.0025f && n < 400.0f)) return false;
      if (!(f[3] > -20000.f && f[3] < 20000.f)) return false;
    }
    return true;
  };
  // A parameter/sentinel row like (0,0,0,1): xyz norm ~0, never a bone row.
  const auto param_row_at = [&](uint32_t reg) -> bool {
    float f[3];
    for (int i = 0; i < 3; ++i) {
      f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
      if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
    }
    return f[0] * f[0] + f[1] * f[1] + f[2] * f[2] <= 0.0025f;
  };
  // DETERMINISTIC main-pass detection first: that layout keeps the CAMERA
  // POSITION at c4 (palette at c7, or c8 behind a parameter row). Matching
  // c4 against the frame camera pins the layout without any scoring; the
  // norm checks alone accepted banks whose c4 was the camera followed by
  // leftover bone rows (the trucker cap's single main-pass capture), and
  // score-based c7-vs-c8 arbitration flipped to the row-shifted palette
  // whenever camera rotation clipped a sample of the real one (the cap
  // teleporting to axis-permuted coordinates while rotating).
  {
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    if (dx * dx + dy * dy + dz * dz < 25.0f &&
        (g_fog_cam[0] != 0.0f || g_fog_cam[1] != 0.0f || g_fog_cam[2] != 0.0f)) {
      if (param_row_at(7) && bone_at(8)) return 8;
      if (bone_at(7)) return 7;
      return 0;
    }
  }
  if (bone_at(4)) return 4;
  if (param_row_at(4) && bone_at(5)) return 5;
  if (bone_at(7)) return 7;
  if (param_row_at(7) && bone_at(8)) return 8;
  return 0;
}

float GuestHalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1F;
  const uint32_t man = h & 0x3FF;
  if (exp == 0) return std::bit_cast<float>(sign);  // denorms ~0
  if (exp == 31) return std::bit_cast<float>(sign | 0x7F800000u);
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (man << 13));
}

// The cloth/morph skinned VS variant (skating-NPC torsos) keeps ONE extra
// parameter row between the viewproj and the bone palette: observed live,
// c4 = (1,0,0,0) with the palette at c5 (pre-pass) / c8 (main-pass). A
// palette read one register early hands every bone [neighbor row, row0,
// row1]; each row is still a perfectly plausible bone row, so the norm
// checks in BankPaletteBase cannot catch it, and the mesh skins to
// component-rotated coordinates ~300 m off in the sky. That was the
// invisible-NPC-torso bug: the torso was captured, non-pending, palette and
// texture resolved, and rendered far outside the view.
//
// Discriminate by skinning a few sample vertices with EVERY candidate base
// (pre-pass c4/c5, main-pass c7/c8, plus the caller's guess) and projecting
// them with the pass's own viewproj (bank c0..c3, column-vector rows): the
// game drew this mesh with these constants, so the correct base puts the
// samples inside the clip volume (validated offline across every skinned
// draw of an F10 capture: correct base 1.00, wrong base <= ~0.3), AND keeps
// the skinned samples' spatial spread near the mesh's bind-pose size. The
// spread test is what rejects a FOREIGN palette that lands coincidentally
// in view: the player's trucker cap draws exactly ONCE per frame (main
// pass, palette at c7) while leftover rows at c4 pass the norm checks,
// junk-skinning it into a ~3 m smear near the world origin (bind size
// ~0.4 m) that was sometimes on screen, i.e. the sometimes-visible
// disappearing-hat bug. Returns the winning base, or 0 when no candidate
// both projects in-clip and keeps a sane spread; the caller then refuses
// the capture and the post-draw fixup retries on a later draw.
uint32_t RefinePaletteBase(uint8_t* base, uint32_t bank, uint32_t palette_base,
                           const DrawItem& item) {
  if (item.bw_offset == 0 || item.bi_offset == 0 || item.stride == 0) {
    return palette_base;
  }
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2) {
    return palette_base;
  }
  float vp[16];
  for (int i = 0; i < 16; ++i) {
    vp[i] = LoadGuestF32(base, bank + i * 4);
    if (!(vp[i] > -1e9f && vp[i] < 1e9f)) return palette_base;
  }
  constexpr uint32_t kSamples = 6;
  // score: fraction (0..16) of samples that skin in front of and inside the
  // bank's own clip volume; *out_spread = the skinned samples' bbox diagonal
  // (world units) for the bind-size sanity test below.
  const auto score = [&](uint32_t pb, float* out_spread) -> int {
    int ok = 0;
    int n = 0;
    float qmin[3] = {1e9f, 1e9f, 1e9f};
    float qmax[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t s = 0; s < kSamples; ++s) {
      const uint32_t v = item.vb_addr + (s * (count - 1) / (kSamples - 1)) * item.stride;
      float p[3];
      const uint32_t pa = v + item.pos_offset;
      switch (item.pos_fmt) {
        case 57:
          for (int a = 0; a < 3; ++a) p[a] = LoadGuestF32(base, pa + a * 4);
          break;
        case 32:
          for (int a = 0; a < 3; ++a) {
            p[a] = GuestHalfToFloat(uint16_t(REX_LOAD_U16(pa + a * 2)));
          }
          break;
        case 26: {
          constexpr float kScale = 2.0f / 32767.0f;
          for (int a = 0; a < 3; ++a) {
            p[a] = int16_t(REX_LOAD_U16(pa + a * 2)) * kScale + (a == 1 ? 0.8f : 0.0f);
          }
          break;
        }
        default:
          return -1;
      }
      // u8x4 attributes are big-endian per 32-bit word: component k is byte
      // (24 - 8k) of the host-order load.
      const uint32_t bw = REX_LOAD_U32(v + item.bw_offset);
      const uint32_t bi = REX_LOAD_U32(v + item.bi_offset);
      uint32_t total = 0;
      float q[3] = {0.0f, 0.0f, 0.0f};
      for (int k = 0; k < 4; ++k) {
        const uint32_t w = (bw >> (24 - 8 * k)) & 0xFF;
        if (w == 0) continue;
        const uint32_t bone = (bi >> (24 - 8 * k)) & 0xFF;
        const uint32_t r0 = pb + 3 * bone;
        if (r0 + 3 > 256) continue;
        total += w;
        for (int a = 0; a < 3; ++a) {
          float row[4];
          for (int i = 0; i < 4; ++i) {
            row[i] = LoadGuestF32(base, bank + ((r0 + a) * 4 + i) * 4);
          }
          q[a] += float(w) * (row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3]);
        }
      }
      if (total == 0) continue;
      for (int a = 0; a < 3; ++a) q[a] /= float(total);
      float clip[4];
      for (int r = 0; r < 4; ++r) {
        clip[r] = vp[r * 4] * q[0] + vp[r * 4 + 1] * q[1] + vp[r * 4 + 2] * q[2] +
                  vp[r * 4 + 3];
      }
      const float aw = std::abs(clip[3]) < 1.0f ? 1.0f : std::abs(clip[3]);
      ++n;
      for (int a = 0; a < 3; ++a) {
        qmin[a] = std::min(qmin[a], q[a]);
        qmax[a] = std::max(qmax[a], q[a]);
      }
      // In FRONT of the projection (w > 0) and inside a generous guard band.
      // Without the w check a foreign palette that skins the mesh BEHIND the
      // camera can still land |x|,|y| within the band and score well.
      if (clip[3] > 0.0f && std::abs(clip[0]) <= 1.5f * aw &&
          std::abs(clip[1]) <= 1.5f * aw) {
        ++ok;
      }
    }
    if (n == 0) {
      return -1;
    }
    const float dx = qmax[0] - qmin[0];
    const float dy = qmax[1] - qmin[1];
    const float dz = qmax[2] - qmin[2];
    *out_spread = std::sqrt(dx * dx + dy * dy + dz * dz);
    return (ok * 16) / n;
  };
  // Bind-pose size of the SAMPLED span (approximates the mesh diagonal):
  // legit skinning keeps the world spread near it; a foreign palette
  // composes inconsistent transforms and smears the samples several times
  // wider. Generous bound: articulation can stretch a garment's sampled
  // span, junk palettes overshoot by ~10x.
  float bind_diag = 0.0f;
  for (int a = 0; a < 3; ++a) {
    const float d = item.bbox_max[a] - item.bbox_min[a];
    bind_diag += d * d;
  }
  bind_diag = std::sqrt(bind_diag);
  const float max_spread = std::max(3.0f * bind_diag, bind_diag + 1.0f);
  // Acceptance gate: skins into the bank's own view AND at a sane size.
  // IMPORTANT selection constraint (offline-validated on the cap capture):
  // a palette shifted by whole rows is a RIGID transform of the mesh, same
  // spread, often still on screen, so "best spread/score wins" mis-picks
  // permuted bases. Selection therefore stays conservative: keep the
  // caller's guess (old base-vs-base+1 arbitration) whenever it passes the
  // gate, and only on gate FAILURE fall through to the other layout homes
  // (pre-pass c4/c5, main-pass c7/c8) in canonical order. The trucker cap
  // is the motivating case: it draws ONCE per frame (main-pass layout,
  // palette at c7) while leftovers at c4 pass the row-plausibility checks;
  // the old code never looked past c4/c5 and skinned it into a ~3 m smear
  // near the origin (the disappearing-hat bug).
  const auto gate = [&](uint32_t pb, int* ok_out) -> bool {
    float spread = 0.0f;
    const int ok = score(pb, &spread);
    if (ok_out) {
      *ok_out = ok;
    }
    return ok >= 8 && spread <= max_spread;
  };
  int s_std = -1;
  int s_plus = -1;
  const bool std_pass = gate(palette_base, &s_std);
  if (s_std < 0) {
    // Unsupported position format / no weighted samples: nothing to judge;
    // keep the caller's guess rather than refusing every capture.
    return palette_base;
  }
  // The guess wins whenever it passes; +1 (the cloth/morph parameter-row
  // variant) is consulted only on FAILURE. A row-shifted palette is a rigid
  // axis-permutation of the mesh, often still on screen with a sane spread
  // - so "switch when +1 scores strictly better" flip-flopped whenever
  // camera rotation clipped one sample of the real base (cap teleporting
  // while rotating). The genuine +1 layouts skin the guess-base hundreds of
  // meters off-view, which the gate rejects decisively.
  if (std_pass) {
    return palette_base;
  }
  if (gate(palette_base + 1, &s_plus)) {
    g_palette_base_plus1.fetch_add(1, std::memory_order_relaxed);
    return palette_base + 1;
  }
  for (const uint32_t pb : {4u, 7u, 5u, 8u}) {
    if (pb == palette_base || pb == palette_base + 1) {
      continue;
    }
    if (gate(pb, nullptr)) {
      return pb;
    }
  }
  // No candidate skins this mesh into the bank's own view at a sane size:
  // the bank belongs to another mesh. The caller refuses the capture (item
  // stays pending) and the post-draw fixup re-captures from a real draw.
  return 0;
}

// Rigid transform validation: the game uses (at least) two VS constant
// layouts, verified from recorded draw streams: the pre-pass layout has a
// row-vector 4x4 world at c4..c7 (rotation rows end in 0, translation in
// c7); the main-pass layout has the camera position at c4 and the world
// 4x4 at c8..c11. Older meshes use a column-vector affine 4x3 at c4..c6.
// Sanity-check rows so a camera-position or parameter block is never
// mistaken for a matrix.
bool TryRow4x4(uint8_t* base, uint32_t bank, uint32_t reg, float* out) {
  float f[16];
  for (int i = 0; i < 16; ++i) {
    f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
    if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
  }
  if (f[3] != 0.0f || f[7] != 0.0f || f[11] != 0.0f || f[15] != 1.0f) return false;
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) return false;
  }
  if (!(f[12] > -20000.f && f[12] < 20000.f && f[13] > -20000.f && f[13] < 20000.f &&
        f[14] > -20000.f && f[14] < 20000.f)) {
    return false;
  }
  std::memcpy(out, f, 16 * sizeof(float));
  return true;
}

bool TryColAffine(uint8_t* base, uint32_t bank, uint32_t reg, float* out) {
  float f[12];
  for (int i = 0; i < 12; ++i) {
    f[i] = LoadGuestF32(base, bank + (reg * 4 + i) * 4);
    if (!(f[i] > -1e7f && f[i] < 1e7f)) return false;
  }
  for (int r = 0; r < 3; ++r) {
    const float n = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] +
                    f[r * 4 + 2] * f[r * 4 + 2];
    if (!(n > 0.0025f && n < 400.0f)) return false;
    if (!(f[r * 4 + 3] > -20000.f && f[r * 4 + 3] < 20000.f)) return false;
  }
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      out[i * 4 + j] = f[j * 4 + i];
    }
    out[i * 4 + 3] = 0.0f;
  }
  out[12] = f[3];
  out[13] = f[7];
  out[14] = f[11];
  out[15] = 1.0f;
  return true;
}

// Fill a rigid item's world matrix from a constant bank, whichever layout
// the staging draw used.
bool BankRigidWorld(uint8_t* base, uint32_t bank, float* out) {
  return TryRow4x4(base, bank, 4, out) || TryRow4x4(base, bank, 8, out) ||
         TryColAffine(base, bank, 4, out);
}

bool GuestReadableApprox(uint8_t* base, uint32_t addr) {
  // The hook layer only walks pointers the game is actively rendering from;
  // they are mapped. Reject null/small.
  (void)base;
  return addr >= 0x10000;
}

// Lock-free guarded bulk copy for render-thread reads of guest payloads.
// GuestRangeReadable's VirtualQuery loop takes the process VAD lock, which
// the guest streaming threads hammer exactly while panning streams the world
// in; every render-thread decode then queues behind them (multi-ms stalls;
// same lock as the 3 fps PERF TRAP). An SEH-guarded memcpy costs nothing in
// the good case and fails cleanly if streaming decommitted the range between
// capture and decode. Own function, no C++ objects: SEH cannot share a frame
// with unwinding.
#if defined(_WIN32)
// TWO TRAPS PROVEN FROM CRASH DUMPS OF THE REGISTRY-PREWARM PROBE; both
// silently delete the guard and let the AV kill the process:
// 1. clang marks std::memcpy nounwind, concludes the __except is
//    unreachable, and compiles the whole function to `jmp memcpy` (seen in
//    the shipped binary). Calling through a VOLATILE function pointer makes
//    the callee opaque so the SEH scope must be kept.
// 2. When this function is INLINED into a caller using the C++ EH
//    personality (__CxxFrameHandler3), the __except scope is dropped in the
//    merge, hence the noinline.
// This means plain `__try { std::memcpy(...) } __except` NEVER protected
// anything in optimized builds; any future guarded read must go through
// this function.
typedef void* (*GuestMemcpyFn)(void*, const void*, size_t);
volatile GuestMemcpyFn g_guest_memcpy_fn = std::memcpy;
__declspec(noinline) bool GuestTryCopy(void* dst, const void* src, size_t size) {
  __try {
    g_guest_memcpy_fn(dst, src, size);
    return true;
  } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
               GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {  // NOLINT
    return false;
  }
}
#else
bool GuestTryCopy(void* dst, const void* src, size_t size) {
  std::memcpy(dst, src, size);
  return true;
}
#endif

// SEH-guarded single-value guest loads for registry-time walks (the
// loading-screen prewarm): unlike the capture-path walks, whose pointers
// the game is actively rendering from, registry probes dereference
// candidate words that may not be pointers at all, and load-time payloads
// that may not be committed yet. GuestReadableApprox is only a null/small
// filter; these actually survive the fault.
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint32_t raw;
  if (!GuestTryCopy(&raw, REX_RAW_ADDR(addr), 4)) {
    return false;
  }
  *out = __builtin_bswap32(raw);
  return true;
}

bool GuestTryLoadU64(uint8_t* base, uint32_t addr, uint64_t* out) {
  if (addr < 0x10000) {
    return false;
  }
  uint64_t raw;
  if (!GuestTryCopy(&raw, REX_RAW_ADDR(addr), 8)) {
    return false;
  }
  *out = __builtin_bswap64(raw);
  return true;
}

// Guarded bounded C-string read (`out` gets up to cap-1 chars + NUL, tail
// zeroed like the old byte-at-a-time loops). Fast path is one bulk guarded
// copy; a short string right before an unmapped page falls back to
// byte-wise guarded reads so it still resolves.
void GuestTryReadString(uint8_t* base, uint32_t addr, char* out, uint32_t cap) {
  std::memset(out, 0, cap);
  if (addr < 0x10000) {
    return;
  }
  if (GuestTryCopy(out, REX_RAW_ADDR(addr), cap - 1)) {
    const size_t n = strnlen(out, cap - 1);
    std::memset(out + n, 0, cap - n);
    return;
  }
  for (uint32_t k = 0; k + 1 < cap; ++k) {
    uint8_t c;
    if (!GuestTryCopy(&c, REX_RAW_ADDR(addr + k), 1) || c == 0) {
      break;
    }
    out[k] = char(c);
  }
}

// Committed-page check for bulk reads (transient ring memory can be
// partially uncommitted, and resources may be released between the game
// thread capturing an address and the render thread reading it; a blind
// memcpy would fault).
bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t size) {
#if defined(_WIN32)
  uint8_t* p = base + addr;
  uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
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

// Payload fingerprint (FNV-1a over bytes sampled across the whole VB/IB) so
// the renderer re-decodes when streaming replaces or fills in the data at
// this address, including middle-of-buffer fills. Guarded reads: capture-
// path payloads are always resident (the game is drawing from them), but
// the registration prewarm fingerprints meshes whose payload pages may not
// be committed yet; the failure defers the mesh instead of faulting the
// thread. Returns false when the payload is unreadable.
bool ComputeItemFingerprint(uint8_t* base, DrawItem& item) {
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
      uint64_t vq = 0, iq = 0;
      const uint32_t vb_off = uint32_t(uint64_t(item.vb_bytes - 8) * k / 15u) & ~7u;
      const uint32_t ib_off = uint32_t(uint64_t(item.ib_count * 2 - 8) * k / 15u) & ~7u;
      if (!GuestTryLoadU64(base, item.vb_addr + vb_off, &vq) ||
          !GuestTryLoadU64(base, item.ib_addr + ib_off, &iq)) {
        return false;
      }
      mix(vq);
      mix(iq);
    }
  }
  item.fingerprint = h;
  return true;
}

// Walk one MeshContext into a DrawItem (geometry, material and draw list;
// world left as identity). Returns false if any pointer in the chain is
// implausible. For dynamic entities this MUST run at RenderMesh hook time;
// the whole chain lives in transient per-frame arenas.
// Parse a guest mesh struct (vertex descriptor, buffers, bbox, material
// channels, payload fingerprint) into the item. The draw list and world
// transform are the caller's responsibility (world starts as identity).
bool BuildItemFromMesh(uint8_t* base, uint32_t mesh, DrawItem& item) {
  const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
  const uint32_t ib = REX_LOAD_U32(mesh + kMeshIndexBuffer);
  const uint32_t vb = REX_LOAD_U32(mesh + kMeshVertexBuffer);
  if (!GuestReadableApprox(base, vdesc) || !GuestReadableApprox(base, ib) ||
      !GuestReadableApprox(base, vb)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Vertex descriptor: find the stream-0 position element and the first
  // stream-0 texcoord (D3DDECLUSAGE 5) for the diffuse map. Read via ONE
  // guarded bulk copy into scratch: descriptors are runtime renderengine
  // objects, and the registration prewarm can reach a mesh before its
  // descriptor is initialized (a raw read there faults the thread; the
  // capture path also gets marginally faster than the per-field volatile
  // loads).
  uint32_t desc_head[3];
  if (!GuestTryCopy(desc_head, REX_RAW_ADDR(vdesc), sizeof(desc_head))) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t num_elements = BSwap32(desc_head[2]) >> 16;  // u16 at +8
  if (num_elements == 0 || num_elements > 32) return false;
  // Element table at +0x10 (16 bytes per element) followed by the stride
  // byte at +(num_elements + 1) * 16.
  uint8_t desc_tab[32 * 16 + 1];
  const uint32_t desc_tab_bytes = num_elements * 16 + 1;
  if (!GuestTryCopy(desc_tab, REX_RAW_ADDR(vdesc + 0x10), desc_tab_bytes)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto elem_u16 = [&](uint32_t i, uint32_t off) -> uint32_t {
    return (uint32_t(desc_tab[i * 16 + off]) << 8) | desc_tab[i * 16 + off + 1];
  };
  const auto elem_u32 = [&](uint32_t i, uint32_t off) -> uint32_t {
    uint32_t v;
    std::memcpy(&v, desc_tab + i * 16 + off, 4);
    return BSwap32(v);
  };
  bool have_pos = false;
  bool have_bw = false;
  bool have_bi = false;
  item.uv_offset = 0;
  item.uv_fmt = 0;
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.normal_offset = 0;
  item.normal_fmt = 0;
  item.tangent_offset = 0;
  item.binormal_offset = 0;
  item.tb_fmt = 0;
  bool have_tan = false;
  bool have_bin = false;
  item.skinned = false;
  for (uint32_t i = 0; i < num_elements; ++i) {
    const uint32_t stream = elem_u16(i, 0);
    const uint32_t usage = desc_tab[i * 16 + 9];
    if (stream != 0) continue;
    if (usage == 0 && !have_pos) {
      item.pos_offset = uint16_t(elem_u16(i, 2));
      item.pos_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
      have_pos = true;
    } else if (usage == 3 && item.normal_fmt == 0) {
      const uint8_t fmt = uint8_t(elem_u32(i, 4) & 0x3F);
      if (fmt == 16) {  // k_10_11_11 packed normal
        item.normal_offset = uint16_t(elem_u16(i, 2));
        item.normal_fmt = fmt;
      }
    } else if (usage == 5 && item.uv_fmt == 0) {
      item.uv_offset = uint16_t(elem_u16(i, 2));
      item.uv_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
    } else if (usage == 5 && item.uv2_fmt == 0) {
      item.uv2_offset = uint16_t(elem_u16(i, 2));
      item.uv2_fmt = uint8_t(elem_u32(i, 4) & 0x3F);
    } else if (usage == 1 && !have_bw) {  // blend weights u8x4
      item.bw_offset = uint16_t(elem_u16(i, 2));
      have_bw = (elem_u32(i, 4) & 0x3F) == 6;
    } else if (usage == 2 && !have_bi) {  // blend indices u8x4
      item.bi_offset = uint16_t(elem_u16(i, 2));
      have_bi = (elem_u32(i, 4) & 0x3F) == 6;
    } else if (usage == 6 && !have_tan &&
               (elem_u32(i, 4) & 0x3F) == 16) {  // tangent k_10_11_11
      item.tangent_offset = uint16_t(elem_u16(i, 2));
      have_tan = true;
    } else if (usage == 7 && !have_bin &&
               (elem_u32(i, 4) & 0x3F) == 16) {  // binormal k_10_11_11
      item.binormal_offset = uint16_t(elem_u16(i, 2));
      have_bin = true;
    }
  }
  if (have_tan && have_bin) {
    item.tb_fmt = 16;
  }
  if (!have_pos) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.skinned = have_bw && have_bi;
  item.stride = desc_tab[num_elements * 16];  // byte at vdesc+(num+1)*16
  if (item.stride == 0) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Mesh BBox = two Vector4s at +0x00/+0x10. Reject NaN/absurd bounds.
  for (int axis = 0; axis < 3; ++axis) {
    const float lo = LoadGuestF32(base, mesh + axis * 4);
    const float hi = LoadGuestF32(base, mesh + 0x10 + axis * 4);
    if (!(hi - lo < 50000.0f)) {
      g_rej_bbox.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    item.bbox_min[axis] = lo;
    item.bbox_max[axis] = hi;
  }

  item.mesh = mesh;
  item.pending = false;
  item.cloth_quads = false;
  item.vb_obj = vb;
  item.ib_obj = ib;
  // Guarded: the registration prewarm can reach buffer objects before they
  // finish initializing (the capture path only ever sees live ones).
  uint32_t vb_words[3] = {}, ib_words[3] = {};
  if (!GuestTryCopy(vb_words, REX_RAW_ADDR(vb + kBufferPhysAddr), sizeof(vb_words)) ||
      !GuestTryCopy(ib_words, REX_RAW_ADDR(ib + kBufferPhysAddr), sizeof(ib_words))) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.vb_addr = BSwap32(vb_words[0]) & 0xFFFFFFFC;
  item.vb_bytes = BSwap32(vb_words[2]);
  item.ib_addr = BSwap32(ib_words[0]) & 0xFFFFFFFC;
  item.ib_count = BSwap32(ib_words[2]);
  if (item.vb_addr == 0 || item.ib_addr == 0 || item.vb_bytes == 0 ||
      item.ib_count == 0 || item.vb_bytes % item.stride != 0) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Material channels: resolve the "diffuse" and "lightmap" texture guids to
  // registered texture objects.
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  item.macro_tex = 0;
  item.macro_scale = 1.0f;
  item.macro_opacity = 1.0f;
  item.decal_art = 0;
  item.hair = false;
  item.char_family = 0;
  item.hair_alpha_tex = 0;
  item.ropa = false;
  item.decal = false;
  item.decal_tileable = false;
  item.transparent = false;
  item.water = false;
  item.water_normal = 0;
  item.water_env = 0;
  item.unlit = false;
  item.env_family = 0;
  item.dynobj = 0;
  item.spec_tex = 0;
  item.tint[0] = item.tint[1] = item.tint[2] = item.tint[3] = 0.0f;
  // Material header + channel array read via guarded bulk copies: the
  // registration prewarm walks materials while their arena is still
  // loading, where raw reads fault the thread. (The capture path gets the
  // same copies; one memcpy per material beats ~8 volatile loads per
  // channel anyway.)
  const uint32_t material = REX_LOAD_U32(mesh + kMeshMaterial);
  uint32_t mat_head[3] = {};
  if (GuestReadableApprox(base, material) &&
      GuestTryCopy(mat_head, REX_RAW_ADDR(material), sizeof(mat_head))) {
    const uint32_t num_channels = BSwap32(mat_head[0]);
    const uint32_t channels = BSwap32(mat_head[2]);
    uint8_t chan_buf[32 * 0x20];
    if (num_channels != 0 && num_channels <= 32 && GuestReadableApprox(base, channels) &&
        GuestTryCopy(chan_buf, REX_RAW_ADDR(channels), num_channels * 0x20)) {
      const auto chan_u32 = [&](uint32_t idx, uint32_t off) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, chan_buf + idx * 0x20 + off, 4);
        return BSwap32(v);
      };
      for (uint32_t i = 0; i < num_channels; ++i) {
        const uint32_t name = chan_u32(i, 0);
        char text[20];
        GuestTryReadString(base, name, text, sizeof(text));
        if (text[0] == '\0') continue;
        uint32_t* slot = nullptr;
        if (std::memcmp(text, "diffuse", 8) == 0) {
          slot = &item.diffuse_tex;
        } else if (std::memcmp(text, "lightmap", 9) == 0) {
          slot = &item.lightmap_tex;
        } else if (std::memcmp(text, "macrooverlay", 13) == 0) {
          slot = &item.macro_tex;
        } else if (std::memcmp(text, "decal", 6) == 0) {
          slot = &item.decal_art;
        } else if (std::memcmp(text, "normal", 7) == 0) {
          // Exact match only ("normal2" is the second flowing-water tap).
          // Consumed by the water branch; other families ignore it.
          slot = &item.water_normal;
        } else if (std::memcmp(text, "alpha", 6) == 0) {
          // Hair strand coverage (cac_hair/defaulthair tf5, sampled at the
          // second texcoord), the hair alpha-blend term.
          slot = &item.hair_alpha_tex;
        } else if (std::memcmp(text, "environment", 12) == 0) {
          // Environment CUBE map, the water and environment.reflective*
          // reflection term.
          slot = &item.water_env;
        } else if (std::memcmp(text, "specular", 9) == 0 ||
                   std::memcmp(text, "noise", 6) == 0) {
          // Spec/eccentricity/reflection-mask map (environment families) /
          // the animated.tree "noise" tint map, bound at t4 on families
          // that carry no decal art (see DrawItem::spec_tex).
          slot = &item.spec_tex;
        } else if (std::memcmp(text, "macroOverlayUVScale", 20) == 0 ||
                   std::memcmp(text, "macroOverlayOpacity", 20) == 0) {
          // Shader-constant channel: the float lives in the first guid word.
          const float f = std::bit_cast<float>(chan_u32(i, 0x10));
          if (f > 0.0f && f < 1e6f) {
            (text[12] == 'U' ? item.macro_scale : item.macro_opacity) = f;
          }
          continue;
        } else if (std::memcmp(text, "Attribul", 8) == 0) {
          // AttribulatorMaterialName: chan+0x18 is the material name string.
          // "character.hair" marks the grayscale hair that needs the
          // per-character tint from the pixel constant bank; "sky.*" draws
          // fullbright; "character.cloth_ropa" is the Ropa cloth-sim VS
          // variant (flag-row-switched skinned/rigid, see CaptureSkinnedState).
          const uint32_t s = chan_u32(i, 0x18);
          if (GuestReadableApprox(base, s)) {
            // 40 bytes: "character.livingworld_vehicles_glass" (36 chars) is
            // the longest name that must be distinguishable; the previous
            // 28-byte buffer truncated both vehicle names into the plain
            // "livingworld" pedestrian prefix.
            char mat_name[40];
            GuestTryReadString(base, s, mat_name, sizeof(mat_name));
            item.hair = std::memcmp(mat_name, "character.hair", 15) == 0;
            item.unlit = std::memcmp(mat_name, "sky.", 4) == 0;
            item.ropa = std::memcmp(mat_name, "character.cloth_ropa", 21) == 0;
            // Character shading family (see DrawItem::char_family). Order
            // matters: "default_hair" before the "default" prefix, exact
            // "hair" after (memcmp includes the NUL for exact names).
            if (std::memcmp(mat_name, "character.", 10) == 0) {
              const char* sub = mat_name + 10;
              if (std::memcmp(sub, "default_hair", 13) == 0) {
                item.char_family = 5;
              } else if (std::memcmp(sub, "default", 7) == 0) {
                item.char_family = 1;
              } else if (std::memcmp(sub, "hair", 5) == 0) {
                item.char_family = 4;
              } else if (std::memcmp(sub, "livingworld_vehicles_glass", 27) == 0) {
                item.char_family = 7;  // reflection-only blended windows
              } else if (std::memcmp(sub, "livingworld_vehicles", 21) == 0) {
                item.char_family = 6;  // vehicle.fx paint/spec/cube body
              } else if (std::memcmp(sub, "livingworld", 11) == 0) {
                item.char_family = 3;
              } else {
                item.char_family = 2;  // skin/face/cloth/leather/shift/ropa
              }
            }
            // environment.decal / environment.decal_tileable: graffiti and
            // painted-branding overlay meshes (see DrawItem::decal).
            // Tileable art WRAPS (rock/cliff faces tile the art across the
            // whole surface, uv spans many periods; clamping stretches the
            // border texels into giant streaks); single-placement decal art
            // CLAMPS (wrap tiled the graffiti across the plaza).
            item.decal = std::memcmp(mat_name, "environment.decal", 17) == 0;
            item.decal_tileable =
                std::memcmp(mat_name, "environment.decal_tileable", 26) == 0;
            // environment.transparent: alpha-blended world geometry (mist
            // sheets, glass, fences); see DrawItem::transparent.
            item.transparent =
                std::memcmp(mat_name, "environment.transparent", 23) == 0;
            // water.* (canal) and ocean.* (the sea): transparent sub-pass
            // with the dedicated water shading branch (see DrawItem::water).
            // ocean.default has NO diffuse channel at all (ocean.fx computes
            // color purely from the environment cube x lightmap x fresnel);
            // without this branch it rendered as an 8 km white plane (white
            // fallback diffuse x near-white ocean lightmap x2).
            item.water = std::memcmp(mat_name, "water.", 6) == 0 ||
                         std::memcmp(mat_name, "ocean.", 6) == 0;
            // Exact world-shading family (see DrawItem::env_family). The
            // attrib <-> pixel-shader-family mapping is 1:1; the shading
            // models were verified per-pixel against the game's own shaders.
            const auto is = [&](const char* s, size_t n) {
              return std::memcmp(mat_name, s, n) == 0;
            };
            if (is("environment.default", 20)) {
              item.env_family = 1;
            } else if (is("environmentsimple.default", 26)) {
              item.env_family = 2;
            } else if (is("environment.decal_tileable", 26)) {
              item.env_family = 4;  // includes decal_tileable_simple
            } else if (is("environment.decal", 18)) {
              item.env_family = 3;
            } else if (is("environment.reflective_simple", 30)) {
              item.env_family = 6;
            } else if (is("environment.reflective", 23)) {
              item.env_family = 5;
            } else if (is("environmentsimple.alphatest", 28)) {
              item.env_family = 7;
            } else if (is("environmentsimple.diffuse", 26)) {
              item.env_family = 8;
            } else if (is("tree.default", 13)) {
              item.env_family = 9;
            } else if (is("animated.tree", 14)) {
              item.env_family = 10;
            } else if (is("proxyworld.", 11)) {
              item.env_family = 11;
            } else if (is("incandescent.default", 21)) {
              item.env_family = 12;
            }
            // dynamicobject.fx props (dispensers, dumpsters, benches, cans):
            // rigid movable objects with their own dual-shadow lit PS
            // (model verified exact against the game's own pixel shader).
            // Separate from the world env families; the lighting rows come
            // from the draw's PS bank, not the frame-global world rows.
            if (is("dynamicobject.alphatest", 24)) {
              item.dynobj = 2;
            } else if (is("dynamicobject", 13)) {
              item.dynobj = 1;
            }
          }
          continue;
        }
        if (slot != nullptr && *slot == 0) {
          // Prefer the channel's live stream record (chan+0x1C -> word 0 =
          // the renderengine::Texture actually bound): runtime-composed
          // customization textures (CAS face/skin, shoes, deck, wheels)
          // are never registered under an asset GUID. Validate via the
          // fetch-constant type bits before trusting the pointer.
          // Guarded: the prewarm walks materials whose stream records /
          // texture objects may not be loaded yet.
          const uint32_t stream = chan_u32(i, 0x1C);
          uint32_t tex = 0, tex_w0 = 0;
          if (GuestTryLoadU32(base, stream, &tex) &&
              GuestTryLoadU32(base, tex + 7 * 4, &tex_w0) && (tex_w0 & 3u) == 2u) {
            *slot = tex;
          }
          if (*slot == 0) {
            const uint64_t guid =
                (uint64_t(chan_u32(i, 0x10)) << 32) | chan_u32(i, 0x14);
            std::lock_guard<std::mutex> lock(g_texture_map_mutex);
            auto it = g_texture_map.find(guid & kGuidMask);
            if (it != g_texture_map.end()) {
              *slot = it->second;
            }
          }
        }
        // No early-out: channel order varies per material and the float
        // channels ride AFTER their texture (macroOverlayUVScale follows
        // macrooverlay on the plaza asphalt, breaking once the textures
        // resolved left the macro tiling at 1.0 instead of 0.3).
      }
    }
  }

  // Payload fingerprint: see ComputeItemFingerprint (the item cache
  // refreshes it on its own cadence).
  if (!ComputeItemFingerprint(base, item)) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Transform (identity for world geometry, instance matrix for props;
  // characters get their bone array's first matrix, which roughly places
  // them until skinning is implemented).
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  return true;
}

// Streamed-artwork override (event ads): the Massive ad system rebinds
// texture fetch slots at draw time to show the current event-ad art in
// place of the asset's default poster. Two observed shapes: a 16x16
// min-mip stub channel whose real art exists ONLY at draw time (the
// ace-of-spades "Big Event" grid), and a full-size default poster (the
// letter-writing frames) whose diffuse is wholesale REPLACED with the
// current ad (the MONDO "THE NEW VIDEO" portrait). Adopt the fetch
// slot-4 words recorded at this mesh's LAST indexed draw this frame,
// but only if that draw's slot 3 is this item's own lightmap
// (mirror-masked base compare), which proves the recorded fetch state
// belongs to the mesh's MAIN-pass draw (the z-prepass leaves another
// material's bindings). Slot targets are family-specific (Skate 2
// shader source): environmentsimple.* sample the diffuse at s4;
// environment.decal samples its decal overlay art at s4 (diffuse rides
// s6); environment.default has DETAIL at s4, so its diffuse adoption
// stays stub-gated; never adopt a detail texture over real poster art.
// Per-frame state (g_frame_draw_fetch), so this runs on every item build;
// it is NOT part of the cacheable item core.
void AdoptDrawFetchOverrides(uint8_t* base, DrawItem& item) {
  if (item.env_family != 0 && item.diffuse_tex != 0 && item.lightmap_tex != 0) {
    // Physical base of a fetch word-1, collapsing the 0xA0000000-style
    // cached/uncached mirrors (the channel object records the mirror
    // address, the draw-time fetch constant the plain one).
    const auto phys_base = [](uint32_t w1) { return w1 & 0x1FFFF000u; };
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    const auto fit =
        g_frame_draw_fetch.find((uint64_t(item.ib_obj) << 32) | item.vb_obj);
    if (fit != g_frame_draw_fetch.end()) {
      // Texture-object reads only on a fetch-map hit, and guarded: the map
      // is empty on prewarm/loading frames, and the GUID registry can
      // resolve to a freed previous-map object there (raw reads faulted).
      uint32_t lm_w1 = 0;
      const uint32_t* slot3 = fit->second.data();
      const uint32_t* slot4 = fit->second.data() + 6;
      const uint32_t art_w = (slot4[2] & 0x1FFFu) + 1;
      const uint32_t art_h = ((slot4[2] >> 13) & 0x1FFFu) + 1;
      const bool main_pass = (slot3[0] & 3u) == 2u && (slot4[0] & 3u) == 2u &&
                             GuestTryLoadU32(base, item.lightmap_tex + 8 * 4, &lm_w1) &&
                             phys_base(slot3[1]) == phys_base(lm_w1);
      // Dimension floor: never adopt a placeholder-sized s4 (an idle ad
      // rotation slot) over whatever the channel resolves to.
      if (main_pass && (art_w >= 32 || art_h >= 32)) {
        if (item.env_family == 3) {
          uint32_t da_w1 = 0;
          if (item.decal_art != 0 &&
              GuestTryLoadU32(base, item.decal_art + 8 * 4, &da_w1) &&
              phys_base(slot4[1]) != phys_base(da_w1)) {
            std::memcpy(item.decal_fetch, slot4, 6 * sizeof(uint32_t));
          }
        } else {
          uint32_t diff_w2 = 0, diff_w1 = 0;
          if (!GuestTryLoadU32(base, item.diffuse_tex + 9 * 4, &diff_w2) ||
              !GuestTryLoadU32(base, item.diffuse_tex + 8 * 4, &diff_w1)) {
            diff_w2 = 0;
            diff_w1 = slot4[1];  // unreadable diffuse object: adopt nothing
          }
          const uint32_t diff_w = (diff_w2 & 0x1FFFu) + 1;
          const uint32_t diff_h = ((diff_w2 >> 13) & 0x1FFFu) + 1;
          const bool diff_stub = diff_w <= 32 && diff_h <= 32;
          const bool s4_is_diffuse = item.env_family == 2 ||
                                     item.env_family == 7 ||
                                     item.env_family == 8;
          if ((s4_is_diffuse || diff_stub) &&
              phys_base(slot4[1]) != phys_base(diff_w1)) {
            std::memcpy(item.diffuse_fetch, slot4, 6 * sizeof(uint32_t));
          }
        }
      }
    }
  }

}

// ---- World-item cache ------------------------------------------------------
// BuildItemFromMesh walks the descriptor, the material channels (string
// reads) and the payload fingerprint, and used to run for EVERY visible
// item EVERY frame on the guest render thread. The walk's result is stable
// while a mesh stays loaded, so it is cached per mesh:
//   - every frame: 4-pointer structural validation + buffer address check
//     (streaming reuses arena addresses; re-inits rebuild)
//   - every 4th frame (or every frame for skinned/ropa payloads, whose
//     buffers the CPU sim rewrites): fingerprint refresh
//   - every 32nd frame: full rebuild (material channels re-resolve: late
//     CAS composites, streamed textures binding into channel records)
// Invalidated when the mesh re-registers (tRModelData::Fixup /
// AddRenderInstance -> OnMeshRegistered), on the menus/loading flip and by
// the mesh-cache debug flush. Guarded by its own mutex: builders run on the
// guest render thread, invalidation on loader threads.
struct CachedItemCore {
  DrawItem item;  // draws empty, world identity, fetch overrides zero
  uint64_t fp_frame = 0;       // next fingerprint refresh (guest frames)
  uint64_t rebuild_frame = 0;  // next full rebuild
};
std::mutex g_item_cache_mutex;
std::unordered_map<uint32_t, CachedItemCore> g_item_cache;
std::atomic<uint64_t> g_item_cache_hits{0};
std::atomic<uint64_t> g_item_cache_builds{0};

void InvalidateCachedItem(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  g_item_cache.erase(mesh);
}

void ClearItemCache() {
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  g_item_cache.clear();
}

bool BuildItemFromMeshCached(uint8_t* base, uint32_t mesh, DrawItem& item) {
  const uint64_t frame = g_guest_frame;
  {
    std::lock_guard<std::mutex> lock(g_item_cache_mutex);
    auto it = g_item_cache.find(mesh);
    if (it != g_item_cache.end() && frame < it->second.rebuild_frame) {
      CachedItemCore& core = it->second;
      // Structural validation: the mesh's pointer fields must still match
      // the cached walk.
      const bool intact =
          REX_LOAD_U32(mesh + kMeshVertexBuffer) == core.item.vb_obj &&
          REX_LOAD_U32(mesh + kMeshIndexBuffer) == core.item.ib_obj;
      // Buffer payload address/size can move under the same objects
      // (streaming re-init), cheap guarded re-read every frame.
      uint32_t vb_words[3] = {}, ib_words[3] = {};
      if (intact &&
          GuestTryCopy(vb_words, REX_RAW_ADDR(core.item.vb_obj + kBufferPhysAddr),
                       sizeof(vb_words)) &&
          GuestTryCopy(ib_words, REX_RAW_ADDR(core.item.ib_obj + kBufferPhysAddr),
                       sizeof(ib_words)) &&
          (BSwap32(vb_words[0]) & 0xFFFFFFFC) == core.item.vb_addr &&
          BSwap32(vb_words[2]) == core.item.vb_bytes &&
          (BSwap32(ib_words[0]) & 0xFFFFFFFC) == core.item.ib_addr &&
          BSwap32(ib_words[2]) == core.item.ib_count) {
        // Fingerprint cadence: per frame for CPU-rewritten payloads
        // (skinned/ropa), every 4th frame otherwise (mesh_revalidate heals
        // streamed fills within that window; address reuse is caught by the
        // registration invalidation immediately).
        const bool dynamic_payload = core.item.skinned || core.item.ropa;
        if (dynamic_payload || frame >= core.fp_frame) {
          if (!ComputeItemFingerprint(base, core.item)) {
            g_rej_geom.fetch_add(1, std::memory_order_relaxed);
            g_item_cache.erase(it);
            return false;
          }
          core.fp_frame = frame + 4;
        }
        item = core.item;
        g_item_cache_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      g_item_cache.erase(it);
    }
  }
  if (!BuildItemFromMesh(base, mesh, item)) {
    return false;
  }
  g_item_cache_builds.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_item_cache_mutex);
  if (g_item_cache.size() >= 16384) {
    // Runaway growth backstop (streaming churn over a long session): drop
    // everything and let the next frames rebuild, one frame of full walks.
    g_item_cache.clear();
  }
  CachedItemCore core;
  core.item = item;
  core.fp_frame = frame + 4;
  // Materials with an unresolved diffuse retry the full walk quickly: CAS
  // composites and streamed channel textures bind shortly after first sight
  // (ocean.default legitimately has no diffuse and just rebuilds often,
  // a handful of items).
  core.rebuild_frame = frame + (item.diffuse_tex != 0 ? 32 : 2);
  g_item_cache[mesh] = std::move(core);
  return true;
}

bool BuildItemGeometry(uint8_t* base, uint32_t ctx, DrawItem& item) {
  const uint32_t record = REX_LOAD_U32(ctx);
  if (!GuestReadableApprox(base, record)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t mesh = REX_LOAD_U32(record);
  if (!GuestReadableApprox(base, mesh)) {
    g_rej_chain.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!BuildItemFromMeshCached(base, mesh, item)) {
    return false;
  }
  // Draw-time fetch overrides are per-frame state, applied after the cached
  // core, on every build.
  AdoptDrawFetchOverrides(base, item);

  // Culled island draw list from the context.
  const uint32_t draw_count = REX_LOAD_U16(ctx + kCtxDrawCountU16);
  const uint32_t draw_list = REX_LOAD_U32(ctx + kCtxDrawList);
  if (draw_count == 0 || draw_count > 512 || !GuestReadableApprox(base, draw_list)) {
    g_rej_draws.fetch_add(1, std::memory_order_relaxed);
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
  if (item.draws.empty()) {
    g_rej_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

}  // namespace

bool Enabled() { return SceneEnabled(); }

void FlushTextureCache() { g_flush_textures.store(true, std::memory_order_relaxed); }
void FlushMeshCache() { g_flush_meshes.store(true, std::memory_order_relaxed); }

int CycleSyntheticPan() {
  const int mode =
      (std::clamp(int(REXCVAR_GET(skate3_native_render_scene_synthetic_pan)), 0, 3) + 1) %
      4;
  REXCVAR_SET(skate3_native_render_scene_synthetic_pan, mode);
  static const char* kModeNames[] = {"off", "time-based (host clock at scene build)",
                                     "fixed angle step per frame",
                                     "synthetic samples through the smoother"};
  REXLOG_INFO("native-scene synthetic-pan: hotkey -> mode {} ({})", mode,
              kModeNames[mode]);
  return mode;
}

void RecordBoneSignal(double seconds) {
  g_bonesig_request.store(seconds, std::memory_order_release);
  REXLOG_INFO(
      "native-scene bone-signal: recording {} s; skate past the camera at a steady "
      "speed NOW",
      seconds);
}

void RecordCameraSignal(double seconds) {
  {
    std::lock_guard<std::mutex> lock(g_camsig_mutex);
    g_camsig.clear();
    g_camsig.reserve(size_t(seconds * 1400.0));
  }
  const double now =
      std::chrono::duration<double>(PerfClock::now().time_since_epoch()).count();
  g_camsig_deadline.store(now + seconds, std::memory_order_release);
  REXLOG_INFO(
      "native-scene cam-signal: recording {} s; pan the camera with the stick at a "
      "steady rate NOW",
      seconds);
}

bool ToggleSceneEnabled() {
  if (!REXCVAR_GET(skate3_native_render)) {
    REXLOG_WARN(
        "native-scene: renderer toggle ignored; the skate3_native_render hook layer "
        "is off (set it and restart; it installs the capture hooks the scene needs)");
    return false;
  }
  const bool enabled = !SceneEnabled();
  if (enabled) {
    // Capture idles while the emulated renderer is active, so anything still
    // published is from before the switch away (stale camera). Drop it:
    // RenderScene yields to the emulated frame until the capture hooks
    // publish a fresh scene (the next frame).
    {
      std::lock_guard<std::mutex> lock(g_scene_mutex);
      g_scene.reset();
    }
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      g_scene_2d.clear();
      g_scene_spline.clear();
    }
    // Warm up before taking over: after a long emulated stretch the decode
    // caches can be cold/stale, and the takeover frame would pay the whole
    // decode burst at once. A warm cache completes warmup in one frame.
    g_warmup_armed.store(true, std::memory_order_relaxed);
  }
  REXCVAR_SET(skate3_native_render_scene, enabled);
  REXLOG_INFO("native-scene: switched to the {} renderer (runtime toggle)",
              enabled ? "NATIVE" : "EMULATED");
  return enabled;
}

void OnRegisterTexture(uint64_t guid, uint32_t texture) {
  if (texture == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_texture_map_mutex);
  g_texture_map[guid & kGuidMask] = texture;
}

void OnMeshRegistered(uint8_t* base, uint32_t mesh) {
  if (mesh == 0) {
    return;
  }
  // A (re-)registration is the "content changed at this address" signal;
  // drop the cached item core so the next frame re-walks it.
  InvalidateCachedItem(mesh);
  // Publish the guest base here too: at boot the loading-screen prewarm
  // workers run before the first gameplay frame would otherwise publish it.
  g_guest_base.store(base, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_prewarm_queue.size() < 65536 && g_prewarm_seen.insert(mesh).second) {
    g_prewarm_queue.push_back({mesh, 60});
    g_prewarm_cv.notify_one();
  }
}

// Validate a candidate tRModelData against the layout the game's OWN
// AddRenderInstance (sub_82791290) walks: mesh table pointer at model+0x24,
// mesh COUNT as a u16 at model+0x32 (`lhz r10,50(r11)`). Table entries are
// 8 bytes: word0 = mesh, word1 = 0 for the optimesh world form (its
// material lives at mesh+0x24, which is also why a {mesh, material} pair
// scan finds nothing) / non-zero for the tRMeshData character/prop form.
// Every dereference is SEH-guarded: the offset probe feeds this arbitrary
// instance words (bbox floats, guids) as candidates.
static bool PlausibleRModel(uint8_t* base, uint32_t model) {
  uint32_t count_w = 0, table = 0;
  if (!GuestTryLoadU32(base, model + 0x30, &count_w) ||
      !GuestTryLoadU32(base, model + 0x24, &table)) {
    return false;
  }
  const uint32_t num_meshes = count_w & 0xFFFF;  // u16 at +0x32
  if (num_meshes == 0 || num_meshes > 512) {
    return false;
  }
  // Entry 0: the mesh must carry a plausible material (channel count sane).
  uint32_t mesh0 = 0, mesh0_mat = 0, mat_channels = 0;
  return GuestTryLoadU32(base, table, &mesh0) &&
         GuestTryLoadU32(base, mesh0 + kMeshMaterial, &mesh0_mat) &&
         mesh0_mat >= 0x10000 &&
         GuestTryLoadU32(base, mesh0_mat, &mat_channels) && mat_channels != 0 &&
         mat_channels <= 64;
}

// Queue every optimesh-form mesh of a validated tRModelData for the prewarm
// decode workers.
static void QueueModelMeshes(uint8_t* base, uint32_t model) {
  uint32_t count_w = 0, table = 0;
  if (!GuestTryLoadU32(base, model + 0x30, &count_w) ||
      !GuestTryLoadU32(base, model + 0x24, &table)) {
    return;
  }
  const uint32_t num_meshes = count_w & 0xFFFF;
  for (uint32_t i = 0; i < num_meshes && i < 512; ++i) {
    uint32_t mesh = 0, entry_mat = 0;
    if (!GuestTryLoadU32(base, table + i * 8, &mesh) ||
        !GuestTryLoadU32(base, table + i * 8 + 4, &entry_mat)) {
      continue;
    }
    if (entry_mat != 0) {
      continue;  // tRMeshData form (characters/props), wrong offsets
    }
    OnMeshRegistered(base, mesh);
  }
}

void OnModelFixup(uint8_t* base, uint32_t model) {
  // Fires per model during the load's DISK-STREAMING phase (arena fixup),
  // the early prewarm source. The validation gate keeps non-render models
  // (and any layout drift) out of the queue.
  if (model != 0 && PlausibleRModel(base, model)) {
    QueueModelMeshes(base, model);
  }
}

void OnAddRenderInstance(uint8_t* base, uint32_t instance) {
  if (instance == 0) {
    return;
  }
  // Resolve tInstance::m_pRModel. The game's own AddRenderInstance reads it
  // at +0x80 (`lwz r11,128(r4)`); the offset is still confirmed by the
  // validation probe before first use so an image-version drift degrades to
  // "prewarm off" instead of queueing garbage.
  uint32_t off = g_instance_rmodel_offset.load(std::memory_order_relaxed);
  if (off == 0) {
    for (uint32_t cand = 0x80; cand < 0xC0; cand += 4) {
      uint32_t model = 0;
      if (GuestTryLoadU32(base, instance + cand, &model) &&
          PlausibleRModel(base, model)) {
        g_instance_rmodel_offset.store(cand, std::memory_order_relaxed);
        REXLOG_INFO("native-scene: tInstance::m_pRModel offset confirmed at +0x{:X}",
                    cand);
        off = cand;
        break;
      }
    }
    if (off == 0) {
      return;  // CModel-only / embedded instance, or the model is not ready
    }
  }
  uint32_t model = 0;
  if (!GuestTryLoadU32(base, instance + off, &model) ||
      !PlausibleRModel(base, model)) {
    return;  // instances without a renderable model are normal
  }
  QueueModelMeshes(base, model);
}

void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device) {
  (void)base;
  (void)mask;
  if (device != 0) {
    g_device.store(device, std::memory_order_relaxed);
  }
  if (!SceneEnabled() || ptr == 0) {
    return;
  }
  if (bank == 0x4400) {
    g_ps_bank.store(ptr, std::memory_order_relaxed);
    return;
  }
  if (bank != 0x4000) {
    return;
  }
  g_vs_uploads.fetch_add(1, std::memory_order_relaxed);
  g_vs_bank.store(ptr, std::memory_order_relaxed);
}

void On2dPhase(uint32_t bit, bool enter) {
  if (bit >= 6) {
    return;
  }
  if (enter) {
    g_phase2d_depth[bit].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_phase2d_depth[bit].fetch_sub(1, std::memory_order_relaxed);
  }
}

uint32_t Phase2dFlags() {
  uint32_t flags = 0;
  for (uint32_t bit = 0; bit < 6; ++bit) {
    if (g_phase2d_depth[bit].load(std::memory_order_relaxed) != 0) {
      flags |= 1u << bit;
    }
  }
  return flags;
}


void OnSetShader(bool pixel, uint32_t obj) {
  (pixel ? g_cur_ps_obj : g_cur_vs_obj).store(obj, std::memory_order_relaxed);
}

void OnRenderStateUpload(uint64_t mask, uint32_t bank, uint32_t ptr) {
  (void)mask;
  if (ptr == 0) {
    return;
  }
  // Log each distinct bank id once, the AluConstants analog of discovering
  // 0x4000/0x4400. Tiny lock-free seen-set (at most a handful of banks).
  static std::atomic<uint32_t> seen[8];
  for (auto& slot : seen) {
    uint32_t cur = slot.load(std::memory_order_relaxed);
    if (cur == bank) {
      break;
    }
    if (cur == 0) {
      uint32_t expected = 0;
      if (slot.compare_exchange_strong(expected, bank, std::memory_order_relaxed)) {
        REXLOG_INFO("native-scene: render-state bank id={:#x} ptr={:08X}", bank, ptr);
        break;
      }
      if (expected == bank) {
        break;
      }
    }
  }
  g_rs_bank.store(ptr, std::memory_order_relaxed);
}

void OnSetViewport(uint8_t* base, uint32_t viewport_ptr) {
  if (viewport_ptr == 0) {
    return;
  }
  for (int i = 0; i < 6; ++i) {
    g_cur_viewport[i].store(REX_LOAD_U32(viewport_ptr + i * 4), std::memory_order_relaxed);
  }
}

void OnSetScissor(uint8_t* base, uint32_t rect_ptr) {
  if (rect_ptr == 0) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    g_cur_scissor[i].store(REX_LOAD_U32(rect_ptr + i * 4), std::memory_order_relaxed);
  }
}

// The character.hair pixel shader keeps the per-character hair color at PS
// c17 (fixed layout for that shader; verified from recorded PS banks;
// c16/c17 hold the dark ambient/diffuse hair color pair while other
// registers carry lighting globals). Used as captured.
void CaptureHairTint(uint8_t* base, DrawItem& item) {
  const uint32_t ps = g_ps_bank.load(std::memory_order_relaxed);
  if (ps == 0) {
    return;
  }
  float rgb[3];
  for (int i = 0; i < 3; ++i) {
    rgb[i] = LoadGuestF32(base, ps + (17 * 4 + i) * 4);
    if (!(rgb[i] >= 0.0f && rgb[i] <= 4.0f)) {
      return;  // implausible bank contents; keep the previous/no tint
    }
  }
  for (int i = 0; i < 3; ++i) {
    item.tint[i] = rgb[i];
  }
  item.tint[3] = 1.0f;
}

// Character-family lighting capture: reads the family-specific rows of the
// PIXEL constant bank into the canonical block the scene PS character branch
// consumes (cbuffer CH at b2). Row maps come from the disassembled Skate 3
// pixel shaders (offline-validated by running the actual ucode per pixel):
//   defaultcharacter (fam 1): light c0, key c6, ambMult c10.w, SH c14..c22
//     scaled by c12.y, exposure c4.z, alpha c13.x.
//   cacstamp/cac_* (fam 2): light c9, key c15, ambMult c19.w, SH c24..c32
//     scaled by c21.y, exposure c13.z, alpha c22.x, diffuse tint c23.
//   livingworld_stamp (fam 3): light c9, key c15, FLAT ambient = c19.w *
//     (0.1, 0.175, 0.3) (shader literal), exposure c13.z, alpha c21.x, stamp
//     recolor tints c22 (red mask) / c23 (blue mask).
//   cac_hair (fam 4): light c4, key c16, ambient c14.w * 0.25, fresnel tint
//     c17 with power c11.w, exposure c8.z, strand-alpha scale c15.x.
//   defaulthair (fam 5): light c4, key c15, ambient c14.w * 0.25, fresnel
//     tint c10 with power c9.z, exposure c8.z, strand-alpha scale c16.x.
//   vehicle (fam 6, character.livingworld_vehicles): light c9, key c15
//     (fresnel power in c15.w), FLAT ambient = c19.w * (0.1, 0.175, 0.3)
//     (same literal as livingworld), exposure c13.z, phong spec color c16
//     with power c16.w (stored in the unused SH row 0), paint recolor
//     colorize_red c21 / colorize_blue c22 (vehicle.fx: where diffuse green
//     is below the mask threshold, rgb = r * red_tint + b * blue_tint;
//     that is the taxi yellow; validated by executing the captured
//     vehicle_defaultPS offline).
//   vehicle glass (fam 7): same rows; glass tint c18.rgb (zero = the color
//     is reflection-only) in the tintA slot, alpha out = c20.x * c18.w.
// The SH irradiance evaluation is sat(c_base + s*(N.x*r1 + N.y*r2 + N.z*r3)
// + s^2*(NxNz*r4 + NzNy*r5 + NyNx*r6) + (3 s^2 Nz^2 - 1)*r7 + s^2*(Nx^2 -
// Ny^2)*r8); the scale and the -1 are folded into the stored rows so the
// shader evaluates a plain 9-row basis.
//
// Canonical block (15 float4 rows): [0] = light dir + hair fresnel power,
// [1] = key color + exposure, [2] = flat ambient rgb + SH-ambient
// multiplier (hair ambient scalar in w), [3..11] = SH rows, [12] = tintA
// (w = apply), [13] = tintB + strand-alpha scale, [14].x = alpha out,
// [14].y = family (0 = capture failed validation -> legacy shading).
void CaptureCharLighting(uint8_t* base, DrawItem& item) {
  if (item.char_family == 0) {
    return;
  }
  const uint32_t ps = g_ps_bank.load(std::memory_order_relaxed);
  if (ps == 0) {
    return;
  }
  g_char_attempts.fetch_add(1, std::memory_order_relaxed);
  const auto row = [&](uint32_t r, uint32_t c) {
    return LoadGuestF32(base, ps + (r * 4 + c) * 4);
  };
  // Build locally and commit only on success: the capture can run again on a
  // later draw with the same buffers (the caster-pass bank is stale; its
  // shadowPS touches no PS constants), and a failed refresh must not wipe
  // rows a previous successful capture staged.
  float local[60];
  float* d = local;
  std::memset(local, 0, sizeof(local));
  uint8_t fam = item.char_family;
  const auto rows_valid = [&](uint8_t f, float* light, float* expo, float* key) {
    uint32_t light_r = 9, key_r = 15, expo_r = 13, expo_c = 2;
    switch (f) {
      case 1: light_r = 0; key_r = 6; expo_r = 4; expo_c = 2; break;
      case 2: case 3: case 6: case 7: break;  // defaults above
      case 4: light_r = 4; key_r = 16; expo_r = 8; expo_c = 2; break;
      case 5: light_r = 4; key_r = 15; expo_r = 8; expo_c = 2; break;
    }
    float norm2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
      light[i] = row(light_r, uint32_t(i));
      if (!(light[i] > -4.0f && light[i] < 4.0f)) return false;
      norm2 += light[i] * light[i];
    }
    // A real light-direction row carries w = 0; the world materials'
    // shadow-transform rows are ALSO unit in xyz but keep the light-space
    // translation (hundreds of meters) in w; reject those banks.
    const float light_w = row(light_r, 3);
    if (!(light_w > -1.0f && light_w < 1.0f)) return false;
    *expo = row(expo_r, expo_c);
    for (int i = 0; i < 3; ++i) {
      key[i] = row(key_r, uint32_t(i));
      if (!(key[i] >= 0.0f && key[i] < 64.0f)) return false;
    }
    // The bank can hold another pass's constants at our capture moment
    // (characters render a shadow-caster pass first); the light-dir norm
    // and exposure gates reject those; the item then keeps its previous /
    // legacy shading.
    return norm2 > 0.25f && norm2 < 2.25f && *expo > 0.25f && *expo < 16.0f;
  };
  float light[3];
  float key[3];
  float expo = 0.0f;
  if (!rows_valid(fam, light, &expo, key)) {
    // NPC skin (character_skin_defaultPS) shares the "character.skin"
    // attribulator name with the CAC player skin (cacstamp_skin) but uses
    // the DEFAULTCHARACTER register layout; the two banks are mutually
    // exclusive on the light-row position (the other layout's slot holds
    // non-unit data), so a failed fam-2 read retries as fam 1.
    if (fam == 2 && rows_valid(1, light, &expo, key)) {
      fam = 1;
    } else {
      static std::atomic<uint32_t> rej_log{0};
      const uint32_t n = rej_log.fetch_add(1, std::memory_order_relaxed);
      if (n < 16 || (n & 2047u) == 0) {
        REXLOG_INFO(
            "native-scene: char capture REJECTED fam={} light=({:.3f},{:.3f},{:.3f}) "
            "expo={:.3f} key=({:.3f},{:.3f},{:.3f})",
            fam, light[0], light[1], light[2], expo, key[0], key[1], key[2]);
      }
      return;
    }
  }
  d[0] = light[0]; d[1] = light[1]; d[2] = light[2];
  d[4] = key[0]; d[5] = key[1]; d[6] = key[2];
  d[7] = expo;
  if (fam == 1 || fam == 2) {
    const uint32_t sh_base = fam == 1 ? 14u : 24u;
    const float s = row(fam == 1 ? 12u : 21u, 1);
    const float s2 = s * s;
    const float amb_mult = row(fam == 1 ? 10u : 19u, 3);
    if (!(s > -8.0f && s < 8.0f) || !(amb_mult >= 0.0f && amb_mult < 16.0f)) {
      return;
    }
    d[2 * 4 + 3] = amb_mult;
    for (int r = 0; r < 9; ++r) {
      const float scale = r == 0 ? 1.0f : (r <= 3 ? s : (r == 7 ? 3.0f * s2 : s2));
      for (int c = 0; c < 3; ++c) {
        float v = row(sh_base + uint32_t(r), uint32_t(c)) * scale;
        if (!(v > -64.0f && v < 64.0f)) v = 0.0f;
        d[(3 + r) * 4 + c] = v;
      }
    }
    for (int c = 0; c < 3; ++c) {
      // Fold the (3 s^2 Nz^2 - 1) term's -1 into the base row.
      d[3 * 4 + c] -= row(sh_base + 7u, uint32_t(c));
    }
    if (fam == 2) {
      // CAC diffuse/skin tint (c23): multiplies the squared diffuse.
      bool tint_ok = true;
      float tint[3];
      for (int c = 0; c < 3; ++c) {
        tint[c] = row(23u, uint32_t(c));
        tint_ok = tint_ok && tint[c] >= 0.0f && tint[c] < 16.0f;
      }
      if (tint_ok) {
        d[12 * 4 + 0] = tint[0];
        d[12 * 4 + 1] = tint[1];
        d[12 * 4 + 2] = tint[2];
        d[12 * 4 + 3] = 1.0f;
      }
    }
    d[14 * 4 + 0] = row(fam == 1 ? 13u : 22u, 0);
  } else if (fam == 3) {
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 0] = amb * 0.1f;
    d[2 * 4 + 1] = amb * 0.175f;
    d[2 * 4 + 2] = amb * 0.3f;
    for (int c = 0; c < 3; ++c) {
      d[12 * 4 + c] = std::clamp(row(22u, uint32_t(c)), 0.0f, 4.0f);
      d[13 * 4 + c] = std::clamp(row(23u, uint32_t(c)), 0.0f, 4.0f);
    }
    d[12 * 4 + 3] = 1.0f;
    d[14 * 4 + 0] = row(21u, 0);
  } else if (fam == 6 || fam == 7) {
    // vehicle.fx body / vehicle_glass.fx windows (row map above). The
    // otherwise-unused SH row 0 carries the phong spec color + power.
    const float amb = row(19u, 3);
    if (!(amb >= 0.0f && amb < 16.0f)) return;
    d[2 * 4 + 0] = amb * 0.1f;
    d[2 * 4 + 1] = amb * 0.175f;
    d[2 * 4 + 2] = amb * 0.3f;
    d[3] = std::clamp(row(15u, 3), 1.0f, 64.0f);  // fresnel power c15.w
    for (int c = 0; c < 4; ++c) {
      d[3 * 4 + c] = std::clamp(row(16u, uint32_t(c)), 0.0f, 64.0f);
    }
    if (fam == 6) {
      for (int c = 0; c < 3; ++c) {
        d[12 * 4 + c] = std::clamp(row(21u, uint32_t(c)), 0.0f, 4.0f);
        d[13 * 4 + c] = std::clamp(row(22u, uint32_t(c)), 0.0f, 4.0f);
      }
      d[12 * 4 + 3] = 1.0f;
      d[14 * 4 + 0] = 1.0f;  // opaque body
    } else {
      // Glass tint c18.rgb multiplies the ambient/key terms (zero in every
      // capture = reflection-only glass); alpha out = c20.x * c18.w.
      for (int c = 0; c < 3; ++c) {
        d[12 * 4 + c] = std::clamp(row(18u, uint32_t(c)), 0.0f, 4.0f);
      }
      d[12 * 4 + 3] = 1.0f;
      d[14 * 4 + 0] = std::clamp(row(20u, 0) * row(18u, 3), 0.0f, 1.0f);
    }
  } else {
    // Hair: flat ambient scalar + fresnel rim tint, strand-alpha scale.
    const float amb = row(14u, 3) * 0.25f;
    d[2 * 4 + 3] = std::clamp(amb, 0.0f, 4.0f);
    d[3] = std::clamp(row(fam == 4 ? 11u : 9u, fam == 4 ? 3u : 2u), 1.0f, 64.0f);
    const uint32_t fres_r = fam == 4 ? 17u : 10u;
    for (int c = 0; c < 3; ++c) {
      d[13 * 4 + c] = std::clamp(row(fres_r, uint32_t(c)), 0.0f, 8.0f);
    }
    d[13 * 4 + 3] = std::clamp(row(fam == 4 ? 15u : 16u, 0), 0.0f, 4.0f);
    d[14 * 4 + 0] = 1.0f;
  }
  d[14 * 4 + 1] = float(fam);
  std::memcpy(item.char_rows, local, sizeof(item.char_rows));
  g_char_valid.fetch_add(1, std::memory_order_relaxed);
  // Remember the validated rows per garment for the cross-frame fallback
  // (see g_char_rows_cache). Runaway-growth backstop only; mesh keys are
  // bounded by loaded character content in practice.
  if (g_char_rows_cache.size() > 4096) {
    g_char_rows_cache.clear();
  }
  std::memcpy(g_char_rows_cache[item.mesh].data(), local, sizeof(local));
}

// Copy the staged bone palette (and, for hair, the tint) out of the shadow
// banks into the item. The banks are reused draw to draw, hence the copy.
// The base from BankPaletteBase is refined by +1 for the cloth/morph VS
// layout (extra parameter row before the palette); the mesh's own sample
// vertices projected with the bank's viewproj decide (RefinePaletteBase).
//
// character.cloth_ropa items (Ropa cloth-simulated garments, the player's
// tee) use a VS that BRANCHES on the row in front of the palette (c4
// pre-pass / c7 main-pass; disassembled from a live
// capture): flag.x > 0 skins with the palette one
// register late (c5/c8); flag.x <= 0 means the CPU cloth sim already wrote
// deformed root-local positions into the (dynamic, per-frame) VB and the VS
// ignores palette and blend attributes entirely, applying ONE affine at
// c188 (pre-pass) / c191 (main-pass): 3 column-vector [R | t] rows, same
// packing as palette rows. Skinning the simulated vertices instead renders
// the garment as a mangled ribbon off the body (the distorted-player-shirt
// bug). Offline validation: the rigid rows put 31/31 sampled shirt verts in
// clip at the player's position; the skinned interpretation scores 0/31.
// Transform up to 6 sample vertices of the item by the affine at register m
// (3 column-vector [R | t] rows) and project them with the bank's own
// viewproj (c0..c3). Returns the count of samples inside the clip volume,
// -1 when unscorable. A stale bank (the submit-exit capture can run after
// ANOTHER mesh's draw when this mesh's own draws are deferred) holds some
// other object's matrix at c188/c191; geometry projected with it lands
// far off-clip, while the true matrix scores full (validated offline:
// 31/31 vs 0/31 on an F10 capture).
int ScoreRigidAffine(uint8_t* base, uint32_t bank, uint32_t m, const DrawItem& item) {
  if (item.stride == 0) return -1;
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2) return -1;
  float vp[16];
  float rows[12];
  for (int i = 0; i < 16; ++i) {
    vp[i] = LoadGuestF32(base, bank + i * 4);
    if (!(vp[i] > -1e9f && vp[i] < 1e9f)) return -1;
  }
  for (int i = 0; i < 12; ++i) {
    rows[i] = LoadGuestF32(base, bank + (m * 4 + i) * 4);
  }
  constexpr uint32_t kSamples = 6;
  int ok = 0;
  int n = 0;
  for (uint32_t s = 0; s < kSamples; ++s) {
    const uint32_t v = item.vb_addr + (s * (count - 1) / (kSamples - 1)) * item.stride;
    const uint32_t pa = v + item.pos_offset;
    float p[3];
    switch (item.pos_fmt) {
      case 57:
        for (int a = 0; a < 3; ++a) p[a] = LoadGuestF32(base, pa + a * 4);
        break;
      case 32:
        for (int a = 0; a < 3; ++a) {
          p[a] = GuestHalfToFloat(uint16_t(REX_LOAD_U16(pa + a * 2)));
        }
        break;
      case 26: {
        constexpr float kScale = 2.0f / 32767.0f;
        for (int a = 0; a < 3; ++a) {
          p[a] = int16_t(REX_LOAD_U16(pa + a * 2)) * kScale + (a == 1 ? 0.8f : 0.0f);
        }
        break;
      }
      default:
        return -1;
    }
    float q[3];
    for (int a = 0; a < 3; ++a) {
      q[a] = rows[a * 4] * p[0] + rows[a * 4 + 1] * p[1] + rows[a * 4 + 2] * p[2] +
             rows[a * 4 + 3];
    }
    float clip[4];
    for (int r = 0; r < 4; ++r) {
      clip[r] = vp[r * 4] * q[0] + vp[r * 4 + 1] * q[1] + vp[r * 4 + 2] * q[2] +
                vp[r * 4 + 3];
    }
    const float aw = std::abs(clip[3]) < 1.0f ? 1.0f : std::abs(clip[3]);
    ++n;
    if (std::abs(clip[0]) <= 1.5f * aw && std::abs(clip[1]) <= 1.5f * aw) {
      ++ok;
    }
  }
  return n == 0 ? -1 : (ok * 16) / n;
}

// Returns false when the bank could not be consumed for this item (ropa
// rigid matrix implausible or off-clip = stale bank); the caller must
// leave/mark the item pending so a later matching draw re-captures it.
bool CaptureSkinnedState(uint8_t* base, uint32_t bank, uint32_t palette_base,
                         DrawItem& item) {
  if (item.ropa && palette_base != 0) {
    const bool main_pass = palette_base >= 7;
    const uint32_t flag_reg = main_pass ? 7u : 4u;
    const float flag_x = LoadGuestF32(base, bank + (flag_reg * 4) * 4);
    // Diagnosis logging (rate-limited): the flag/matrix registers were
    // verified against an emulated-mode F10 capture; this confirms what the
    // live native-mode banks actually hold at OUR capture moments.
    static std::atomic<uint32_t> ropa_log_count{0};
    const uint32_t ln = ropa_log_count.fetch_add(1, std::memory_order_relaxed);
    if (ln < 8 || (ln & 1023u) == 0) {
      const uint32_t m = main_pass ? 191u : 188u;
      REXLOG_INFO(
          "native-scene: ropa mesh={:08X} vb={:08X} base={} score={} "
          "flag=({:.3f},{:.3f},{:.3f},{:.3f}) "
          "m[c{}]=({:.3f},{:.3f},{:.3f},{:.2f})({:.3f},{:.3f},{:.3f},{:.2f})({:.3f},{:.3f},{:.3f},{:.2f})",
          item.mesh, item.vb_obj, palette_base, ScoreRigidAffine(base, bank, m, item),
          LoadGuestF32(base, bank + (flag_reg * 4 + 0) * 4),
          LoadGuestF32(base, bank + (flag_reg * 4 + 1) * 4),
          LoadGuestF32(base, bank + (flag_reg * 4 + 2) * 4),
          LoadGuestF32(base, bank + (flag_reg * 4 + 3) * 4), m,
          LoadGuestF32(base, bank + ((m + 0) * 4 + 0) * 4),
          LoadGuestF32(base, bank + ((m + 0) * 4 + 1) * 4),
          LoadGuestF32(base, bank + ((m + 0) * 4 + 2) * 4),
          LoadGuestF32(base, bank + ((m + 0) * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 0) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 1) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 2) * 4),
          LoadGuestF32(base, bank + ((m + 1) * 4 + 3) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 0) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 1) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 2) * 4),
          LoadGuestF32(base, bank + ((m + 2) * 4 + 3) * 4));
    }
    if (flag_x > 0.0f) {
      // Sim inactive: the palette sits one register late (c5/c8). The LAYOUT
      // is exact, but the BANK can still be foreign, the same stale-bank
      // hazard the rigid branch below scores against (the flag itself was
      // read from the foreign bank, so a positive x proves nothing). Blind
      // acceptance here staged whatever the foreign bank held as an 84-bone
      // palette; when the garment's sim was actually ACTIVE (skating NPCs
      // toggle with distance/activity) that skinned the sim-deformed
      // vertices, the mangled map-length ribbon (ScoreRigidAffine's 0/31
      // interpretation), shadows matching because the caster pass shares the
      // item. Gate through the same sample-projection acceptance as every
      // other palette; on refusal the item stays pending (post-draw fixup /
      // ropa state rescue).
      palette_base = RefinePaletteBase(base, bank, main_pass ? 8u : 5u, item);
      if (palette_base == 0) {
        g_ropa_stale.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    } else {
      const uint32_t m = main_pass ? 191u : 188u;
      float rows[12];
      bool plausible = true;
      for (int r = 0; r < 3 && plausible; ++r) {
        float n = 0.0f;
        for (int i = 0; i < 4; ++i) {
          const float f = LoadGuestF32(base, bank + ((m + r) * 4 + i) * 4);
          if (!(f > -1e7f && f < 1e7f)) {
            plausible = false;
            break;
          }
          rows[r * 4 + i] = f;
          if (i < 3) n += f * f;
        }
        plausible = plausible && n > 0.0025f && n < 400.0f &&
                    rows[r * 4 + 3] > -20000.f && rows[r * 4 + 3] < 20000.f;
      }
      if (plausible && ScoreRigidAffine(base, bank, m, item) >= 8) {
        // Column-vector [R | t] rows -> item.world (row-vector, t in row 3).
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            item.world[i * 4 + j] = rows[j * 4 + i];
          }
          item.world[i * 4 + 3] = 0.0f;
          item.world[12 + i] = rows[i * 4 + 3];
        }
        item.world[15] = 1.0f;
        item.skinned = false;
        item.bones.clear();
        g_ropa_rigid.fetch_add(1, std::memory_order_relaxed);
        CaptureCharLighting(base, item);
        return true;
      }
      // Implausible or off-clip matrix: the bank belongs to another mesh
      // (the submit-exit capture runs after whatever draw happened to be
      // inline). Refuse it; the caller keeps the item pending and the
      // post-draw fixup re-captures from this mesh's own draw.
      g_ropa_stale.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  } else {
    palette_base = RefinePaletteBase(base, bank, palette_base, item);
    if (palette_base == 0) {
      // The bank's palette provably does not skin this mesh into the bank's
      // own view (foreign/stale bank): refuse; the caller keeps the item
      // pending and the post-draw (ib,vb) fixup re-captures on a real draw.
      return false;
    }
  }
  constexpr uint32_t kPaletteFloats = 84 * 12;  // c4..c255 = up to 84 bones
  item.bones.resize(kPaletteFloats);
  for (uint32_t i = 0; i < kPaletteFloats; ++i) {
    const float f = LoadGuestF32(base, bank + (palette_base * 4 + i) * 4);
    item.bones[i] = (f > -1e7f && f < 1e7f) ? f : 0.0f;
  }
  if (item.hair) {
    CaptureHairTint(base, item);
  }
  CaptureCharLighting(base, item);
  return true;
}

uint64_t DrawSequence() { return g_draw_seq.load(std::memory_order_relaxed); }

uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx, bool world_path,
                             bool drew_inside) {
  if (!SceneEnabled()) {
    return 0;
  }
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0) {
    return 0;
  }
  // Guest-thread capture cost telemetry (folded per frame in BuildFrameScene).
  const auto perf_t0 = PerfClock::now();
  struct PerfFold {
    PerfClock::time_point t0;
    ~PerfFold() {
      g_capture_frame_ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
              .count());
    }
  } perf_fold{perf_t0};
  if (world_path) {
    // Cheap pre-check so the world path (~800 calls/frame) only pays the
    // full capture for the meshes that need per-draw transforms: skinned
    // (LOD pedestrians, jointed props) and rigid MODEL-SPACE props (position
    // fmt 32/26). Movable props (vending machines) can reach the frame ONLY
    // through the sort lists, rendered as absolute world geometry they
    // collapse at the origin. Absolute meshes (fmt 57) stay on the identity
    // world path.
    const uint32_t record = REX_LOAD_U32(ctx);
    if (!GuestReadableApprox(base, record)) return 0;
    const uint32_t mesh = REX_LOAD_U32(record);
    if (!GuestReadableApprox(base, mesh)) return 0;
    const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
    if (!GuestReadableApprox(base, vdesc)) return 0;
    const uint32_t num_elements = REX_LOAD_U16(vdesc + 8);
    if (num_elements == 0 || num_elements > 32) return 0;
    bool have_bw = false;
    bool have_bi = false;
    uint32_t pos_fmt = 0;
    bool have_pos = false;
    for (uint32_t i = 0; i < num_elements; ++i) {
      const uint32_t e = vdesc + 0x10 + i * 16;
      if (REX_LOAD_U16(e) != 0) continue;
      const uint32_t usage = REX_LOAD_U8(e + 9);
      if (usage == 0 && !have_pos) {
        pos_fmt = REX_LOAD_U32(e + 4) & 0x3F;
        have_pos = true;
      }
      if (usage == 1 && (REX_LOAD_U32(e + 4) & 0x3F) == 6) have_bw = true;
      if (usage == 2 && (REX_LOAD_U32(e + 4) & 0x3F) == 6) have_bi = true;
    }
    const bool skinned = have_bw && have_bi;
    const bool model_space = pos_fmt == 32 || pos_fmt == 26;
    if (!skinned && !model_space) {
      return 0;
    }
  }
  DrawItem item;
  if (!BuildItemGeometry(base, ctx, item)) {
    return 0;
  }
  // Rigid transform: the bank only holds this mesh's constants if its own
  // draws ran inside the submit call. Deferred (multi-pass) rigid props draw
  // later; the bank belongs to some earlier mesh, and a leftover identity
  // matrix at c4 VALIDATES as a plausible world (verified from recorded
  // draw streams: 4 of 6 vending-machine clones captured exact identity and
  // rendered invisibly at the origin). Defer those to the post-draw fixup,
  // like skinned palettes.
  if (!item.skinned) {
    if (!drew_inside || !BankRigidWorld(base, bank, item.world)) {
      item.pending = true;
      g_rigid_pending.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (item.skinned) {
    // Copy the whole possible palette span (c4..c255 = 84 bones) verbatim:
    // 3 float4 rows per bone, column-vector affine [R | t], model space
    // directly to world space (verified live: bone translations match the
    // camera focus). The shader applies rows with explicit dot products,
    // sidestepping HLSL matrix packing. Registers beyond the mesh's real
    // bone count hold stale bank data but are never indexed. World stays
    // identity.
    //
    // The bank only holds this mesh's palette at c4 if its own draws ran
    // inside the call AND used the pre-pass layout. Multi-pass (deferred)
    // meshes draw later, and main-pass-layout banks have the camera at c4;
    // in both cases leave the palette pending for the post-draw fixup.
    const uint32_t effect_list = REX_LOAD_U32(ctx + kCtxEffectList);
    uint32_t passes = 1;
    if (GuestReadableApprox(base, effect_list)) {
      passes = REX_LOAD_U32(effect_list + kEffectListPassCount);
    }
    const uint32_t palette_base = BankPaletteBase(base, bank);
    // World-path captures come from the sort-list hook BEFORE any of the
    // mesh's draws ran; the bank belongs to some other mesh; always defer
    // to the post-draw fixup. Same when no draw ran inside the submit call
    // (deferred mesh): a stale bank can still hold plausible bone rows.
    item.pending = world_path || !drew_inside || (passes > 1 && passes < 16) ||
                   palette_base == 0;
    if (!item.pending) {
      if (CaptureSkinnedState(base, bank, palette_base, item)) {
        g_palette_snapshots.fetch_add(1, std::memory_order_relaxed);
      } else {
        item.pending = true;
      }
    }
    g_skinned_items.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_recording.load(std::memory_order_relaxed)) {
    // Preserve the buffer payload for offline decode (streaming arenas are
    // recycled long before the end-of-window memory snapshot).
    std::lock_guard<std::mutex> lock(g_record_mutex);
    const uint64_t key = uint64_t(item.vb_addr) * 1099511628211ull ^ item.fingerprint;
    if (item.vb_bytes <= (1u << 20) && g_recorded_buffer_bytes < (512u << 20) &&
        g_recorded_buffer_keys.insert(key).second) {
      RecordedBuffer buf;
      buf.vb_addr = item.vb_addr;
      buf.ib_addr = item.ib_addr;
      buf.fingerprint = item.fingerprint;
      buf.vb.resize(item.vb_bytes);
      std::memcpy(buf.vb.data(), base + item.vb_addr, item.vb_bytes);
      buf.ib.resize(size_t(item.ib_count) * 2);
      std::memcpy(buf.ib.data(), base + item.ib_addr, buf.ib.size());
      g_recorded_buffer_bytes += buf.vb.size() + buf.ib.size();
      g_recorded_buffers.push_back(std::move(buf));
    }
  }
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  g_frame_dynitems.push_back(std::move(item));
  const size_t index = g_frame_dynitems.size() - 1;
  if (g_frame_dynitems[index].pending) {
    const DrawItem& d = g_frame_dynitems[index];
    g_frame_pending_by_buffers.emplace((uint64_t(d.ib_obj) << 32) | d.vb_obj, index);
  } else if (g_frame_dynitems[index].char_family != 0 &&
             g_frame_char_refresh.size() < 256) {
    // Character captured at submit-exit: the PS bank there can predate this
    // character's main pass; refresh the lighting rows on its later draws
    // (last successful capture wins).
    const DrawItem& d = g_frame_dynitems[index];
    g_frame_char_refresh.emplace((uint64_t(d.ib_obj) << 32) | d.vb_obj, index);
  }
  return uint32_t(index + 1);
}

uint32_t CaptureClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6,
                          uint32_t r7, uint32_t* out_key) {
  (void)r4;
  (void)r7;
  if (!SceneEnabled() || r6 != 0x80000000u ||
      !REXCVAR_GET(skate3_native_render_scene_quadlists)) {
    return 0;
  }
  // Quad-list draw: stream 0 is a dynamic ping-pong object whose vertex
  // fetch block (+0x18 dword0 = base|flags, +0x20 = size in bytes) points at
  // the CPU-simulated vertices for this frame: stride 24 = {float3 world
  // position (BE), packed normal, float2 uv}, quad-list topology (verified
  // offline from recorded payloads). NOTE: every capture examined so far is
  // a PARTICLE system (disjoint 2-4cm sprites), which is why rendering is
  // gated off by default; see the cvar.
  const uint32_t vb_obj = g_cur_vb.load(std::memory_order_relaxed);
  if (!GuestReadableApprox(base, vb_obj)) {
    return 0;
  }
  const uint32_t addr = REX_LOAD_U32(vb_obj + 0x18) & 0xFFFFFFFC;
  const uint32_t size = REX_LOAD_U32(vb_obj + 0x20);
  constexpr uint32_t kStride = 24;
  if (addr < 0x10000 || size < kStride * 4 || size > (1u << 20) || size % kStride != 0) {
    return 0;
  }
  // The garment's live vertices start at the buffer head; r5 is the live
  // vertex count (verified offline: the zero-fill run starts exactly at r5
  // for every garment). Ring slack past it must not be drawn.
  uint32_t verts = size / kStride;
  if (r5 >= 4 && r5 <= verts) {
    verts = r5;
  }
  const uint32_t quads = verts / 4;
  if (quads == 0) {
    return 0;
  }
  const uint32_t start = 0;

  const uint32_t garment_key = vb_obj ^ (start * 2654435761u);
  DrawItem item{};
  item.mesh = garment_key;
  item.vb_obj = vb_obj;
  item.ib_obj = 0;
  item.vb_addr = addr;
  item.vb_bytes = verts * kStride;
  item.ib_addr = 0;
  item.ib_count = quads * 6;
  item.diffuse_tex = 0;
  item.lightmap_tex = 0;
  item.pos_offset = 0;
  item.pos_fmt = 57;  // float3
  item.uv_offset = 16;
  item.uv_fmt = 38;  // float2
  item.uv2_offset = 0;
  item.uv2_fmt = 0;
  item.bw_offset = 0;
  item.bi_offset = 0;
  item.stride = kStride;
  item.skinned = false;
  item.pending = false;
  item.cloth_quads = true;
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  for (int axis = 0; axis < 3; ++axis) {
    item.bbox_min[axis] = -20000.0f;
    item.bbox_max[axis] = 20000.0f;
  }
  item.draws.push_back({4, 0, 0, quads * 6});

  // Content fingerprint (simulation output changes every frame -> the
  // renderer re-decodes each frame).
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(addr);
  mix(item.vb_bytes);
  for (uint32_t k = 0; k < 16; ++k) {
    const uint32_t off = uint32_t(uint64_t(item.vb_bytes - 8) * k / 15u) & ~7u;
    mix(REX_LOAD_U64(addr + off));
  }
  item.fingerprint = h;

  // Distinct draw ranges within one ring buffer are distinct garments.
  *out_key = garment_key;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  g_frame_dynitems.push_back(std::move(item));
  return uint32_t(g_frame_dynitems.size());
}

namespace {

// Guest shader objects carry their compiled-source debug path at +0x54
// ("D:\P4\xbox2-ww-f\...\spline_darkenPS.updb"); the spline renderer's pixel
// shaders identify the in-world neon guide elements. Cached per object;
// the current pixel/vertex object trackers are checked both ways because the
// hook labels are swapped relative to the real shader types.
uint32_t ClassifySplineShader(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return 0;
  }
  static std::mutex mu;
  static std::unordered_map<uint32_t, uint32_t> cache;
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char path[97] = {};
  uint32_t kind = 0;
  if (GuestTryCopy(path, base + obj + 0x54, 96)) {
    path[96] = '\0';
    if (std::strstr(path, "spline_darken") != nullptr) {
      kind = 1;
    } else if (std::strstr(path, "spline_default") != nullptr) {
      kind = 2;
    }
  }
  cache.emplace(obj, kind);
  return kind;
}

}  // namespace

void OnSetIndices(uint32_t ib_obj) { g_cur_ib.store(ib_obj, std::memory_order_relaxed); }

void OnSetStreamSource(uint32_t stream, uint32_t vb_obj, uint32_t offset, uint32_t stride) {
  if (stream == 0) {
    g_cur_vb.store(vb_obj, std::memory_order_relaxed);
    g_cur_vb_offset.store(offset, std::memory_order_relaxed);
    g_cur_vb_stride.store(stride, std::memory_order_relaxed);
  }
  if (stream < 4) {
    g_cur_streams[stream][0].store(vb_obj, std::memory_order_relaxed);
    g_cur_streams[stream][1].store(offset, std::memory_order_relaxed);
    g_cur_streams[stream][2].store(stride, std::memory_order_relaxed);
  }
}

void OnDrawDone(uint8_t* base, uint32_t func, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7) {
  g_draw_seq.fetch_add(1, std::memory_order_relaxed);
  const uint32_t flags2d = Phase2dFlags();
  if (flags2d != 0) {
    g_draws_2d.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t bank = g_vs_bank.load(std::memory_order_relaxed);
  if (bank == 0) {
    return;
  }
  // Fog rows: grab c5/c6 from the first 3D draw whose bank c4 row matches
  // the last built scene's camera (main-pass layout; tolerant of one frame
  // of camera motion). See g_fog_rows. The same camera-keyed draws also
  // carry the frame's CSM shadow constants in the PIXEL bank (c0..c8,
  // pass-global on environment-family draws), captured here with an
  // independent done-flag: the first main-pass draw can be a character/tree
  // whose PS allocates differently (rejected by the sanity gate below).
  if ((!g_fog_frame_done || !g_shadow_frame_done || !g_sky_frame_done ||
       !g_tree_frame_done || !g_proxy_frame_done || !g_dynobj_frame_done) &&
      func == 0 && flags2d == 0 && SceneEnabled() &&
      (g_fog_cam[0] != 0.0f || g_fog_cam[1] != 0.0f || g_fog_cam[2] != 0.0f)) {
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    // The sky draw's bank: c4.xz == camera, c4.y = the fixed level sky
    // elevation (dy ~ +160). dy > 50 excludes normal draws (dy == 0) and
    // any reflection pass (y mirrored DOWN); the bound keeps out garbage.
    if (!g_sky_frame_done && dx * dx + dz * dz < 25.0f && dy > 50.0f && dy < 2000.0f) {
      g_sky_height = LoadGuestF32(base, bank + 17 * 4);
      g_sky_have = true;
      g_sky_frame_done = true;
    }
    if (dx * dx + dy * dy + dz * dz < 25.0f) {
      if (!g_fog_frame_done) {
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        // Sanity-gate before trusting the layout: ramp scale is a tiny
        // per-meter slope, the exponent is a small power, the fog color is a
        // dim linear-space rgb and the transmittance scale a small factor.
        const bool sane = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                          rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                          rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                          rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (sane) {
          std::memcpy(g_fog_rows, rows, sizeof(rows));
          g_fog_have = true;
          g_fog_frame_done = true;
        }
      }
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      // POSITIVE family check for the receiver-row capture, by shader debug
      // path: the value gates below cannot fully discriminate;
      // flowingwater_defaultPS keeps the ENTIRE baseenvironment receiver
      // layout (CSM rows, sun c6, camera c7, dim c8) but its material
      // multiplier c11.y is 0.2, not 1.0. When the canal was in view its
      // draw won the first-pass race and the whole world tone chain ran at
      // x0.2 linear, the "world goes dark at some camera rotations" bug
      // (native exactly 0.50x emulated, uniform). Only the environment
      // families that share the c10.x/c11.y layout are eligible.
      const auto env_receiver_ps = [&]() -> bool {
        const auto check = [&](uint32_t obj) -> int {  // 0 unknown, 1 no, 2 yes
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return 0;
          }
          static std::unordered_map<uint32_t, int> cache;
          auto it = cache.find(obj);
          if (it != cache.end()) {
            return it->second;
          }
          char text[120] = {};
          for (int k = 0; k < 119; ++k) {
            text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
            if (text[k] == '\0') break;
          }
          const bool hit = std::strstr(text, "\\baseenvironment") != nullptr ||
                           std::strstr(text, "\\defaultenvironment") != nullptr ||
                           std::strstr(text, "\\decalenvironment") != nullptr;
          // Empty/garbled paths stay unknown (0) and are not cached-in as
          // negatives forever.
          const int result = text[0] == '\0' ? 0 : (hit ? 2 : 1);
          if (result != 0 && cache.size() < 4096) {
            cache.emplace(obj, result);
          }
          return result;
        };
        const int a = check(g_cur_ps_obj.load(std::memory_order_relaxed));
        if (a != 0) {
          return a == 2;
        }
        return check(g_cur_vs_obj.load(std::memory_order_relaxed)) == 2;
      };
      // Not gated on the shadows cvar: the captured rows also carry the
      // scene exposure / material multiplier / sun direction consumed by the
      // exact world shading (rows 40/45/24..26); shading must not die when
      // dynamic shadows are toggled off.
      if (!g_shadow_frame_done && ps_bank != 0 && env_receiver_ps()) {
        float rows[48];
        for (int i = 0; i < 48; ++i) {
          rows[i] = LoadGuestF32(base, ps_bank + i * 4);
        }
        // Receiver-layout sanity:
        // c0/c3 are the light-space X/Y rows with equal magnitude (the
        // cascade-0 extent), c1/c2 the square cascade scale+offsets with
        // scale2 < scale1 < 1, c4 the depth row (a pure height ramp: x/z
        // components tiny), c8 a dim shadow color in [0,1]. Environment
        // family only; other families fail these checks.
        const float mx2 = rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2];
        const float my2 = rows[12] * rows[12] + rows[13] * rows[13] + rows[14] * rows[14];
        const float s1 = rows[4], s2 = rows[8];
        bool sane =
            mx2 > 0.01f && mx2 < 4.0f && std::fabs(mx2 - my2) < 0.05f * mx2 &&
            s1 > 0.0f && s1 < 1.0f && std::fabs(rows[5] - s1) < 1e-4f &&
            s2 > 0.0f && s2 < s1 && std::fabs(rows[9] - s2) < 1e-4f &&
            std::fabs(rows[16]) < 0.02f && std::fabs(rows[18]) < 0.02f &&
            std::fabs(rows[17]) > 0.005f && std::fabs(rows[17]) < 1.0f &&
            rows[32] >= 0.0f && rows[32] <= 1.0f && rows[33] >= 0.0f &&
            rows[33] <= 1.0f && rows[34] >= 0.0f && rows[34] <= 1.0f;
        // c5..c8 are NOT pass-global like c0..c4: only the baseenvironment /
        // defaultenvironment-style bank keeps (bias, ..., sun dir, camera,
        // shadow color) there. Verified in capture: the
        // dynamicobject/livingworld/cacstamp banks pass the geometric checks
        // above with garbage in those rows (c8 ~ (0.0025,0.005,0.012) -> a
        // near-black shadow, c5.x = 0.0245 -> a 29 cm receiver height bias
        // that culled the feet/board shadow); advertisement keeps the color
        // at c7 (c8 = 0 -> pure black); wateralpha has c8 ~ (0.5,0.99,0.5)
        // (-> no shadow at all). Discriminate on the family-specific rows:
        // c6 = unit sun direction with normalize(cross(c0,c3)) == -c6 (the
        // oblique projection axis), c7 = the camera position, c8 dim.
        if (sane) {
          const float cxx = rows[1] * rows[14] - rows[2] * rows[13];
          const float cxy = rows[2] * rows[12] - rows[0] * rows[14];
          const float cxz = rows[0] * rows[13] - rows[1] * rows[12];
          const float cn = std::sqrt(cxx * cxx + cxy * cxy + cxz * cxz);
          const float sn = std::sqrt(rows[24] * rows[24] + rows[25] * rows[25] +
                                     rows[26] * rows[26]);
          const float align =
              cn > 1e-6f && sn > 1e-6f
                  ? (cxx * rows[24] + cxy * rows[25] + cxz * rows[26]) / (cn * sn)
                  : 0.0f;
          const float dcx = rows[28] - g_fog_cam[0];
          const float dcy = rows[29] - g_fog_cam[1];
          const float dcz = rows[30] - g_fog_cam[2];
          sane = align < -0.9f && sn > 0.9f && sn < 1.1f &&
                 dcx * dcx + dcy * dcy + dcz * dcz < 25.0f && rows[32] <= 0.4f &&
                 rows[33] <= 0.4f && rows[34] <= 0.4f &&
                 // c10.x = scene exposure, c11.y = material multiplier,
                 // consumed by the exact world tone chain.
                 rows[40] > 0.1f && rows[40] < 16.0f && rows[45] > 0.0f &&
                 rows[45] < 16.0f;
        }
        if (sane) {
          std::memcpy(g_shadow_rows, rows, sizeof(rows));
          g_shadow_have = true;
          g_shadow_frame_done = true;
        }
      }
      // tree / proxyworld frame rows (see FrameScene::family_rows): their PS
      // banks keep family-global lighting constants in different registers
      // than the environment layout. Identified by the shader debug path
      // (cached, like the blur trigger); the camera-keyed VS c4 gate above
      // already filtered to main-pass draws.
      if ((!g_tree_frame_done || !g_proxy_frame_done || !g_dynobj_frame_done) &&
          ps_bank != 0) {
        const auto world_family = [&](uint32_t obj) -> int {
          if (obj < 0x10000 || !GuestReadableApprox(base, obj)) {
            return 0;
          }
          static std::unordered_map<uint32_t, int> cache;
          auto it = cache.find(obj);
          if (it != cache.end()) {
            return it->second;
          }
          char text[96] = {};
          for (int k = 0; k < 95; ++k) {
            text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
            if (text[k] == '\0') break;
          }
          int fam = 0;
          if (std::strstr(text, "\\tree_defaultPS") != nullptr ||
              std::strstr(text, "\\treeanimate_defaultPS") != nullptr) {
            fam = 1;
          } else if (std::strstr(text, "\\proxyworld_defaultPS") != nullptr) {
            fam = 2;
          } else if (std::strstr(text, "\\dynamicobject_defaultPS") != nullptr ||
                     std::strstr(text, "\\alphatestdynamicobject_defaultPS") !=
                         nullptr) {
            fam = 3;
          }
          if (cache.size() < 4096) {
            cache.emplace(obj, fam);
          }
          return fam;
        };
        int fam = world_family(g_cur_ps_obj.load(std::memory_order_relaxed));
        if (fam == 0) {
          fam = world_family(g_cur_vs_obj.load(std::memory_order_relaxed));
        }
        if (fam == 3 && !g_dynobj_frame_done) {
          // Sun dir c9, exposure c13.x, ambient c15.xyz + bounce c15.w,
          // material multiplier c14.y, static world-shadow floor c8.w.
          float sun[3];
          float n2 = 0.0f;
          for (int k = 0; k < 3; ++k) {
            sun[k] = LoadGuestF32(base, ps_bank + (9 * 4 + k) * 4);
            n2 += sun[k] * sun[k];
          }
          const float expo = LoadGuestF32(base, ps_bank + (13 * 4 + 0) * 4);
          const float mult = LoadGuestF32(base, ps_bank + (14 * 4 + 1) * 4);
          if (n2 > 0.9f && n2 < 1.1f && expo > 0.1f && expo < 16.0f &&
              mult > 0.0f && mult < 16.0f) {
            g_dynobj_rows[0] = sun[0];
            g_dynobj_rows[1] = sun[1];
            g_dynobj_rows[2] = sun[2];
            g_dynobj_rows[3] = expo;
            for (int k = 0; k < 3; ++k) {
              g_dynobj_rows[4 + k] = LoadGuestF32(base, ps_bank + (15 * 4 + k) * 4);
            }
            g_dynobj_rows[7] = LoadGuestF32(base, ps_bank + (15 * 4 + 3) * 4);
            g_dynobj_rows[8] = mult;
            g_dynobj_rows[9] = LoadGuestF32(base, ps_bank + (8 * 4 + 3) * 4);
            if (!g_dynobj_have) {
              REXLOG_INFO(
                  "native-scene: dynamicobject rows captured sun=({:.3f},{:.3f},"
                  "{:.3f}) expo={:.2f} amb=({:.3f},{:.3f},{:.3f}) bounce={:.3f} "
                  "mult={:.2f} floor={:.3f}",
                  g_dynobj_rows[0], g_dynobj_rows[1], g_dynobj_rows[2],
                  g_dynobj_rows[3], g_dynobj_rows[4], g_dynobj_rows[5],
                  g_dynobj_rows[6], g_dynobj_rows[7], g_dynobj_rows[8],
                  g_dynobj_rows[9]);
            }
            g_dynobj_have = true;
            g_dynobj_frame_done = true;
          }
        } else if (fam == 1 && !g_tree_frame_done) {
          const float c0x = LoadGuestF32(base, ps_bank + 0 * 4);
          const float c0y = LoadGuestF32(base, ps_bank + 1 * 4);
          const float c4y = LoadGuestF32(base, ps_bank + 17 * 4);
          if (c0x > 0.0f && c0x < 4.0f && c0y >= 0.0f && c0y < 1.0f &&
              c4y > 0.0f && c4y < 8.0f) {
            g_family_rows[0] = c0x;
            g_family_rows[1] = c0y;
            g_family_rows[2] = c4y;
            g_tree_frame_done = true;
          }
        } else if (fam == 2 && !g_proxy_frame_done) {
          const float c3y = LoadGuestF32(base, ps_bank + 13 * 4);
          if (c3y > 0.0f && c3y < 4.0f) {
            g_family_rows[3] = c3y;
            g_proxy_frame_done = true;
          }
        }
      }
    }
  }
  // UI background blur: while a frontend popup is up, the game appends a
  // dedicated pass chain after the postfx uber: blur_hBlurPS + blur_vBlurPS
  // (func-2 inline quads OUTSIDE the 2D phase) then a postfx_basictex
  // fullscreen replace. The blur_hBlurPS draw itself is the trigger; its
  // PS c0.x is the kernel scale (8 in every capture). See kBlurShaderSource.
  if (func == 2 && flags2d == 0 && SceneEnabled()) {
    const auto is_hblur = [&](uint32_t obj) -> bool {
      if (obj == 0 || !GuestReadableApprox(base, obj)) {
        return false;
      }
      // Guest-render-thread only (like the fog capture globals).
      static std::unordered_map<uint32_t, bool> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      // Debug path at +0x54, e.g. ".../blur_hBlurPS.updb".
      char text[96] = {};
      for (int k = 0; k < 95; ++k) {
        text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
        if (text[k] == '\0') break;
      }
      const bool hit = std::strstr(text, "blur_hBlurPS") != nullptr;
      if (cache.size() < 1024) {
        cache.emplace(obj, hit);
      }
      return hit;
    };
    // Shader labels can be swapped in the hook; accept either slot.
    if (is_hblur(g_cur_ps_obj.load(std::memory_order_relaxed)) ||
        is_hblur(g_cur_vs_obj.load(std::memory_order_relaxed))) {
      // Kernel scale pinned to the ucode-verified steady-state constant: the
      // recorded blur draw's PS c0.x is exactly 8 (tap pitch 0.0025). The
      // live bank read at this hook point returned varying/stale values
      // (e.g. 0.8 vs 8 across reads) and feeding it through per frame
      // pulsed the blur radius; the popup backdrop shimmered.
      g_ui_blur = 8.0f;
      g_ui_blur_seen = true;
    }
  }
  // Selected-object outline capture (see g_frame_selected): the sky draw
  // opens the post-sky window; environmentpark/dynamicobject draws inside it
  // are the selection re-draws (the game excludes the selected object from
  // the main pass and stencil-marks it here). The postfx_edgedetectstencil
  // draw refreshes the outline color.
  if (flags2d == 0 && SceneEnabled() &&
      REXCVAR_GET(skate3_native_render_scene_selection_outline)) {
    // Debug-path family, cached per shader object (guest render thread only,
    // like the blur classifier). 1 = sky, 2 = park piece / dynamic object,
    // 3 = postfx_edgedetectstencil.
    const auto family_of = [&](uint32_t obj) -> int {
      if (obj == 0 || !GuestReadableApprox(base, obj)) {
        return 0;
      }
      static std::unordered_map<uint32_t, int> cache;
      auto it = cache.find(obj);
      if (it != cache.end()) {
        return it->second;
      }
      char text[120] = {};
      for (int k = 0; k < 119; ++k) {
        text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
        if (text[k] == '\0') break;
      }
      int fam = 0;
      if (std::strstr(text, "\\sky_") != nullptr) {
        fam = 1;
      } else if (std::strstr(text, "\\environmentpark") != nullptr ||
                 std::strstr(text, "\\dynamicobject") != nullptr) {
        fam = 2;
      } else if (std::strstr(text, "postfx_edgedetectstencil") != nullptr) {
        fam = 3;
      }
      if (cache.size() < 4096) {
        cache.emplace(obj, fam);
      }
      return fam;
    };
    // Shader labels can be swapped in the hook; accept either slot.
    int fam = family_of(g_cur_ps_obj.load(std::memory_order_relaxed));
    if (fam == 0) {
      fam = family_of(g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (func == 0 && fam == 1) {
      g_sky_seen_this_frame = true;
    } else if (func == 0 && fam == 2 && g_sky_seen_this_frame) {
      // World matrix in the VS bank: 3 rotation rows (w == 0) followed by
      // the translation row (w == 1): c11..c14 on the decal variant,
      // c9..c12 on the diffuse variant. Scan for the first such group.
      for (int r = 8; r <= 16; ++r) {
        if (LoadGuestF32(base, bank + (r * 4 + 3) * 4) != 1.0f) continue;
        bool rot = true;
        for (int p = 1; p <= 3 && rot; ++p) {
          rot = LoadGuestF32(base, bank + ((r - p) * 4 + 3) * 4) == 0.0f;
        }
        if (!rot) continue;
        float t[3];
        for (int a = 0; a < 3; ++a) {
          t[a] = LoadGuestF32(base, bank + (r * 4 + a) * 4);
        }
        if (std::fabs(t[0]) < 100000.0f && std::fabs(t[1]) < 100000.0f &&
            std::fabs(t[2]) < 100000.0f) {
          const uint32_t ib = g_cur_ib.load(std::memory_order_relaxed);
          const uint32_t vb = g_cur_vb.load(std::memory_order_relaxed);
          bool merged = false;
          for (SelectedDrawKey& k : g_frame_selected) {
            if (k.ib == ib && k.vb == vb && std::fabs(k.t[0] - t[0]) < 0.05f &&
                std::fabs(k.t[1] - t[1]) < 0.05f && std::fabs(k.t[2] - t[2]) < 0.05f) {
              ++k.count;
              merged = true;
              break;
            }
          }
          if (!merged && g_frame_selected.size() < 64) {
            g_frame_selected.push_back({ib, vb, {t[0], t[1], t[2]}, 1});
          }
        }
        break;
      }
    } else if (fam == 3) {
      // postfx edge-detect: the presence of this draw is what authorizes the
      // native outline this frame (see g_outline_edge_seen); its PS c0 = the
      // outline color as staged (the park-editor blue (0.216, 0.647, 1.0) in
      // every capture).
      g_outline_edge_seen = true;
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        float c[4];
        for (int a = 0; a < 4; ++a) {
          c[a] = LoadGuestF32(base, ps_bank + a * 4);
        }
        if (c[0] >= 0.0f && c[0] <= 8.0f && c[1] >= 0.0f && c[1] <= 8.0f &&
            c[2] >= 0.0f && c[2] <= 8.0f) {
          std::memcpy(g_outline_color, c, sizeof(g_outline_color));
        }
      }
    }
  }
  // Live 2D overlay capture: the HUD renders exclusively through the
  // BeginVertices inline path (func 2). Vertex payloads are read at frame
  // end; everything else (transform constants, texture fetch) is staged now.
  if (flags2d != 0 && SceneEnabled() && REXCVAR_GET(skate3_native_render_scene_2d)) {
    if (func == 2) {
      const uint32_t device = g_device.load(std::memory_order_relaxed);
      if (device != 0 && r7 >= 0x10000 && r5 != 0 && r5 <= 65536 && r6 >= 8 &&
          r6 <= 256 && (r6 & 3) == 0) {
        Draw2d d;
        d.prim = r4;
        d.count = r5;
        d.stride = r6;
        d.addr = r7;
        d.flags = flags2d;
        for (int i = 0; i < 6; ++i) {
          d.fetch[i] = REX_LOAD_U32(device + 0x480 + i * 4);
        }
        for (int i = 0; i < 36; ++i) {
          d.consts[i] = LoadGuestF32(base, bank + i * 4);
        }
        std::lock_guard<std::mutex> lock(g_2d_mutex);
        if (g_frame_2d.size() < 4096) {
          g_frame_2d.push_back(std::move(d));
        } else {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      g_draws_2d_other.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // In-world neon spline capture (see SplineDraw): a DrawVertices strip of
  // 12-byte float3 params on the spline shaders, bound VB object carrying
  // the payload fetch block (+0x18 base | flags, +0x20 size).
  if (func == 1 && flags2d == 0 && SceneEnabled() &&
      REXCVAR_GET(skate3_native_render_scene_splines) && r5 >= 4 && r5 <= 8192) {
    uint32_t kind = ClassifySplineShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (kind == 0) {
      kind = ClassifySplineShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    const uint32_t vb_obj = g_cur_vb.load(std::memory_order_relaxed);
    if (kind != 0 && GuestReadableApprox(base, vb_obj)) {
      const uint32_t addr = REX_LOAD_U32(vb_obj + 0x18) & 0xFFFFFFFCu;
      const uint32_t size = REX_LOAD_U32(vb_obj + 0x20);
      if (addr >= 0x10000 && size >= r5 * 12) {
        SplineDraw s;
        s.pass = kind;
        s.count = r5;
        s.verts.resize(size_t(r5) * 12);
        if (GuestTryCopy(s.verts.data(), base + addr, s.verts.size())) {
          const uint32_t device = g_device.load(std::memory_order_relaxed);
          for (int i = 0; i < 6; ++i) {
            // The gradient texture lives in fetch shadow slot 3 (24 bytes
            // per slot; the spline PS samples tf3).
            s.fetch[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + 3 * 24 + i * 4) : 0;
          }
          for (int i = 0; i < 153 * 4; ++i) {
            s.consts[i] = LoadGuestF32(base, bank + i * 4);
          }
          g_draws_spline.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard<std::mutex> lock(g_2d_mutex);
          if (g_frame_spline.size() < 64) {
            g_frame_spline.push_back(std::move(s));
          }
        }
      }
    }
  }
  const uint32_t cur_ib = g_cur_ib.load(std::memory_order_relaxed);
  const uint32_t cur_vb = g_cur_vb.load(std::memory_order_relaxed);
  if (g_recording.load(std::memory_order_relaxed)) {
    // Sample the full draw stream on a couple of recorded frames out of
    // every 60, spread across the window (ground-truth coverage for the
    // whole recording), and only for frames that are themselves recorded.
    // 2D-phase draws are recorded on EVERY recorded frame; the HUD stream
    // is small and is exactly what the 2D reconstruction needs.
    std::lock_guard<std::mutex> lock(g_record_mutex);
    const bool frame_recorded = (g_frames_seen + 1) % g_record_stride == 0;
    const bool all_draws = REXCVAR_GET(skate3_native_render_snapshot_all_draws);
    const size_t draw_cap = all_draws ? 200000 : 32768;
    if (frame_recorded && (all_draws || flags2d != 0 || (g_record_frame % 60) < 2) &&
        g_recorded_draws.size() < draw_cap) {
      auto rec = std::make_unique<RecordedDraw>();
      rec->func = func;
      rec->ib = cur_ib;
      rec->vb = cur_vb;
      rec->vb_offset = g_cur_vb_offset.load(std::memory_order_relaxed);
      rec->vb_stride = g_cur_vb_stride.load(std::memory_order_relaxed);
      for (int s = 0; s < 4; ++s) {
        for (int k = 0; k < 3; ++k) {
          rec->streams[s][k] = g_cur_streams[s][k].load(std::memory_order_relaxed);
        }
      }
      const uint32_t device = g_device.load(std::memory_order_relaxed);
      for (int i = 0; i < 12; ++i) {
        rec->vfetch[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + i * 4) : 0;
      }
      rec->args[0] = r4;
      rec->args[1] = r5;
      rec->args[2] = r6;
      rec->args[3] = r7;
      for (uint32_t i = 0; i < 1024; ++i) {
        rec->bank[i] = LoadGuestF32(base, bank + i * 4);
      }
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < 256; ++i) {
        rec->ps[i] = ps_bank != 0 ? LoadGuestF32(base, ps_bank + i * 4) : 0.0f;
      }
      rec->frame = g_record_frame;
      rec->flags2d = flags2d;
      rec->ps_obj = g_cur_ps_obj.load(std::memory_order_relaxed);
      rec->vs_obj = g_cur_vs_obj.load(std::memory_order_relaxed);
      for (int i = 0; i < 6; ++i) {
        rec->viewport[i] = g_cur_viewport[i].load(std::memory_order_relaxed);
      }
      for (int i = 0; i < 4; ++i) {
        rec->scissor[i] = g_cur_scissor[i].load(std::memory_order_relaxed);
      }
      const uint32_t rs_bank = g_rs_bank.load(std::memory_order_relaxed);
      for (int i = 0; i < 256; ++i) {
        rec->rstates[i] = rs_bank != 0 ? REX_LOAD_U32(rs_bank + i * 4) : 0;
      }
      for (int i = 0; i < 192; ++i) {
        rec->vfetch_all[i] = device != 0 ? REX_LOAD_U32(device + 0x480 + i * 4) : 0;
      }
      rec->vb_dump = ~0u;
      rec->ib_dump = ~0u;
      if (flags2d != 0) {
        // 2D payload capture. The 2D pass runs on transient dynamic buffers
        // (glyph/shape vertices regenerated per frame); dump them now.
        // One dump per (guest address, frame); recording mode, so the
        // GuestRangeReadable VAD cost is acceptable.
        const auto dump_buffer = [&](uint32_t obj, uint32_t size_off) -> uint32_t {
          if (obj == 0) {
            return ~0u;
          }
          const uint32_t addr = REX_LOAD_U32(obj + 0x18) & 0xFFFFFFFCu;
          if (addr < 0x10000) {
            return ~0u;
          }
          uint32_t bytes = REX_LOAD_U32(obj + size_off);
          if (bytes < 4 || bytes > (8u << 20)) {
            return ~0u;
          }
          const uint64_t key = (uint64_t(g_record_frame) << 32) | addr;
          auto it = g_frame_dump_ids.find(key);
          if (it != g_frame_dump_ids.end()) {
            return it->second;
          }
          if (g_recorded_buffer_bytes + bytes > (512u << 20) ||
              !GuestRangeReadable(base, addr, bytes)) {
            return ~0u;
          }
          RecordedBuffer buf;
          buf.vb_addr = addr;
          buf.ib_addr = 0;
          buf.fingerprint = key;
          buf.vb.resize(bytes);
          std::memcpy(buf.vb.data(), base + addr, bytes);
          const uint32_t dump_id = uint32_t(g_recorded_buffers.size());
          g_recorded_buffer_bytes += bytes;
          g_recorded_buffers.push_back(std::move(buf));
          g_frame_dump_ids.emplace(key, dump_id);
          return dump_id;
        };
        if (func == 0) {
          rec->vb_dump = dump_buffer(cur_vb, 0x20);
          rec->ib_dump = dump_buffer(cur_ib, 0x1C);
        } else if (func == 1) {
          rec->vb_dump = dump_buffer(cur_vb, 0x20);
        } else if (func == 2 && r7 >= 0x10000 && r5 != 0 && r6 != 0 &&
                   uint64_t(r5) * r6 <= (4u << 20)) {
          // Inline-ring vertices (r5 = count, r6 = stride, r7 = the write
          // pointer BeginVertices returned): the CPU writes them after the
          // call; dump at frame end.
          g_pending_inline_dumps.push_back({r7, r5 * r6, g_recorded_draws.size()});
        }
      }
      if (func == 1 && cur_vb != 0 && flags2d == 0) {
        // Non-indexed (cloth) draw: the bound object holds two ping-pong
        // vertex fetch blocks whose ring payloads are recycled long before
        // frame end; dump both now, keyed back to this draw via rec->ib.
        const uint32_t dump_id = uint32_t(g_recorded_buffers.size());
        for (uint32_t block = 0; block < 2; ++block) {
          const uint32_t fetch0 = REX_LOAD_U32(cur_vb + 0x18 + block * 0x30);
          const uint32_t addr = fetch0 & 0xFFFFFFFC;
          if (addr < 0x10000) continue;
          uint32_t bytes = REX_LOAD_U32(cur_vb + 0x20 + block * 0x30);
          if (bytes < 64 || bytes > (256u << 10)) bytes = 128u << 10;
          if (g_recorded_buffer_bytes + bytes > (512u << 20)) break;
          while (bytes >= 4096 && !GuestRangeReadable(base, addr, bytes)) {
            bytes /= 2;
          }
          if (bytes < 4096) continue;
          RecordedBuffer buf;
          buf.vb_addr = addr;
          buf.ib_addr = 0;
          buf.fingerprint = (uint64_t(g_record_frame) << 32) | dump_id | (block << 30);
          buf.vb.resize(bytes);
          std::memcpy(buf.vb.data(), base + addr, bytes);
          g_recorded_buffer_bytes += bytes;
          g_recorded_buffers.push_back(std::move(buf));
        }
        rec->ib = 0x80000000u | dump_id;
      }
      g_recorded_draws.push_back(std::move(rec));
    }
  }
  if (func != 0) {
    return;  // palette fixup below matches indexed draws only
  }
  const uint64_t key = (uint64_t(cur_ib) << 32) | cur_vb;
  std::lock_guard<std::mutex> lock(g_palette_mutex);
  // Record this draw's texture fetch slots 3+4 (lightmap + diffuse on the
  // environment families) for the frame-end streamed-artwork diffuse
  // override (see g_frame_draw_fetch). Last writer wins: the z-prepass
  // draws these buffers first with stale bindings, the main pass overwrites.
  if (flags2d == 0 && SceneEnabled()) {
    const uint32_t device = g_device.load(std::memory_order_relaxed);
    if (device != 0 && g_frame_draw_fetch.size() < 4096) {
      auto& w = g_frame_draw_fetch[key];
      for (int i = 0; i < 12; ++i) {
        w[size_t(i)] = REX_LOAD_U32(device + 0x480 + 3 * 24 + uint32_t(i) * 4);
      }
    }
  }
  // Character-lighting refresh: items already fixed up re-read their PS rows
  // from later draws with the same buffers (commit-on-success, so the
  // main-pass draw's rows win over the stale caster-pass bank). CLONES share
  // (ib,vb) with per-instance constants (livingworld stamp tints, hair
  // colors, SH rows); refreshing every registered item on every matching
  // draw collapsed all clones onto the LAST instance's rows (the teal-vest
  // twins). Pair draw to instance by the bone palette: the draw's VS bank
  // holds the instance's palette, which matches exactly one item's captured
  // bones.
  auto refresh = g_frame_char_refresh.equal_range(key);
  if (refresh.first != refresh.second) {
    const uint32_t pb = BankPaletteBase(base, bank);
    float row0[4] = {};
    if (pb != 0) {
      for (int i = 0; i < 4; ++i) {
        row0[i] = LoadGuestF32(base, bank + (pb * 4 + uint32_t(i)) * 4);
      }
    }
    for (auto it = refresh.first; it != refresh.second; ++it) {
      if (it->second >= g_frame_dynitems.size()) continue;
      DrawItem& d = g_frame_dynitems[it->second];
      if (d.bones.size() >= 4 && pb != 0) {
        bool match = true;
        for (int i = 0; i < 4 && match; ++i) {
          // The refined palette can sit one register past BankPaletteBase
          // (cloth/morph layouts); compare against both candidate rows.
          match = std::fabs(d.bones[size_t(i)] - row0[i]) < 1e-4f;
        }
        if (!match) {
          bool match1 = true;
          for (int i = 0; i < 4 && match1; ++i) {
            match1 = std::fabs(d.bones[size_t(i)] -
                               LoadGuestF32(base, bank + ((pb + 1) * 4 + uint32_t(i)) * 4)) <
                     1e-4f;
          }
          match = match1;
        }
        if (!match) continue;
      }
      CaptureCharLighting(base, d);
    }
  }
  auto range = g_frame_pending_by_buffers.equal_range(key);
  if (range.first == range.second) {
    return;
  }
  // This draw's constants belong to the OLDEST pending item with these
  // buffers (clones share mesh assets; the deferred list draws in submit
  // order, so FIFO one-shot pairing keeps clones' palettes apart). This is
  // the only draw that ever stages a deferred mesh's bones/world.
  auto oldest = range.first;
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second < oldest->second) {
      oldest = it;
    }
  }
  DrawItem& d = g_frame_dynitems[oldest->second];
  if (d.skinned) {
    // Locate the palette for this draw's layout; a bank without plausible
    // bone rows (parameter blocks, camera rows) must not be consumed; that
    // was the source of screen-wide stretched characters.
    const uint32_t palette_base = BankPaletteBase(base, bank);
    if (palette_base == 0) {
      return;
    }
    if (!CaptureSkinnedState(base, bank, palette_base, d)) {
      return;  // stale bank refused: wait for a later draw with these buffers
    }
  } else {
    // Deferred rigid prop: the world matrix is wherever this draw's layout
    // keeps it (pre-pass c4..c7, main-pass c8..c11). Not plausible -> wait
    // for a later draw with these buffers.
    if (!BankRigidWorld(base, bank, d.world)) {
      return;
    }
  }
  d.pending = false;
  if (d.char_family != 0 && g_frame_char_refresh.size() < 256) {
    g_frame_char_refresh.emplace(key, oldest->second);
  }
  g_frame_pending_by_buffers.erase(oldest);
}

// ---- Camera re-timing (skate3_native_render_scene_smooth_camera) ----------
// The guest publishes camera poses on its own sim tick (~170-240 Hz,
// irregular, measured cam[chg/rep] streaks of 10 rendered frames on one
// pose at 400 fps) while the render loop free-runs: several rendered frames
// reuse one pose, then the camera steps; the world "judders / skips to
// catch up" while panning. The native renderer owns the view transform, so
// each rendered frame samples a TIME-CORRECT pose instead: interpolate
// between the last two distinct guest poses at (now - one sim interval).
// A few ms of camera latency, no overshoot; teleports/cuts snap.

// Standard 3x3 <-> quaternion pair (self-consistent on the view's row-vector
// rotation; only round-tripping matters).
void QuatFromView(const float view[16], float q[4]) {
  const float m00 = view[0], m01 = view[1], m02 = view[2];
  const float m10 = view[4], m11 = view[5], m12 = view[6];
  const float m20 = view[8], m21 = view[9], m22 = view[10];
  const float tr = m00 + m11 + m22;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q[3] = 0.25f * s;
    q[0] = (m21 - m12) / s;
    q[1] = (m02 - m20) / s;
    q[2] = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q[3] = (m21 - m12) / s;
    q[0] = 0.25f * s;
    q[1] = (m01 + m10) / s;
    q[2] = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q[3] = (m02 - m20) / s;
    q[0] = (m01 + m10) / s;
    q[1] = 0.25f * s;
    q[2] = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q[3] = (m10 - m01) / s;
    q[0] = (m02 + m20) / s;
    q[1] = (m12 + m21) / s;
    q[2] = 0.25f * s;
  }
}

void ViewRotFromQuat(const float q[4], float view[16]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  view[0] = 1 - 2 * (y * y + z * z);
  view[1] = 2 * (x * y - z * w);
  view[2] = 2 * (x * z + y * w);
  view[4] = 2 * (x * y + z * w);
  view[5] = 1 - 2 * (x * x + z * z);
  view[6] = 2 * (y * z - x * w);
  view[8] = 2 * (x * z - y * w);
  view[9] = 2 * (y * z + x * w);
  view[10] = 1 - 2 * (x * x + y * y);
}

void QuatSlerp(const float a[4], const float b_in[4], float t, float out[4]) {
  float b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (dot < 0.0f) {
    for (float& v : b) v = -v;
    dot = -dot;
  }
  float wa = 1.0f - t, wb = t;
  if (dot < 0.9995f) {
    const float theta = std::acos(std::min(dot, 1.0f));
    const float s = std::sin(theta);
    wa = std::sin((1.0f - t) * theta) / s;
    wb = std::sin(t * theta) / s;
  }
  float n2 = 0.0f;
  for (int i = 0; i < 4; ++i) {
    out[i] = wa * a[i] + wb * b[i];
    n2 += out[i] * out[i];
  }
  const float inv = n2 > 1e-12f ? 1.0f / std::sqrt(n2) : 1.0f;
  for (int i = 0; i < 4; ++i) out[i] *= inv;
}

struct CamPose {
  double t = 0.0;
  float q[4] = {0, 0, 0, 1};  // view rotation (rows 0..2)
  float c[3] = {};            // camera world position
};

// Shared playback clock (guest render thread): set by SmoothCamera each
// frame it produces a re-timed pose; dynamic items (bone palettes, rigid
// worlds) interpolate at the SAME time so the skater/NPCs/props stay in
// phase with the smoothed camera; smoothing only the camera concentrated
// all the sim stepping on the skater ("he definitively judders now").
double g_smooth_play = 0.0;
bool g_smooth_active = false;
// Last pose SmoothCamera produced (guest render thread), read by the
// synthetic-pan probe (mode 3) to measure reconstruction error against the
// known ideal pose.
CamPose g_smooth_pose;

// ---- Synthetic camera pan (judder isolation probe) -------------------------
// See the skate3_native_render_scene_synthetic_pan cvar comment for the mode
// semantics. Engage state is captured from the RAW guest camera on the guest
// render thread; the sampler thread reads it for mode 3 under g_synpan_mutex.
std::atomic<int> g_synpan_active{0};  // engaged mode (0 = off)
std::mutex g_synpan_mutex;
float g_synpan_view0[16];  // raw guest view at engage
float g_synpan_proj0[16];
float g_synpan_c0[3];   // camera world position at engage (held fixed)
double g_synpan_t0 = 0.0;
double g_synpan_step_phase = 0.0;  // mode 2: accumulated phase (degrees)
double g_synpan_ema_dt = 0.0;      // mode 2: slow EMA of the publish dt
// Telemetry (guest render thread only).
uint64_t g_synpan_frames = 0;
double g_synpan_last_build = 0.0;
double g_synpan_dt_sum = 0.0, g_synpan_dt_sum2 = 0.0;
double g_synpan_dt_min = 0.0, g_synpan_dt_max = 0.0;
double g_synpan_err_sum2 = 0.0, g_synpan_err_max = 0.0;
uint64_t g_synpan_err_n = 0;

// Full-surround world union for the probe: the game only submits what ITS
// frustum sees each frame, so a synthetic pan away from the real heading
// would show an empty world (sky dome only). While the probe is engaged,
// every static item published is accumulated by identity and appended to
// each frame's scene; sweep the REAL camera around once with the stick
// after engaging and the union fills in the whole surround, making a full
// 360-degree spin (amplitude 0) usable. Guest render thread only.
std::unordered_map<uint64_t, DrawItem> g_synpan_union;

uint64_t SynPanItemKey(const DrawItem& it) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(it.mesh);
  mix(it.vb_obj);
  mix(it.ib_obj);
  mix(std::bit_cast<uint32_t>(it.world[12]));
  mix(std::bit_cast<uint32_t>(it.world[13]));
  mix(std::bit_cast<uint32_t>(it.world[14]));
  return h;
}

// Pan phase (degrees of accumulated rotation) -> heading angle in degrees.
// Triangle wave in [-amp, +amp] starting at 0 heading upward; amp <= 0 =
// unbounded continuous rotation.
double SynPanAngleDeg(double phase_deg, double amp) {
  if (amp <= 0.0) {
    return phase_deg;
  }
  const double period = 4.0 * amp;
  double m = std::fmod(phase_deg, period);
  if (m < 0.0) {
    m += period;
  }
  return m < amp ? m : (m < 3.0 * amp ? 2.0 * amp - m : m - period);
}

// View matrix for the engage pose yawed by `angle_deg` about world up (+Y),
// camera position held at the engage position. Row-vector convention
// throughout (p_view = p * V): the 3x3 becomes Ry * R0 (world rotates first,
// then the engage view, a level pan; direction sign is irrelevant for the
// probe), translation row rebuilt as -c0 * R'.
void SynPanView(double angle_deg, float view_out[16]) {
  const double a = angle_deg * (3.14159265358979323846 / 180.0);
  const float c = float(std::cos(a)), s = float(std::sin(a));
  const float ry[3][3] = {{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}};
  std::memset(view_out, 0, 16 * sizeof(float));
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      float sum = 0.0f;
      for (int k = 0; k < 3; ++k) {
        sum += ry[i][k] * g_synpan_view0[k * 4 + j];
      }
      view_out[i * 4 + j] = sum;
    }
  }
  for (int k = 0; k < 3; ++k) {
    view_out[12 + k] = -(g_synpan_c0[0] * view_out[0 * 4 + k] +
                         g_synpan_c0[1] * view_out[1 * 4 + k] +
                         g_synpan_c0[2] * view_out[2 * 4 + k]);
  }
  view_out[15] = 1.0f;
}

// ---- High-rate camera sampler ---------------------------------------------
// Pose samples taken once per RENDERED frame alias as soon as the render
// rate drops near/below the sim's camera tick (~200 Hz): at the 140 fps
// pacing cap some frames contain one sim step of rotation and some two, all
// timestamped on the even frame grid; the smoother then faithfully renders
// a 140-vs-200 Hz beat as CONSTANT judder. This thread samples the guest
// ViewCamera at ~1 kHz with precise host timestamps, so the reconstruction
// signal is always finer than the sim tick regardless of render cadence.
// Torn reads (the sim thread writes the matrix concurrently) are rejected
// by a double-read compare.
struct RawCamSample {
  double t;
  float view[16];
  float proj[16];
};
std::atomic<uint32_t> g_sampler_viewcam{0};  // published by BuildFrameScene
std::mutex g_cam_samples_mutex;
std::vector<RawCamSample> g_cam_samples;  // pending; drained by SmoothCamera
std::atomic<bool> g_cam_sampler_started{false};
std::atomic<uint64_t> g_cam_sampler_pushes{0};  // telemetry

void CamSamplerLoop() {
  float last_view[16] = {};
  double syn_next_emit = 0.0;
  int syn_cadence_i = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!SceneEnabled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    // Synthetic pan mode 3: synthesize guest-like pose samples instead of
    // reading the guest camera, ~200 Hz with a deliberately irregular
    // cadence (the real sim tick is irregular; the smoother must cope). The
    // downstream path (ring, period EMA, playback clock, slerp) runs
    // unmodified on them, and its output is compared numerically against
    // the ideal pose in BuildFrameScene.
    if (g_synpan_active.load(std::memory_order_acquire) == 3) {
      static constexpr double kCadenceMs[6] = {4.0, 5.0, 6.0, 4.0, 6.0, 5.0};
      const double now =
          std::chrono::duration<double>(PerfClock::now().time_since_epoch()).count();
      if (syn_next_emit == 0.0) {
        syn_next_emit = now;
      }
      if (now < syn_next_emit) {
        continue;
      }
      syn_next_emit += kCadenceMs[syn_cadence_i++ % 6] * 1e-3;
      if (syn_next_emit < now) {
        syn_next_emit = now;  // fell behind (OS scheduling hitch): re-anchor
      }
      RawCamSample s;
      s.t = now;
      {
        std::lock_guard<std::mutex> lock(g_synpan_mutex);
        const double rate = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate);
        const double amp = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp);
        SynPanView(SynPanAngleDeg((now - g_synpan_t0) * rate, amp), s.view);
        std::memcpy(s.proj, g_synpan_proj0, sizeof(s.proj));
      }
      std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
      if (g_cam_samples.size() < 256) {
        g_cam_samples.push_back(s);
        g_cam_sampler_pushes.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    syn_next_emit = 0.0;
    const uint32_t vc = g_sampler_viewcam.load(std::memory_order_relaxed);
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (vc == 0 || base == nullptr) {
      continue;
    }
    // view at +0x20, proj at +0x60: one 128-byte guarded copy covers both.
    uint32_t raw_a[32], raw_b[32];
    if (!GuestTryCopy(raw_a, base + vc + 0x20, sizeof(raw_a)) ||
        !GuestTryCopy(raw_b, base + vc + 0x20, sizeof(raw_b)) ||
        std::memcmp(raw_a, raw_b, sizeof(raw_a)) != 0) {
      continue;  // unreadable or torn mid-write: try again next tick
    }
    const double now = std::chrono::duration<double>(
                           PerfClock::now().time_since_epoch())
                           .count();
    RawCamSample s;
    s.t = now;
    for (int i = 0; i < 16; ++i) {
      s.view[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[i]));
      s.proj[i] = std::bit_cast<float>(__builtin_bswap32(raw_a[16 + i]));
    }
    // Sanity: perspective proj + a plausible rotation row (stale viewcam
    // addresses after a map change read garbage until re-published).
    const float r0n = s.view[0] * s.view[0] + s.view[1] * s.view[1] +
                      s.view[2] * s.view[2];
    if (s.proj[2 * 4 + 3] != 1.0f || r0n < 0.9f || r0n > 1.1f) {
      continue;
    }
    if (std::memcmp(last_view, s.view, sizeof(last_view)) == 0) {
      continue;  // pose unchanged
    }
    std::memcpy(last_view, s.view, sizeof(last_view));
    // Camera-signal recorder: every DISTINCT pose the guest produced, with
    // the sampler's precise host timestamp (kind 0).
    if (now < g_camsig_deadline.load(std::memory_order_relaxed)) {
      std::lock_guard<std::mutex> lock(g_camsig_mutex);
      if (g_camsig.size() < 200000) {
        g_camsig.push_back({now, 0.0, YawFromViewRows(s.view), 0});
      }
    }
    std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
    if (g_cam_samples.size() < 256) {
      g_cam_samples.push_back(s);
      g_cam_sampler_pushes.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void EnsureCamSampler() {
  if (g_cam_sampler_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::thread(CamSamplerLoop).detach();
  REXLOG_INFO("native-scene: camera sampler thread started (~1 kHz)");
}

// Rebuild a row-vector view matrix from pose (rows 0..2 = R, row 3 = -c*R)
// and produce view_proj = view x proj.
void ComposeViewProj(const CamPose& p, const float proj[16], float vp_out[16],
                     float cam_out[3]) {
  float v[16] = {};
  ViewRotFromQuat(p.q, v);
  v[3] = v[7] = v[11] = 0.0f;
  for (int k = 0; k < 3; ++k) {
    v[12 + k] = -(p.c[0] * v[0 * 4 + k] + p.c[1] * v[1 * 4 + k] + p.c[2] * v[2 * 4 + k]);
  }
  v[15] = 1.0f;
  for (int r = 0; r < 4; ++r) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += v[r * 4 + k] * proj[k * 4 + col];
      }
      vp_out[r * 4 + col] = sum;
    }
  }
  std::memcpy(cam_out, p.c, 3 * sizeof(float));
}

// Guest render thread only. Returns true when vp_out/cam_out hold a
// re-timed pose for `now`; false = keep the raw guest pose (no history yet,
// or a cut/teleport snapped).
//
// Dejitter structure (the naive last-two-samples lerp still juddered):
// sample DETECTION times are quantized to render frames (+-2.5 ms of noise
// on a ~4.5 ms camera tick), and re-basing the timeline on each new sample
// jerks the output whenever sample spacing varies. Instead: a pose RING
// timestamped at detection, an EMA of the sample period, and a playback
// clock that advances on real frame time and only slews gently toward
// (newest - 2 periods). The bracketing ring samples are slerped at the
// playback time; sample-time noise is absorbed by the slew instead of
// feeding straight into the pose velocity.
// Most recent camera sim-tick time (guest render thread only): entities that
// changed pose this frame changed on the same sim tick; timestamping their
// pose rings with it instead of the frame time avoids the same aliasing.
double g_latest_cam_tick = 0.0;

bool SmoothCamera(const float view[16], const float proj[16], const float raw_vp[16],
                  const float raw_cam[3], double now, float vp_out[16],
                  float cam_out[3]) {
  constexpr int kRing = 64;  // ~1 kHz sampler: cover well past the lag window
  static CamPose s_ring[kRing];
  static int s_count = 0;  // -1 = convention drift, never smooth this run
  static int s_newest = 0;
  static float s_last_view[16] = {};
  static double s_period = 1.0 / 200.0;  // EMA of the guest camera tick
  static double s_play = 0.0;            // playback clock (guest-pose time axis)
  static double s_last_now = 0.0;
  static bool s_play_valid = false;

  const auto push_pose = [&](const CamPose& p) {
    if (s_count < 0) {
      return;
    }
    const CamPose& latest = s_ring[s_newest];
    const float dot = std::fabs(p.q[0] * latest.q[0] + p.q[1] * latest.q[1] +
                                p.q[2] * latest.q[2] + p.q[3] * latest.q[3]);
    const float dx = p.c[0] - latest.c[0], dy = p.c[1] - latest.c[1],
                dz = p.c[2] - latest.c[2];
    const float dist2 = dx * dx + dy * dy + dz * dz;
    if (s_count == 0 || p.t - latest.t > 0.1 || dist2 > 25.0f || dot < 0.9f) {
      // First sample / stale history / teleport / hard cut: snap.
      s_ring[0] = p;
      s_newest = 0;
      s_count = 1;
      s_play_valid = false;
    } else {
      const double dt = p.t - latest.t;
      if (dt <= 0.0) {
        return;  // out-of-order/duplicate
      }
      if (dt > 0.0005 && dt < 0.05) {
        s_period = s_period * 0.9 + dt * 0.1;
      }
      s_newest = (s_newest + 1) % kRing;
      s_ring[s_newest] = p;
      s_count = std::min(s_count + 1, kRing);
    }
    g_latest_cam_tick = p.t;
  };

  // Primary source: the ~1 kHz sampler thread (precise timestamps: pose
  // samples taken per RENDERED frame alias against the ~200 Hz sim tick as
  // soon as the render rate is paced down; see CamSamplerLoop).
  {
    static std::vector<RawCamSample> s_pending;
    {
      std::lock_guard<std::mutex> lock(g_cam_samples_mutex);
      s_pending.swap(g_cam_samples);
    }
    for (const RawCamSample& s : s_pending) {
      CamPose p;
      p.t = s.t;
      QuatFromView(s.view, p.q);
      for (int j = 0; j < 3; ++j) {
        p.c[j] = -(s.view[12] * s.view[j * 4 + 0] + s.view[13] * s.view[j * 4 + 1] +
                   s.view[14] * s.view[j * 4 + 2]);
      }
      push_pose(p);
    }
    s_pending.clear();
  }

  if (std::memcmp(s_last_view, view, sizeof(s_last_view)) != 0) {
    std::memcpy(s_last_view, view, sizeof(s_last_view));
    CamPose p;
    p.t = now;
    QuatFromView(view, p.q);
    std::memcpy(p.c, raw_cam, sizeof(p.c));
    // One-shot reconstruction self-check: the rebuilt view_proj at this
    // exact pose must match the guest's own (catches any convention drift).
    static bool s_checked = false;
    if (!s_checked) {
      s_checked = true;
      float test_vp[16], test_cam[3];
      ComposeViewProj(p, proj, test_vp, test_cam);
      float max_rel = 0.0f;
      for (int i = 0; i < 16; ++i) {
        const float mag = std::max(1.0f, std::fabs(raw_vp[i]));
        max_rel = std::max(max_rel, std::fabs(test_vp[i] - raw_vp[i]) / mag);
      }
      REXLOG_INFO("native-scene: smooth-camera reconstruction check max_rel={:.6f}{}",
                  max_rel, max_rel > 0.01f ? " (BAD - smoothing disabled this run)" : "");
      if (max_rel > 0.01f) {
        s_count = -1;
      }
    }
    // Fallback only: if the sampler has fed the ring recently, the frame-
    // grid pose is a stale duplicate of a sampler pose; pushing it would
    // corrupt the timestamps.
    if (s_count >= 0 && (s_count == 0 || now - s_ring[s_newest].t > 0.05)) {
      push_pose(p);
    }
  }
  if (s_count < 3 || now - s_ring[s_newest].t > 0.1) {
    s_play_valid = false;
    g_smooth_active = false;
    return false;
  }

  // Playback clock: advance by real frame time; slew gently toward the
  // target lag point so detection-time noise never becomes pose velocity.
  const double frame_dt =
      s_play_valid ? std::clamp(now - s_last_now, 0.0, 0.05) : 0.0;
  s_last_now = now;
  // Boxcar pose filter (see below): the playback point must lag far enough
  // that the CENTERED window has ring samples on both sides.
  const double filter_w = std::clamp(
      REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) *
      1e-3;
  const double lag = std::max(2.0 * s_period, filter_w * 0.5 + s_period);
  const double target = s_ring[s_newest].t - lag;
  if (!s_play_valid) {
    s_play = target;
    s_play_valid = true;
  } else {
    s_play += frame_dt;
    const double err = target - s_play;
    if (err > 0.1 || err < -0.1) {
      s_play = target;  // fell far behind/ahead (hitch): snap the clock
    } else {
      s_play += err * 0.06;
    }
  }
  // Never play past the newest sample: when panning STOPS, samples stop
  // arriving while the clock keeps advancing; extrapolating overshot the
  // stop pose and the staleness cutoff then snapped back ("catch-up judder
  // when I stop panning"). Parked at the newest sample, the smoothed pose
  // settles exactly onto the raw stop pose and every later handoff is
  // seamless.
  s_play = std::min(s_play, s_ring[s_newest].t);

  // Evaluate the piecewise-linear pose signal at an arbitrary time by
  // bracketing in the ring (samples are time-ordered oldest -> newest) and
  // slerping. Clamps to the ring's ends.
  const auto eval_ring = [&](double t_eval) {
    int hi = s_newest;
    int lo = (s_newest + kRing - 1) % kRing;
    for (int step = 1; step < s_count - 1; ++step) {
      if (s_ring[lo].t <= t_eval) {
        break;
      }
      hi = lo;
      lo = (lo + kRing - 1) % kRing;
    }
    const CamPose& p0 = s_ring[lo];
    const CamPose& p1 = s_ring[hi];
    const double span = std::max(p1.t - p0.t, 0.0005);
    const double alpha = std::clamp((t_eval - p0.t) / span, 0.0, 1.0);
    CamPose r;
    r.t = t_eval;
    QuatSlerp(p0.q, p1.q, float(alpha), r.q);
    for (int k = 0; k < 3; ++k) {
      r.c[k] = p0.c[k] + (p1.c[k] - p0.c[k]) * float(alpha);
    }
    return r;
  };

  CamPose p;
  if (filter_w > 0.0005 && s_count >= 4) {
    // Boxcar pose filter: average the interpolated signal over a window
    // CENTERED on the playback point (zero phase error at constant
    // velocity). Root-caused need (camera-signal recordings): the
    // game's camera pose VALUES advance in 60 Hz-quantized lumps at high
    // render rates; during a measured constant stick pan the per-tick
    // angular velocity had sd 144 deg/s on a 172 deg/s mean, +-2.2 deg off
    // a constant-rate path. A 50 ms window (three 60 Hz periods) nulls the
    // quantization at any render rate: measured frame-to-frame velocity
    // jitter fell 185 -> 7 deg/s rms on the recorded signal. Taps past the
    // newest sample clamp to it, so a pan STOP still settles exactly onto
    // the raw stop pose (never extrapolates).
    constexpr int kTaps = 16;
    double qacc[4] = {};
    double cacc[3] = {};
    float qref[4] = {};
    for (int k = 0; k < kTaps; ++k) {
      const double tt = std::min(s_play - filter_w * 0.5 + (k + 0.5) * filter_w / kTaps,
                                 s_ring[s_newest].t);
      const CamPose s = eval_ring(tt);
      float sq[4] = {s.q[0], s.q[1], s.q[2], s.q[3]};
      if (k == 0) {
        std::memcpy(qref, sq, sizeof(qref));
      } else if (sq[0] * qref[0] + sq[1] * qref[1] + sq[2] * qref[2] +
                     sq[3] * qref[3] <
                 0.0f) {
        for (float& v : sq) v = -v;
      }
      for (int j = 0; j < 4; ++j) qacc[j] += sq[j];
      for (int j = 0; j < 3; ++j) cacc[j] += s.c[j];
    }
    const double qn = std::sqrt(qacc[0] * qacc[0] + qacc[1] * qacc[1] +
                                qacc[2] * qacc[2] + qacc[3] * qacc[3]);
    p.t = s_play;
    for (int j = 0; j < 4; ++j) {
      p.q[j] = float(qacc[j] / std::max(qn, 1e-12));
    }
    for (int j = 0; j < 3; ++j) {
      p.c[j] = float(cacc[j] / kTaps);
    }
  } else {
    p = eval_ring(s_play);
  }
  ComposeViewProj(p, proj, vp_out, cam_out);
  g_smooth_play = s_play;
  g_smooth_pose = p;
  g_smooth_active = true;
  return true;
}

// Interpolate the DYNAMIC items' per-draw state (bone palettes; rigid
// non-identity world matrices) at the camera's playback time. Without this
// the smoothed camera glides while the skater/NPCs/props snap at the guest
// sim tick; the stepping that used to be hidden (camera and entities
// stepped IN PHASE) becomes visible entity judder. History pairs poses by
// (mesh, k-th occurrence this frame): clones publish in stable submit
// order, so the k-th copy pairs with last frame's k-th copy. Componentwise
// lerp is exact enough at adjacent-sim-tick deltas (~5 ms of motion).
// Guest render thread only.
void InterpolateDynamicItems(FrameScene& scene, double now) {
  // Per-entity pose RING, like the camera's: the playback clock sits ~2 sim
  // periods behind `now`, so a two-pose history never brackets it (the
  // interpolation alpha pinned at 0 and entities rendered STALE STEPPED
  // poses: "the skater still judders and looks blurry"). Sixteen poses
  // cover ~115 ms at 140 fps: the camera filter pushes the play clock
  // ~(W/2 + period) back AND the entity boxcar (below) reaches another W/2
  // past that. The shared g_smooth_play keeps entities in phase with the
  // camera.
  constexpr int kRing = 16;
  struct DynPose {
    double t = 0.0;
    std::vector<float> b;  // bone palette (skinned), raw captured rows
    float w[16] = {};      // world (rigid)
  };
  struct DynHist {
    DynPose ring[kRing];
    int count = 0;
    int newest = 0;
    uint64_t seen = 0;
  };
  static std::unordered_map<uint64_t, DynHist> s_hist;
  static uint64_t s_frame = 0;
  ++s_frame;
  // Bone-signal recorder window state (see BoneSigAppend). Written on a
  // detached thread once the window closes.
  bool bs_rec = false;
  {
    const double req = g_bonesig_request.exchange(0.0, std::memory_order_acq_rel);
    if (req > 0.0) {
      g_bonesig.clear();
      g_bonesig.reserve(16u << 20);
      g_bonesig_deadline.store(now + req, std::memory_order_relaxed);
    }
    const double dl = g_bonesig_deadline.load(std::memory_order_relaxed);
    if (dl > 0.0) {
      if (now < dl) {
        bs_rec = true;
      } else {
        std::vector<uint8_t> blob;
        blob.swap(g_bonesig);
        g_bonesig_deadline.store(0.0, std::memory_order_release);
        std::thread([blob = std::move(blob)]() {
          std::error_code ec;
          std::filesystem::create_directories("logs", ec);
          char path[128];
          std::snprintf(path, sizeof(path), "logs/bone_signal_%lld.bin",
                        static_cast<long long>(std::time(nullptr)));
          std::ofstream f(path, std::ios::binary);
          f.write("BSIG1\n", 6);
          f.write(reinterpret_cast<const char*>(blob.data()),
                  std::streamsize(blob.size()));
          REXLOG_INFO("native-scene bone-signal: wrote {} bytes -> {}", blob.size() + 6,
                      path);
        }).detach();
      }
    }
  }
  static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::unordered_map<uint32_t, uint32_t> occurrence;
  for (DrawItem& item : scene.items) {
    const bool skinned = item.skinned && !item.bones.empty();
    const bool rigid_dyn =
        !skinned && std::memcmp(item.world, kIdent, sizeof(kIdent)) != 0;
    if (!skinned && !rigid_dyn) {
      continue;
    }
    const uint32_t k = occurrence[item.mesh]++;
    const uint64_t key = (uint64_t(item.mesh) << 8) | (k & 0xFF);
    DynHist& h = s_hist[key];
    h.seen = s_frame;
    const DynPose& latest = h.ring[h.newest];
    const bool changed =
        h.count == 0 ||
        (skinned ? latest.b != item.bones
                 : std::memcmp(latest.w, item.world, sizeof(item.world)) != 0);
    if (changed) {
      // Discontinuities reset the history (pose-size change = LOD/garment
      // swap; long gap = the entity was gone). CRUCIALLY also a translation
      // jump no entity makes in one sim tick (> 1.5 m): clones pair by
      // occurrence order, and the game's sort lists RESHUFFLE clones as the
      // camera moves; without this guard a vending machine inherited its
      // twin's history and visibly slid/teleported between the two
      // placements. Mispairs now render the raw pose (a no-op for the
      // static props where it was visible).
      if (h.count > 0) {
        bool discontinuity =
            now - latest.t > 0.1 ||
            (skinned && latest.b.size() != item.bones.size());
        if (!discontinuity) {
          // Translation: bone-0 rows carry t in float 3 of each row;
          // rigid worlds carry it in row 3.
          const float* nt = skinned ? item.bones.data() : &item.world[12];
          const float* ot = skinned ? latest.b.data() : &latest.w[12];
          const float dx = skinned ? nt[3] - ot[3] : nt[0] - ot[0];
          const float dy = skinned ? nt[7] - ot[7] : nt[1] - ot[1];
          const float dz = skinned ? nt[11] - ot[11] : nt[2] - ot[2];
          discontinuity = dx * dx + dy * dy + dz * dz > 2.25f;  // > 1.5 m
        }
        if (discontinuity) {
          h.count = 0;
        }
      }
      h.newest = h.count == 0 ? 0 : (h.newest + 1) % kRing;
      DynPose& p = h.ring[h.newest];
      // Timestamp with the camera sampler's latest sim tick when fresh:
      // this entity's pose changed on the same sim tick, and frame-grid
      // timestamps alias against the sim rate once the render loop is
      // paced (the same problem the camera sampler solves).
      p.t = (g_latest_cam_tick > 0.0 && now - g_latest_cam_tick < 0.02)
                ? g_latest_cam_tick
                : now;
      p.b = item.bones;
      std::memcpy(p.w, item.world, sizeof(p.w));
      h.count = std::min(h.count + 1, kRing);
      if (bs_rec) {
        BoneSigAppend(0, key, p.t, 0.0,
                      skinned ? item.bones.data() : item.world,
                      skinned ? uint32_t(item.bones.size()) : 16u);
      }
    }
    if (h.count < 3 || now - h.ring[h.newest].t > 0.1) {
      continue;  // not enough history yet: raw stepped pose (one-time snap)
    }
    // Evaluate the ring's piecewise-linear pose signal at time tt into
    // `out_b` (skinned, weighted-accumulated) / `out_w` (rigid), scaled by
    // `weight`. Returns false on a palette-size mismatch inside the window
    // (LOD/garment swap); the caller keeps the raw pose that frame.
    const auto accum_at = [&](double tt, float weight, float* out_b,
                              float out_w[16]) {
      int hi = h.newest;
      int lo = (h.newest + kRing - 1) % kRing;
      for (int step = 1; step < h.count - 1; ++step) {
        if (h.ring[lo].t <= tt) {
          break;
        }
        hi = lo;
        lo = (lo + kRing - 1) % kRing;
      }
      const DynPose& p0 = h.ring[lo];
      const DynPose& p1 = h.ring[hi];
      if (skinned && (p0.b.size() != item.bones.size() ||
                      p1.b.size() != item.bones.size())) {
        return false;
      }
      const double span = std::max(p1.t - p0.t, 0.0005);
      // Clamp at 1.0 (no extrapolation): entities that stop moving must
      // settle exactly onto their raw pose, like the camera.
      const float a = float(std::clamp((tt - p0.t) / span, 0.0, 1.0));
      if (skinned) {
        for (size_t i = 0; i < item.bones.size(); ++i) {
          out_b[i] += (p0.b[i] + (p1.b[i] - p0.b[i]) * a) * weight;
        }
      } else {
        for (int i = 0; i < 16; ++i) {
          out_w[i] += (p0.w[i] + (p1.w[i] - p0.w[i]) * a) * weight;
        }
      }
      return true;
    };
    // Boxcar filter, same as the camera's: the guest's ANIMATION poses are
    // 60 Hz-quantized like its camera; once the camera glides, the 60-vs-
    // render-rate pose alternation reads as skater judder/ghosting against
    // the smooth background. Averaging bone affines componentwise over the
    // window slightly shrinks fast-swinging limb rotations (a subtle motion
    // blur), the trade the game's own 60 Hz presentation makes anyway.
    const double filter_w = std::clamp(
        REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms), 0.0, 200.0) *
        1e-3;
    static std::vector<float> acc;  // guest render thread only
    float wacc[16] = {};
    bool ok = true;
    if (filter_w > 0.0005 && h.count >= 4) {
      constexpr int kTaps = 8;
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      for (int tap = 0; tap < kTaps && ok; ++tap) {
        const double tt =
            std::min(g_smooth_play - filter_w * 0.5 + (tap + 0.5) * filter_w / kTaps,
                     h.ring[h.newest].t);
        ok = accum_at(tt, 1.0f / kTaps, acc.data(), wacc);
      }
    } else {
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      ok = accum_at(g_smooth_play, 1.0f, acc.data(), wacc);
    }
    if (!ok) {
      continue;  // palette-size mismatch in the window: raw pose this frame
    }
    // Fast-spinning bones (skateboard wheels: hundreds of degrees inside
    // the window) COLLAPSE under componentwise averaging; the rotation
    // entries cancel toward zero and the wheel shrinks into the truck /
    // sinks into the ground. Guard per bone: if the averaged 3x3's
    // Frobenius norm dropped versus a raw sample's, the window spans too
    // much rotation; fall back to the plain TWO-ADJACENT-SAMPLE lerp at
    // the playback point for that bone (the pre-filter behavior: samples
    // ~7.5 ms apart lerp with only a few % shrink, and rotation +
    // translation come from the SAME pose pair). Failed alternatives, do
    // not revisit: whole-bone nearest-sample snap = wheels lag the filtered
    // board and snap to catch up; nearest ROTATION + averaged TRANSLATION =
    // wheels ORBIT the axle ~10 cm (palette affines are model->world; the
    // rotation pivots about the MODEL origin and the translation carries
    // the axle-pivot compensation; the pair must stay consistent). Slow
    // bones (the judder sources) pass untouched.
    int b_lo = h.newest, b_hi = h.newest;
    {
      int hi2 = h.newest;
      int lo2 = (h.newest + kRing - 1) % kRing;
      for (int step = 1; step < h.count - 1; ++step) {
        if (h.ring[lo2].t <= g_smooth_play) {
          break;
        }
        hi2 = lo2;
        lo2 = (lo2 + kRing - 1) % kRing;
      }
      b_lo = lo2;
      b_hi = hi2;
    }
    const DynPose& q0 = h.ring[b_lo];
    const DynPose& q1 = h.ring[b_hi];
    const double bspan = std::max(q1.t - q0.t, 0.0005);
    const float ba = float(std::clamp((g_smooth_play - q0.t) / bspan, 0.0, 1.0));
    if (skinned) {
      if (q0.b.size() == item.bones.size() && q1.b.size() == item.bones.size()) {
        // Pass 1: flag collapsed bones (the churning staged-constant rows
        // past the real skeleton, e.g. rows 53..83 of the 84-row character
        // bank, flag every frame too; they are unreferenced by vertices,
        // so rewriting them is harmless).
        static std::vector<uint8_t> s_collapsed;  // guest render thread only
        const size_t nbones = acc.size() / 12;
        s_collapsed.assign(nbones, 0);
        size_t ncollapsed = 0;
        for (size_t b = 0; b < nbones; ++b) {
          const size_t bi = b * 12;
          double fa = 0.0, fr = 0.0;
          for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
              const float av = acc[bi + r * 4 + c];
              const float rv = q1.b[bi + r * 4 + c];
              fa += double(av) * av;
              fr += double(rv) * rv;
            }
          }
          if (fa < fr * 0.94) {  // norm ratio < ~0.97: rotation collapsed
            s_collapsed[b] = 1;
            ++ncollapsed;
          }
        }
        if (ncollapsed > 0) {
          // Collapsed bone (rotating too fast inside the window to
          // average, spinning wheels, and briefly fast-swinging limbs):
          // plain adjacent-sample lerp at the playback point. R and t come
          // from the same pose pair (no orbit), samples are ~7.5 ms apart
          // (only a few % midpoint shrink), and limbs that trip the
          // detector during tricks render essentially correctly. KNOWN
          // COSMETIC LIMIT: wheels ride the raw 60 Hz-lumpy path while the
          // deck rides the boxcar, a subtle lag/catch-up between wheels
          // and board at speed. Smarter per-frame constructions all failed
          // WORSE; do not iterate live again; solve offline against the
          // bone_signal captures.
          for (size_t b = 0; b < nbones; ++b) {
            if (!s_collapsed[b]) {
              continue;
            }
            const size_t bi = b * 12;
            for (int i = 0; i < 12; ++i) {
              acc[bi + i] = q0.b[bi + i] + (q1.b[bi + i] - q0.b[bi + i]) * ba;
            }
          }
        }
      }
      std::copy(acc.begin(), acc.end(), item.bones.begin());
    } else {
      double fa = 0.0, fr = 0.0;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          fa += double(wacc[r * 4 + c]) * wacc[r * 4 + c];
          fr += double(q1.w[r * 4 + c]) * q1.w[r * 4 + c];
        }
      }
      if (fa < fr * 0.94) {
        for (int i = 0; i < 16; ++i) {
          item.world[i] = q0.w[i] + (q1.w[i] - q0.w[i]) * ba;
        }
      } else {
        std::memcpy(item.world, wacc, sizeof(wacc));
      }
    }
    if (bs_rec) {
      BoneSigAppend(1, key, now, g_smooth_play,
                    skinned ? item.bones.data() : item.world,
                    skinned ? uint32_t(item.bones.size()) : 16u);
    }
  }
  // Prune entities not seen recently (map otherwise grows with streaming).
  if (s_hist.size() > 2048) {
    for (auto it = s_hist.begin(); it != s_hist.end();) {
      it = it->second.seen + 60 < s_frame ? s_hist.erase(it) : std::next(it);
    }
  }
}

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  if (!SceneEnabled()) {
    return;
  }
  ++g_guest_frame;  // paces the world-item cache revalidation
  // Perf telemetry: guest frame interval + this frame's capture-hook cost.
  static PerfClock::time_point s_last_frame_tp{};
  const auto build_t0 = PerfClock::now();
  if (s_last_frame_tp.time_since_epoch().count() != 0) {
    g_pw_guest_dt.Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   build_t0 - s_last_frame_tp)
                                   .count()));
  }
  s_last_frame_tp = build_t0;
  g_pw_capture.Add(g_capture_frame_ns);
  g_capture_frame_ns = 0;
  struct BuildPerf {
    PerfClock::time_point t0;
    ~BuildPerf() {
      g_pw_build.Add(uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
              .count()));
    }
  } build_perf{build_t0};
  if (g_recording.load(std::memory_order_relaxed)) {
    // Flush this frame's deferred inline-ring payloads (2D BeginVertices
    // draws): the CPU has finished writing them by frame end, and the ring
    // has not yet been reused.
    std::lock_guard<std::mutex> lock(g_record_mutex);
    for (const PendingInlineDump& p : g_pending_inline_dumps) {
      if (p.draw_index >= g_recorded_draws.size() ||
          g_recorded_buffer_bytes + p.bytes > (512u << 20) ||
          !GuestRangeReadable(base, p.addr, p.bytes)) {
        continue;
      }
      RecordedBuffer buf;
      buf.vb_addr = p.addr;
      buf.ib_addr = 0;
      buf.fingerprint = (uint64_t(g_recorded_draws[p.draw_index]->frame) << 32) | p.addr;
      buf.vb.resize(p.bytes);
      std::memcpy(buf.vb.data(), base + p.addr, p.bytes);
      g_recorded_draws[p.draw_index]->vb_dump = uint32_t(g_recorded_buffers.size());
      g_recorded_buffer_bytes += p.bytes;
      g_recorded_buffers.push_back(std::move(buf));
    }
    g_pending_inline_dumps.clear();
  }
  // Publish this frame's 2D overlay draws (before any early return: menu
  // and empty frames still carry 2D). The inline-ring vertex payloads are
  // complete by frame end; convert them to little-endian and expand quad
  // lists into triangle lists so the render side stays trivial.
  {
    std::vector<Draw2d> frame_2d;
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      frame_2d.swap(g_frame_2d);
    }
    static thread_local std::vector<uint8_t> scratch_2d;
    std::vector<Draw2d> published;
    published.reserve(frame_2d.size());
    for (Draw2d& d : frame_2d) {
      const uint32_t bytes = d.count * d.stride;
      scratch_2d.resize(bytes);
      if (!GuestTryCopy(scratch_2d.data(), base + d.addr, bytes)) {
        continue;
      }
      // Guest dwords are big-endian.
      for (size_t i = 0; i + 4 <= scratch_2d.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, scratch_2d.data() + i, 4);
        v = BSwap32(v);
        std::memcpy(scratch_2d.data() + i, &v, 4);
      }
      // Verified capture-side vertex layouts, all normalized to one 28-byte
      // renderer layout {float4 pos, float2 uv, u32 rgba}:
      //   24-byte {float4 pos, float2 uv}          - APT elements
      //   20-byte {float3 pos, float2 uv}          - glyph text (bit 4)
      //   20-byte {float4 pos, u32 color}          - SimpleDraw untextured
      //   28-byte {float4 pos, u32 color, float2 uv} - SimpleDraw textured
      //   16-byte {float4 pos}                     - SimpleDraw solid fill
      //     (color rides in VS c8 = m[8]; the popup panel strips, Rewards
      //     title bar / teal body / tan footer, are these)
      // (SimpleDraw = bit 5; its DrawParameters ctor orders colours before
      // texcoords.)
      {
        static thread_local std::vector<uint8_t> norm_2d;
        norm_2d.resize(size_t(d.count) * 28);
        const float one = 1.0f;
        const float zero = 0.0f;
        const uint32_t white = 0xFFFFFFFFu;
        const bool font = (d.flags & 0x10u) != 0;
        const bool simple = (d.flags & 0x20u) != 0;
        bool ok = true;
        for (uint32_t v = 0; v < d.count && ok; ++v) {
          uint8_t* dst = norm_2d.data() + size_t(v) * 28;
          const uint8_t* src = scratch_2d.data() + size_t(v) * d.stride;
          if (d.stride == 16 && simple) {  // SimpleDraw: pos4, untextured
            std::memcpy(dst, src, 16);
            std::memcpy(dst + 16, &zero, 4);
            std::memcpy(dst + 20, &zero, 4);
            std::memcpy(dst + 24, &white, 4);
          } else if (d.stride == 24) {  // APT: pos4 + uv (with or without the
            // SimpleDraw bracket; the compass needle/icons are 24-byte
            // quads issued through SimpleDraw::Draw inside the HUD pass,
            // same simpledraw_SimpleDrawUVSC shader as plain APT elements;
            // requiring !simple here dropped them, a regression from adding
            // bracket bit 5)
            std::memcpy(dst, src, 24);
            std::memcpy(dst + 24, &white, 4);
          } else if (d.stride == 20 && font) {  // glyphs: pos3 + uv
            std::memcpy(dst, src, 12);
            std::memcpy(dst + 12, &one, 4);
            std::memcpy(dst + 16, src + 12, 8);
            std::memcpy(dst + 24, &white, 4);
          } else if (d.stride == 20 && simple) {  // SimpleDraw: pos4 + color
            std::memcpy(dst, src, 16);
            std::memcpy(dst + 16, &zero, 4);
            std::memcpy(dst + 20, &zero, 4);
            // The dword byteswap reversed the color's guest byte order
            // (r,g,b,a); restore it for R8G8B8A8_UNORM.
            dst[24] = src[19];
            dst[25] = src[18];
            dst[26] = src[17];
            dst[27] = src[16];
          } else if (d.stride == 28 && simple) {  // SimpleDraw: pos4+color+uv
            std::memcpy(dst, src, 16);
            std::memcpy(dst + 16, src + 20, 8);
            dst[24] = src[19];
            dst[25] = src[18];
            dst[26] = src[17];
            dst[27] = src[16];
          } else {
            ok = false;
          }
        }
        if (!ok) {
          g_draws_2d_other.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (d.stride == 16) {
          // Untextured fill: the captured fetch words are leftover state from
          // the previous textured draw; zero them so the replay binds the
          // white texture instead of tinting the fill with a stale texel.
          std::memset(d.fetch, 0, sizeof(d.fetch));
        }
        scratch_2d = norm_2d;
        d.stride = 28;
      }
      if (d.prim == 13 && d.count % 4 == 0) {
        // Quad list -> triangle list (v0,v1,v2)(v0,v2,v3).
        const uint32_t quads = d.count / 4;
        d.verts.resize(size_t(quads) * 6 * d.stride);
        static constexpr uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t q = 0; q < quads; ++q) {
          for (uint32_t t = 0; t < 6; ++t) {
            std::memcpy(d.verts.data() + (size_t(q) * 6 + t) * d.stride,
                        scratch_2d.data() + (size_t(q) * 4 + kOrder[t]) * d.stride,
                        d.stride);
          }
        }
        d.prim = 4;
        d.count = quads * 6;
      } else if (d.prim == 4 || d.prim == 5) {
        d.verts.assign(scratch_2d.begin(), scratch_2d.end());
      } else {
        continue;
      }
      published.push_back(std::move(d));
    }
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    g_scene_2d = std::move(published);
    ++g_scene_2d_generation;
  }
  // Publish this frame's in-world spline draws: evaluate the guest B-spline
  // VS on the CPU (see SplineDraw for the decoded algorithm) into final
  // clip-space strip vertices so the render side is a passthrough draw.
  {
    std::vector<SplineDraw> frame_spline;
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      frame_spline.swap(g_frame_spline);
    }
    std::vector<SplineDraw> published;
    published.reserve(frame_spline.size());
    for (SplineDraw& s : frame_spline) {
      const float* c = s.consts;
      const auto row = [&](int r) { return c + r * 4; };
      std::vector<uint8_t> out(size_t(s.count) * 28);
      float* dst = reinterpret_cast<float*>(out.data());
      const uint8_t* src = s.verts.data();
      bool ok = true;
      for (uint32_t v = 0; v < s.count; ++v, src += 12, dst += 7) {
        float p[3];
        for (int k = 0; k < 3; ++k) {
          uint32_t w;
          std::memcpy(&w, src + k * 4, 4);
          w = BSwap32(w);
          std::memcpy(&p[k], &w, 4);
        }
        if (!(p[0] >= 0.0f && p[0] < 142.0f)) {
          ok = false;
          break;
        }
        const int idx = int(p[0]);
        const float t = p[0] - float(idx);
        const int side = p[2] >= 0.5f ? 1 : 0;
        // Uniform cubic B-spline basis (matches the shader's embedded
        // coefficients exactly).
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float wgt[4] = {1.0f - 3.0f * t + 3.0f * t2 - t3,
                              3.0f * t3 - 6.0f * t2 + 4.0f,
                              -3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f, t3};
        // Blend the world-transformed control points (world columns c4..c6,
        // translation in .w), /6, plus the world-rotated extrusion offset.
        float wp[3] = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 4; ++k) {
          const float* cp = row(7 + idx + k);
          for (int a = 0; a < 3; ++a) {
            const float* wr = row(4 + a);
            wp[a] += wgt[k] *
                     (wr[0] * cp[0] + wr[1] * cp[1] + wr[2] * cp[2] + wr[3] * cp[3]);
          }
        }
        const float* off = row(151 + side);
        for (int a = 0; a < 3; ++a) {
          const float* wr = row(4 + a);
          wp[a] = wp[a] * (1.0f / 6.0f) +
                  (wr[0] * off[0] + wr[1] * off[1] + wr[2] * off[2] + wr[3] * off[3]);
        }
        float clip[4];
        for (int a = 0; a < 4; ++a) {
          const float* pr = row(a);
          clip[a] = pr[0] * wp[0] + pr[1] * wp[1] + pr[2] * wp[2] + pr[3];
        }
        // Near/far fade against i_clipvalues (clip-space z, pre-divide). A
        // zero range degenerates to a step, like the shader's rcp(0) = inf.
        const float* cv = row(150);
        const auto ramp = [](float z, float start, float range) {
          if (range > 1e-20f || range < -1e-20f) {
            const float r = (z - start) / range;
            return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
          }
          return z - start > 0.0f ? 1.0f : 0.0f;
        };
        const float fade =
            std::min(ramp(clip[2], cv[0], cv[1]), 1.0f - ramp(clip[2], cv[2], cv[3]));
        dst[0] = clip[0];
        dst[1] = clip[1];
        dst[2] = clip[2];
        dst[3] = clip[3];
        dst[4] = p[1];
        dst[5] = p[2];
        dst[6] = fade;
      }
      if (!ok) {
        continue;
      }
      s.verts.swap(out);
      published.push_back(std::move(s));
    }
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    g_scene_spline = std::move(published);
  }
  // Take this frame's hook-time dynamic items regardless of how we exit,
  // leaving them in place across an early return (no perspective view, empty
  // frame) would desynchronize the indices stored in the records.
  std::vector<DrawItem> dynitems;
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    dynitems.swap(g_frame_dynitems);
    g_frame_pending_by_buffers.clear();
    g_frame_char_refresh.clear();
  }
  // Take this frame's selection re-draw captures and re-arm the post-sky
  // window (must happen on every exit path, like the dynitems swap).
  std::vector<SelectedDrawKey> frame_selected;
  frame_selected.swap(g_frame_selected);
  g_sky_seen_this_frame = false;
  const bool outline_edge_seen = g_outline_edge_seen;
  g_outline_edge_seen = false;
  if (count == 0) {
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
  // Dynamic contexts are submitted several times per frame (once per pass);
  // each submission carries that pass's culled island list. Keep the fullest
  // one; a shadow-pass list can be missing body parts the main view needs.
  std::unordered_map<uint32_t, size_t> dyn_slot;
  // First refused/pending skinned capture per mesh: candidates for the
  // post-merge cross-frame palette rescue (see g_bones_cache). Ropa garments
  // are kept apart: their rescue must restore last frame's resolved MODE
  // (rigid vs skinned), never blindly re-skin (see g_ropa_state_cache).
  std::unordered_map<uint32_t, const DrawItem*> pending_skinned_by_mesh;
  std::unordered_map<uint32_t, const DrawItem*> pending_ropa_by_mesh;
  const auto total_indices = [](const DrawItem& d) {
    uint64_t n = 0;
    for (const DrawEntry& e : d.draws) n += e.index_count;
    return n;
  };
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    // Primary opaque list of the chosen view only; other lists (shadow
    // culling, transparents, z-prepass) duplicate the same geometry through
    // different MeshContext objects and z-fight.
    if (r.kind == 1 && (r.c != view || r.b != 20160)) {
      continue;
    }
    if (r.kind == 2 && r.b != view) {
      // World-path capture from another view (shadow cascade): rendering it
      // duplicates the entity as a ghost.
      seen.insert(r.a);
      continue;
    }
    if (r.kind == 0 || r.kind == 2 || r.kind == 3) {
      // Dynamic entity (kind 0), main-view world-path capture (kind 2) or
      // quad-list capture (kind 3): the complete item was built at hook time.
      seen.insert(r.a);
      if (r.c == 0) {
        g_rej_no_dynstate.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (r.c > dynitems.size()) {
        g_rej_dyn_range.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const DrawItem& cand = dynitems[r.c - 1];
      if (cand.pending) {
        // Deferred mesh whose draw never came, or a capture the palette
        // acceptance gates refused. Remember it: if NO copy of this mesh
        // publishes this frame, the post-merge rescue re-publishes it with
        // LAST frame's palette (see g_bones_cache); one frame of pose lag
        // beats a one-frame-missing hat.
        if (cand.ropa) {
          pending_ropa_by_mesh.try_emplace(cand.mesh, &cand);
        } else if (cand.skinned) {
          pending_skinned_by_mesh.try_emplace(cand.mesh, &cand);
        }
        (cand.skinned ? g_skinned_skipped : g_rigid_dropped)
            .fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (!REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
        continue;
      }
      auto [slot, inserted] = dyn_slot.try_emplace(r.a, scene.items.size());
      if (inserted) {
        scene.items.push_back(cand);
      } else if (total_indices(cand) > total_indices(scene.items[slot->second])) {
        scene.items[slot->second] = cand;
      }
      continue;
    }
    if (!seen.insert(r.a).second) {
      continue;
    }
    if (!REXCVAR_GET(skate3_native_render_scene_world_items)) {
      continue;
    }
    DrawItem item;
    if (BuildItemGeometry(base, r.a, item)) {
      if (item.skinned) {
        // Skinned meshes reached through the world sort lists (flags,
        // banners) have no captured palette, bind-pose garbage; skip.
        g_skinned_skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (item.pos_fmt != 57) {
        // Model-space prop (vending machines, dispensers): its vertices need
        // the per-draw transform, which only the kind-2 hook-time capture
        // has; rendered here with identity it collapses at the world
        // origin. The capture handles it (or it is dropped when pending).
        g_world_props.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      scene.items.push_back(std::move(item));
    }
  }
  // Cross-frame palette rescue + cache refresh (see g_bones_cache): exactly
  // one published copy of a skinned mesh -> refresh its cache entry; zero
  // copies but a refused/pending capture -> re-publish with the cached
  // palette (one frame of pose lag instead of a one-frame disappearance).
  if (REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
    std::unordered_map<uint32_t, uint32_t> pub_count;
    for (const DrawItem& item : scene.items) {
      // Ropa garments count in EITHER resolved mode (a rigid-resolved copy
      // has no bones but is very much alive; rescuing a pending clone next
      // to it would draw a duplicate garment).
      if (item.ropa || (item.skinned && !item.bones.empty())) {
        ++pub_count[item.mesh];
      }
    }
    for (const DrawItem& item : scene.items) {
      if (!(item.ropa || (item.skinned && !item.bones.empty())) ||
          pub_count[item.mesh] != 1) {
        continue;
      }
      if (item.ropa) {
        // Remember the resolved mode + transform (see g_ropa_state_cache).
        if (g_ropa_state_cache.size() < 512) {
          RopaResolvedState& c = g_ropa_state_cache[item.mesh];
          c.skinned = item.skinned && !item.bones.empty();
          std::memcpy(c.world, item.world, sizeof(c.world));
          c.bones = item.bones;
        }
      } else if (item.skinned && !item.bones.empty() &&
                 g_bones_cache.size() < 512) {
        g_bones_cache[item.mesh] = item.bones;
      }
    }
    for (const auto& [mesh, cand] : pending_skinned_by_mesh) {
      if (pub_count.find(mesh) != pub_count.end()) {
        continue;  // a live copy published; nothing to rescue
      }
      const auto bit = g_bones_cache.find(mesh);
      if (bit == g_bones_cache.end()) {
        continue;
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.bones = bit->second;
      rescued.pending = false;
      g_bones_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // Refused ropa captures re-publish last frame's resolved state: mode,
    // world AND palette together (mixing frames' interpretations is the
    // mangled-ribbon bug; see g_ropa_state_cache).
    for (const auto& [mesh, cand] : pending_ropa_by_mesh) {
      if (pub_count.find(mesh) != pub_count.end()) {
        continue;  // a live copy published; nothing to rescue
      }
      const auto rit = g_ropa_state_cache.find(mesh);
      if (rit == g_ropa_state_cache.end()) {
        continue;
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.skinned = rit->second.skinned;
      rescued.bones = rit->second.bones;
      std::memcpy(rescued.world, rit->second.world, sizeof(rescued.world));
      rescued.pending = false;
      g_ropa_rescued.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Cross-frame character-lighting fallback (see g_char_rows_cache): items
  // whose capture chain never validated THIS frame reuse their garment's
  // last good rows instead of dropping to the empirical look (and, for
  // hair, out of the blended sub-pass). Single-instance meshes only;
  // clones carry per-instance rows.
  {
    std::unordered_map<uint32_t, uint32_t> char_mesh_count;
    for (const DrawItem& item : scene.items) {
      if (item.char_family != 0) {
        ++char_mesh_count[item.mesh];
      }
    }
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.char_rows[14 * 4 + 1] > 0.0f ||
          char_mesh_count[item.mesh] != 1) {
        continue;
      }
      const auto cit = g_char_rows_cache.find(item.mesh);
      if (cit != g_char_rows_cache.end()) {
        std::memcpy(item.char_rows, cit->second.data(), sizeof(item.char_rows));
        g_char_rows_reused.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if (scene.items.empty()) {
    return;
  }
  // Selected-object outline: flag items matching this frame's post-sky
  // re-draw captures. >= 2 identical draws = the stencil-marking pair; a
  // single occurrence is a legitimately late-drawn object, not a selection.
  // BOTH signals are required: the guest's own postfx_edgedetectstencil draw
  // must have run this frame too (park editor with an active selection);
  // gameplay draws some small props twice after the sky as well, which used
  // to outline distant objects during normal play.
  {
    uint32_t outline_items = 0;
    for (const SelectedDrawKey& k : frame_selected) {
      if (!outline_edge_seen || k.count < 2) {
        continue;
      }
      for (DrawItem& item : scene.items) {
        if (item.ib_obj == k.ib && item.vb_obj == k.vb &&
            std::fabs(item.world[12] - k.t[0]) < 0.05f &&
            std::fabs(item.world[13] - k.t[1]) < 0.05f &&
            std::fabs(item.world[14] - k.t[2]) < 0.05f && !item.selected) {
          item.selected = true;
          ++outline_items;
        }
      }
    }
    std::memcpy(scene.outline_color, g_outline_color, sizeof(scene.outline_color));
    static uint32_t s_outline_items = 0;
    if (outline_items != s_outline_items) {
      REXLOG_INFO("native-scene: selection outline {} item(s) (captures={})",
                  outline_items, frame_selected.size());
      s_outline_items = outline_items;
    }
  }
  for (int i = 0; i < 16; ++i) {
    scene.view_proj[i] = LoadGuestF32(base, viewcam + kViewCamViewProj + i * 4);
  }
  // Camera cadence telemetry (see g_cam_changes): does the guest publish a
  // NEW camera every rendered frame, or step it on a slower sim tick?
  {
    static float s_last_vp[16] = {};
    static uint32_t s_streak = 0;
    if (std::memcmp(s_last_vp, scene.view_proj, sizeof(s_last_vp)) == 0) {
      ++s_streak;
      g_cam_repeats.fetch_add(1, std::memory_order_relaxed);
      uint64_t prev = g_cam_max_streak.load(std::memory_order_relaxed);
      while (s_streak > prev && !g_cam_max_streak.compare_exchange_weak(
                                    prev, s_streak, std::memory_order_relaxed)) {
      }
    } else {
      s_streak = 0;
      g_cam_changes.fetch_add(1, std::memory_order_relaxed);
      std::memcpy(s_last_vp, scene.view_proj, sizeof(s_last_vp));
    }
  }
  // Camera position from the view matrix (+0x20, row-vector convention):
  // cam = -t * R^T.
  float cam_view[16];
  for (int i = 0; i < 16; ++i) {
    cam_view[i] = LoadGuestF32(base, viewcam + 0x20 + i * 4);
  }
  for (int j = 0; j < 3; ++j) {
    scene.cam_pos[j] =
        -(cam_view[12] * cam_view[j * 4 + 0] + cam_view[13] * cam_view[j * 4 + 1] +
          cam_view[14] * cam_view[j * 4 + 2]);
  }
  // The game's projection uses a negative x scale which already yields
  // correct D3D NDC orientation; use the view*proj matrix as captured.
  // (Negating column 0 here mirrors the image left-right.)

  // Camera re-timing (see SmoothCamera): replace the guest's sim-stepped
  // pose with a host-clock-interpolated one so panning is smooth at render
  // rate. All consumers (items, sky follow, sorting, outline) use the
  // smoothed pose coherently.
  if (REXCVAR_GET(skate3_native_render_scene_smooth_camera)) {
    // Publish the ViewCamera for the ~1 kHz sampler thread and make sure it
    // runs (see CamSamplerLoop).
    g_sampler_viewcam.store(viewcam, std::memory_order_relaxed);
    EnsureCamSampler();
    float proj[16];
    for (int i = 0; i < 16; ++i) {
      proj[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
    }
    const double now_s =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    float smooth_vp[16], smooth_cam[3];
    if (SmoothCamera(cam_view, proj, scene.view_proj, scene.cam_pos, now_s, smooth_vp,
                     smooth_cam)) {
      std::memcpy(scene.view_proj, smooth_vp, sizeof(smooth_vp));
      std::memcpy(scene.cam_pos, smooth_cam, sizeof(smooth_cam));
      // Keep the skater/NPCs/props in phase with the smoothed camera:
      // interpolate their palettes/worlds at the same playback time.
      InterpolateDynamicItems(scene, now_s);
    }
  }

  // Camera-signal recorder (see CamSigEntry): per-frame raw + smoothed
  // heading while the window is open; write + reset when it closes.
  {
    const double dl = g_camsig_deadline.load(std::memory_order_relaxed);
    if (dl > 0.0) {
      const double rec_now =
          std::chrono::duration<double>(build_t0.time_since_epoch()).count();
      if (rec_now < dl) {
        std::lock_guard<std::mutex> lock(g_camsig_mutex);
        if (g_camsig.size() < 200000) {
          g_camsig.push_back({rec_now, 0.0, YawFromViewRows(cam_view), 2});
          if (g_smooth_active) {
            float rot[16] = {};
            ViewRotFromQuat(g_smooth_pose.q, rot);
            g_camsig.push_back({rec_now, g_smooth_play, YawFromViewRows(rot), 1});
          }
        }
      } else {
        std::vector<CamSigEntry> entries;
        {
          std::lock_guard<std::mutex> lock(g_camsig_mutex);
          entries.swap(g_camsig);
        }
        g_camsig_deadline.store(0.0, std::memory_order_release);
        std::thread([entries = std::move(entries)]() {
          std::error_code ec;
          std::filesystem::create_directories("logs", ec);
          char path[128];
          std::snprintf(path, sizeof(path), "logs/cam_signal_%lld.csv",
                        static_cast<long long>(std::time(nullptr)));
          std::ofstream f(path);
          f << "kind,t,play_t,yaw_deg\n";
          char line[128];
          for (const CamSigEntry& e : entries) {
            std::snprintf(line, sizeof(line), "%d,%.6f,%.6f,%.5f\n", int(e.kind), e.t,
                          e.play_t, double(e.yaw));
            f << line;
          }
          REXLOG_INFO("native-scene cam-signal: wrote {} entries -> {}", entries.size(),
                      path);
        }).detach();
      }
    }
  }

  // Synthetic camera pan probe (see the synthetic_pan cvar comment for the
  // mode semantics). Runs AFTER the smoothing block: modes 1/2 override the
  // published pose outright (their point is to bypass the guest pose path);
  // mode 3 leaves the smoothed pose in place, the sampler thread is feeding
  // the smoother synthetic samples, and measures reconstruction error
  // against the known ideal.
  {
    const double syn_now =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    int syn_mode =
        std::clamp(int(REXCVAR_GET(skate3_native_render_scene_synthetic_pan)), 0, 3);
    if (syn_mode == 3 && !REXCVAR_GET(skate3_native_render_scene_smooth_camera)) {
      static bool s_warned = false;
      if (!s_warned) {
        s_warned = true;
        REXLOG_WARN(
            "native-scene synthetic-pan: mode 3 needs smooth_camera ON; "
            "running mode 1 instead");
      }
      syn_mode = 1;
    }
    static int s_engaged_mode = 0;
    if (syn_mode != s_engaged_mode) {
      s_engaged_mode = syn_mode;
      if (syn_mode == 0) {
        g_synpan_active.store(0, std::memory_order_release);
        g_synpan_union.clear();
        REXLOG_INFO("native-scene synthetic-pan: off (guest camera restored)");
      } else {
        // (Re-)engage from THIS frame's raw guest pose: heading, position
        // and projection are frozen; only the synthetic yaw moves.
        std::lock_guard<std::mutex> lock(g_synpan_mutex);
        std::memcpy(g_synpan_view0, cam_view, sizeof(g_synpan_view0));
        for (int i = 0; i < 16; ++i) {
          g_synpan_proj0[i] = LoadGuestF32(base, viewcam + 0x60 + i * 4);
        }
        for (int j = 0; j < 3; ++j) {
          g_synpan_c0[j] =
              -(cam_view[12] * cam_view[j * 4 + 0] + cam_view[13] * cam_view[j * 4 + 1] +
                cam_view[14] * cam_view[j * 4 + 2]);
        }
        g_synpan_t0 = syn_now;
        g_synpan_step_phase = 0.0;
        g_synpan_ema_dt = 0.0;
        g_synpan_frames = 0;
        g_synpan_last_build = 0.0;
        g_synpan_dt_sum = g_synpan_dt_sum2 = 0.0;
        g_synpan_dt_min = g_synpan_dt_max = 0.0;
        g_synpan_err_sum2 = g_synpan_err_max = 0.0;
        g_synpan_err_n = 0;
        g_synpan_union.clear();
        g_synpan_active.store(syn_mode, std::memory_order_release);
        static const char* kModeNames[] = {"off", "time-based", "fixed-step",
                                           "through-smoother"};
        REXLOG_INFO(
            "native-scene synthetic-pan: ENGAGED mode={} ({}) rate={:.1f} deg/s "
            "amp=+-{:.1f} deg",
            syn_mode, kModeNames[syn_mode],
            REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate),
            REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp));
      }
    }
    if (syn_mode != 0) {
      const double rate = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate);
      const double amp = REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp);
      const double prev_build = g_synpan_last_build;
      g_synpan_last_build = syn_now;
      const double dt = prev_build > 0.0 ? std::clamp(syn_now - prev_build, 0.0, 0.05) : 0.0;
      if (dt > 0.0) {
        g_synpan_dt_sum += dt;
        g_synpan_dt_sum2 += dt * dt;
        g_synpan_dt_min = g_synpan_dt_min == 0.0 ? dt : std::min(g_synpan_dt_min, dt);
        g_synpan_dt_max = std::max(g_synpan_dt_max, dt);
      }
      if (syn_mode == 1 || syn_mode == 2) {
        double phase;
        if (syn_mode == 1) {
          phase = (syn_now - g_synpan_t0) * rate;
        } else {
          // Fixed step: constant angle per published frame. The step is
          // rate * (slow EMA of dt) so deg/s stays roughly honest while the
          // per-frame advance is effectively constant over any short window.
          if (dt > 0.0) {
            g_synpan_ema_dt =
                g_synpan_ema_dt == 0.0 ? dt : g_synpan_ema_dt * 0.995 + dt * 0.005;
          }
          g_synpan_step_phase += rate * g_synpan_ema_dt;
          phase = g_synpan_step_phase;
        }
        float sview[16];
        SynPanView(SynPanAngleDeg(phase, amp), sview);
        CamPose pose;
        QuatFromView(sview, pose.q);
        std::memcpy(pose.c, g_synpan_c0, sizeof(pose.c));
        ComposeViewProj(pose, g_synpan_proj0, scene.view_proj, scene.cam_pos);
      } else if (g_smooth_active) {
        // Mode 3: the smoother just reconstructed a pose from the synthetic
        // samples at playback time g_smooth_play; compare against the ideal
        // pose at that exact time (both are functions of the same clock).
        float iview[16];
        SynPanView(SynPanAngleDeg((g_smooth_play - g_synpan_t0) * rate, amp), iview);
        float qi[4];
        QuatFromView(iview, qi);
        const float dq =
            std::fabs(qi[0] * g_smooth_pose.q[0] + qi[1] * g_smooth_pose.q[1] +
                      qi[2] * g_smooth_pose.q[2] + qi[3] * g_smooth_pose.q[3]);
        const double err_deg =
            2.0 * std::acos(std::min(dq, 1.0f)) * (180.0 / 3.14159265358979323846);
        g_synpan_err_sum2 += err_deg * err_deg;
        g_synpan_err_max = std::max(g_synpan_err_max, err_deg);
        ++g_synpan_err_n;
      }
      // World union: accumulate this frame's static items and append every
      // previously seen one the game didn't submit this frame (it culls to
      // ITS frustum; the probe camera looks elsewhere). Statics only:
      // skinned/cloth poses go stale immediately.
      {
        std::unordered_set<uint64_t> cur;
        cur.reserve(scene.items.size());
        const size_t published = scene.items.size();
        for (size_t i = 0; i < published; ++i) {
          const DrawItem& it = scene.items[i];
          if (it.skinned || it.cloth_quads || it.ropa || it.pending ||
              !it.bones.empty()) {
            continue;
          }
          const uint64_t key = SynPanItemKey(it);
          cur.insert(key);
          if (g_synpan_union.size() < 20000) {
            auto [slot, inserted] = g_synpan_union.try_emplace(key, it);
            if (!inserted &&
                std::memcmp(slot->second.world, it.world, sizeof(it.world)) != 0) {
              slot->second = it;  // a movable prop moved: refresh
            }
          }
        }
        for (const auto& [key, it] : g_synpan_union) {
          if (cur.find(key) == cur.end()) {
            scene.items.push_back(it);
          }
        }
      }
      if (++g_synpan_frames % 600 == 0) {
        const double n = std::max<double>(1.0, double(g_synpan_frames - 1));
        const double avg = g_synpan_dt_sum / n;
        const double sd =
            std::sqrt(std::max(0.0, g_synpan_dt_sum2 / n - avg * avg));
        if (syn_mode == 3) {
          REXLOG_INFO(
              "native-scene synthetic-pan: mode=3 frames={} build_dt_ms[avg/min/max/sd]="
              "{:.2f}/{:.2f}/{:.2f}/{:.2f} smoother_err_deg[rms/max]={:.4f}/{:.4f} (n={}) "
              "union={}",
              g_synpan_frames, avg * 1e3, g_synpan_dt_min * 1e3, g_synpan_dt_max * 1e3,
              sd * 1e3,
              std::sqrt(g_synpan_err_sum2 / std::max<uint64_t>(1, g_synpan_err_n)),
              g_synpan_err_max, g_synpan_err_n, g_synpan_union.size());
        } else {
          REXLOG_INFO(
              "native-scene synthetic-pan: mode={} frames={} build_dt_ms[avg/min/max/sd]="
              "{:.2f}/{:.2f}/{:.2f}/{:.2f} union={}",
              syn_mode, g_synpan_frames, avg * 1e3, g_synpan_dt_min * 1e3,
              g_synpan_dt_max * 1e3, sd * 1e3, g_synpan_union.size());
        }
      }
    }
  }

  // Publish the frame's captured fog rows and re-arm the OnDrawDone capture
  // (keyed to this frame's camera) for the next frame.
  if (g_fog_have) {
    std::memcpy(scene.fog_ramp, g_fog_rows, 4 * sizeof(float));
    std::memcpy(scene.fog_color, g_fog_rows + 4, 4 * sizeof(float));
  }
  if (g_shadow_have) {
    std::memcpy(scene.shadow_rows, g_shadow_rows, sizeof(g_shadow_rows));
    scene.shadow_valid = true;
  }
  std::memcpy(scene.family_rows, g_family_rows, sizeof(g_family_rows));
  if (g_dynobj_have) {
    std::memcpy(scene.dynobj_rows, g_dynobj_rows, sizeof(g_dynobj_rows));
    scene.dynobj_valid = true;
  }
  if (g_sky_have) {
    scene.sky_height = g_sky_height;
  }
  const bool blur_active = g_ui_blur_seen || g_ui_blur_hold > 0;
  scene.ui_blur = blur_active ? g_ui_blur : 0.0f;
  if (g_ui_blur_seen) {
    g_ui_blur_hold = 2;
  } else if (g_ui_blur_hold > 0) {
    --g_ui_blur_hold;
  }
  {
    static bool s_blur_was_active = false;
    if (blur_active != s_blur_was_active) {
      REXLOG_INFO("native-scene: popup background blur {} (kernel scale {:.1f})",
                  blur_active ? "ON" : "off", g_ui_blur);
      s_blur_was_active = blur_active;
    }
  }
  g_ui_blur_seen = false;
  std::memcpy(g_fog_cam, scene.cam_pos, sizeof(g_fog_cam));
  g_fog_frame_done = false;
  g_shadow_frame_done = false;
  g_sky_frame_done = false;
  g_tree_frame_done = false;
  g_proxy_frame_done = false;
  g_dynobj_frame_done = false;

  // Dynamic cloth decode jobs (see DynDecodeJob): snapshot CHANGED (or
  // first-seen) skinned/ropa payloads for the workers; the render thread
  // never decodes them inline; a first-sight NPC/garment appears 1-2 frames
  // late instead of hitching the frame it streams in on. GuestTryCopy is
  // safe here: the game just drew from these payloads this frame.
  {
    // mesh -> last enqueued fingerprint (guest render thread only). Static
    // skinned meshes enqueue once; ropa every frame.
    static std::unordered_map<uint32_t, uint64_t> s_dyn_fp_sent;
    static uint64_t s_dyn_seq = 0;
    if (s_dyn_fp_sent.size() > 4096) {
      s_dyn_fp_sent.clear();
    }
    std::vector<DynDecodeJob> jobs;
    for (const DrawItem& item : scene.items) {
      if (!(item.skinned || item.ropa) || item.cloth_quads || item.ib_addr == 0) {
        continue;
      }
      auto [fit, first_sight] = s_dyn_fp_sent.try_emplace(item.mesh, item.fingerprint);
      if (!first_sight && fit->second == item.fingerprint) {
        continue;
      }
      fit->second = item.fingerprint;
      DynDecodeJob job;
      job.item = item;
      job.item.bones.clear();
      job.seq = ++s_dyn_seq;
      job.vb.resize(item.vb_bytes);
      if (!GuestTryCopy(job.vb.data(), base + item.vb_addr, item.vb_bytes)) {
        continue;
      }
      job.ib.resize(size_t(item.ib_count) * 2);
      if (!GuestTryCopy(job.ib.data(), base + item.ib_addr, job.ib.size())) {
        continue;
      }
      jobs.push_back(std::move(job));
    }
    if (!jobs.empty()) {
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      for (DynDecodeJob& j : jobs) {
        if (g_dyn_jobs.size() >= 32) {
          break;  // workers behind: the cloth skips a sim frame
        }
        g_dyn_jobs.push_back(std::move(j));
      }
      g_prewarm_cv.notify_all();
    }
  }

  if (g_recording.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    if (++g_frames_seen % g_record_stride == 0) {
      RecordedFrame rf;
      rf.generation = g_generation + 1;
      std::memcpy(rf.view_proj, scene.view_proj, sizeof(rf.view_proj));
      std::memcpy(rf.cam_pos, scene.cam_pos, sizeof(rf.cam_pos));
      rf.dynitems = dynitems;
      rf.items = scene.items;
      g_recorded_frames.push_back(std::move(rf));
      ++g_record_frame;
    }
  }

  // The draw-time fetch map served this frame's item builds (streamed-artwork
  // diffuse override); next frame's draws repopulate it. Cleared HERE, not at
  // the top with the other per-frame structures; the world items that
  // consume it are built above, after that swap.
  {
    std::lock_guard<std::mutex> lock(g_palette_mutex);
    g_frame_draw_fetch.clear();
  }

  std::lock_guard<std::mutex> lock(g_scene_mutex);
  scene.generation = ++g_generation;
  g_scene = std::make_shared<const FrameScene>(std::move(scene));
}

void StartRecording(uint32_t stride) {
  std::lock_guard<std::mutex> lock(g_record_mutex);
  g_recorded_draws.clear();
  g_recorded_frames.clear();
  g_recorded_buffers.clear();
  g_recorded_buffer_keys.clear();
  g_recorded_buffer_bytes = 0;
  g_pending_inline_dumps.clear();
  g_frame_dump_ids.clear();
  g_record_frame = 0;
  g_frames_seen = 0;
  g_record_stride = stride == 0 ? 1 : stride;
  g_recording.store(true, std::memory_order_relaxed);
}

void WriteRecording(const char* dir, const char* stem) {
  g_recording.store(false, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_record_mutex);
  const std::filesystem::path base_path = std::filesystem::path(dir) / stem;

  // Binary draw stream: header "SK3DRAW7", fixed records {u32 frame, u32
  // func, u32 ib, u32 vb, u32 vb_offset, u32 vb_stride, u32 streams[4][3],
  // u32 vfetch[12], u32 args[4], f32 vs_bank[1024], f32 ps_bank[256],
  // u32 flags2d, u32 ps_obj, u32 vs_obj, u32 viewport[6], u32 scissor[4],
  // u32 rstates[256], u32 vfetch_all[192], u32 vb_dump, u32 ib_dump}
  // (little-endian). func: 0 DrawIndexedVertices, 1 DrawVertices,
  // 2 BeginVertices. flags2d: bit0 FrontEndManager::Render2D, bit1
  // AptMovieIntegration::Render, bit2 DrawRenderingUnit. vb_dump/ib_dump
  // index into buffers.bin records (~0u = none).
  {
    std::ofstream out(base_path.string() + ".draws.bin", std::ios::binary);
    out.write("SK3DRAW7", 8);
    for (const auto& d : g_recorded_draws) {
      out.write(reinterpret_cast<const char*>(&d->frame), 4);
      out.write(reinterpret_cast<const char*>(&d->func), 4);
      out.write(reinterpret_cast<const char*>(&d->ib), 4);
      out.write(reinterpret_cast<const char*>(&d->vb), 4);
      out.write(reinterpret_cast<const char*>(&d->vb_offset), 4);
      out.write(reinterpret_cast<const char*>(&d->vb_stride), 4);
      out.write(reinterpret_cast<const char*>(d->streams), 48);
      out.write(reinterpret_cast<const char*>(d->vfetch), 48);
      out.write(reinterpret_cast<const char*>(d->args), 16);
      out.write(reinterpret_cast<const char*>(d->bank), 4096);
      out.write(reinterpret_cast<const char*>(d->ps), 1024);
      out.write(reinterpret_cast<const char*>(&d->flags2d), 4);
      out.write(reinterpret_cast<const char*>(&d->ps_obj), 4);
      out.write(reinterpret_cast<const char*>(&d->vs_obj), 4);
      out.write(reinterpret_cast<const char*>(d->viewport), 24);
      out.write(reinterpret_cast<const char*>(d->scissor), 16);
      out.write(reinterpret_cast<const char*>(d->rstates), 1024);
      out.write(reinterpret_cast<const char*>(d->vfetch_all), 768);
      out.write(reinterpret_cast<const char*>(&d->vb_dump), 4);
      out.write(reinterpret_cast<const char*>(&d->ib_dump), 4);
    }
  }

  // Captured buffer payloads: header "SK3BUFS1", then records {u32 vb_addr,
  // u32 ib_addr, u64 fingerprint, u32 vb_len, u32 ib_len, raw vb, raw ib}.
  {
    std::ofstream out(base_path.string() + ".buffers.bin", std::ios::binary);
    out.write("SK3BUFS1", 8);
    for (const RecordedBuffer& b : g_recorded_buffers) {
      const uint32_t vb_len = uint32_t(b.vb.size());
      const uint32_t ib_len = uint32_t(b.ib.size());
      out.write(reinterpret_cast<const char*>(&b.vb_addr), 4);
      out.write(reinterpret_cast<const char*>(&b.ib_addr), 4);
      out.write(reinterpret_cast<const char*>(&b.fingerprint), 8);
      out.write(reinterpret_cast<const char*>(&vb_len), 4);
      out.write(reinterpret_cast<const char*>(&ib_len), 4);
      out.write(reinterpret_cast<const char*>(b.vb.data()), vb_len);
      out.write(reinterpret_cast<const char*>(b.ib.data()), ib_len);
    }
  }

  // Per-frame item dump.
  {
    std::ofstream out(base_path.string() + ".scene.jsonl");
    const auto write_item = [&out](const DrawItem& d) {
      out << "{\"mesh\":\"" << std::hex << d.mesh << "\",\"ib_obj\":\"" << d.ib_obj
          << "\",\"vb_obj\":\"" << d.vb_obj << "\",\"vb_addr\":\"" << d.vb_addr
          << "\",\"ib_addr\":\"" << d.ib_addr << "\",\"diffuse\":\"" << d.diffuse_tex
          << "\",\"diffuse_fetch\":\"" << d.diffuse_fetch[1]
          << "\",\"lightmap\":\"" << d.lightmap_tex << "\",\"macro\":\"" << d.macro_tex
          << "\",\"decal_art\":\"" << d.decal_art
          << "\",\"decal_fetch\":\"" << d.decal_fetch[1]
          << "\",\"fp\":\"" << d.fingerprint
          << "\"" << std::dec << ",\"vb_bytes\":" << d.vb_bytes << ",\"ib_count\":"
          << d.ib_count << ",\"stride\":" << int(d.stride) << ",\"pos_fmt\":"
          << int(d.pos_fmt) << ",\"pos_off\":" << d.pos_offset << ",\"bw_off\":"
          << d.bw_offset << ",\"bi_off\":" << d.bi_offset << ",\"skinned\":"
          << (d.skinned ? 1 : 0) << ",\"pending\":" << (d.pending ? 1 : 0)
          << ",\"decal\":" << (d.decal ? 1 : 0)
          << ",\"transparent\":" << (d.transparent ? 1 : 0)
          << ",\"selected\":" << (d.selected ? 1 : 0) << ",\"world\":[";
      for (int i = 0; i < 16; ++i) out << (i ? "," : "") << d.world[i];
      out << "],\"draws\":[";
      for (size_t i = 0; i < d.draws.size(); ++i) {
        const DrawEntry& e = d.draws[i];
        out << (i ? "," : "") << "[" << e.prim << "," << e.base_vertex << ","
            << e.start_index << "," << e.index_count << "]";
      }
      out << "],\"bones\":[";
      for (size_t i = 0; i < d.bones.size(); ++i) out << (i ? "," : "") << d.bones[i];
      out << "]}";
    };
    for (const RecordedFrame& rf : g_recorded_frames) {
      out << "{\"generation\":" << rf.generation << ",\"cam\":[" << rf.cam_pos[0] << ","
          << rf.cam_pos[1] << "," << rf.cam_pos[2] << "],\"view_proj\":[";
      for (int i = 0; i < 16; ++i) out << (i ? "," : "") << rf.view_proj[i];
      out << "],\"dynitems\":[";
      for (size_t i = 0; i < rf.dynitems.size(); ++i) {
        if (i) out << ",";
        write_item(rf.dynitems[i]);
      }
      out << "],\"items\":[";
      for (size_t i = 0; i < rf.items.size(); ++i) {
        if (i) out << ",";
        write_item(rf.items[i]);
      }
      out << "]}\n";
    }
  }
  REXLOG_INFO(
      "native-scene: recording written ({} draws, {} frames, {} buffers {} MiB) -> "
      "{}.draws.bin/.scene.jsonl/.buffers.bin",
      g_recorded_draws.size(), g_recorded_frames.size(), g_recorded_buffers.size(),
      g_recorded_buffer_bytes >> 20, base_path.string());
  g_recorded_draws.clear();
  g_recorded_frames.clear();
  g_recorded_buffers.clear();
  g_recorded_buffer_keys.clear();
  g_recorded_buffer_bytes = 0;
  g_pending_inline_dumps.clear();
  g_frame_dump_ids.clear();
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
  // Dynamic-payload decode order (DynDecodeJob::seq): the commit drops
  // results older than the cached entry so multi-worker reordering cannot
  // step the cloth backwards a frame. 0 on static decodes.
  uint64_t dyn_seq = 0;
  // Double-sided sheet prop (banners/flags): most triangles have an
  // opposite-winding twin ~1cm behind, and the two faces map to DIFFERENT
  // lightmap atlas cells (lit vs shaded side). Drawn without culling both
  // faces z-fight once their depth gap drops below buffer precision;
  // distant triangles alternate between the two cells per frame (the "flag
  // flicker", stops up close where 1cm still resolves). These meshes draw
  // with the backface-culling PSO instead.
  bool two_sided_sheet = false;
};

struct GuestTexture {
  ID3D12Resource* texture = nullptr;
  ID3D12Resource* upload = nullptr;  // kept alive; copy recorded in deferred list
  uint32_t fetch_words[6] = {};      // big-endian words as read for revalidation
  uint32_t srv_slot = 0;
  // For failed decodes: frame number for periodic retry (payload may stream
  // in after the fetch constant is already valid).
  uint64_t retry_after_frame = 0;
  // Payload-content revalidation: a texture first seen while its payload is
  // still STREAMING IN decodes to garbage (blocky macro-tile checkers /
  // blacked-out walls), and the fetch words never change when the content
  // finishes filling in at the same address, so the garbage decode was
  // cached forever. Sampled qwords across mip 0, rechecked periodically.
  uint32_t payload_addr = 0;   // 0xA-mirror guest address of mip 0
  uint32_t payload_size = 0;
  uint64_t payload_fp = 0;
  uint64_t recheck_frame = 0;
  bool valid = false;
};

// Cache key for draw-time fetch-word texture bindings (streamed artwork /
// decal ad overrides, no guest texture object to key on): FNV-1a over the
// six raw fetch-constant words. Shared by the draw path and the warmup
// pre-decode so both hit the same g_r.textures_2d entries.
inline uint64_t FetchWordsKey(const uint32_t words[6]) {
  uint64_t key = 1469598103934665603ull;
  for (int k = 0; k < 6; ++k) {
    key ^= words[k];
    key *= 1099511628211ull;
  }
  return key;
}

// FNV-1a over 16 qwords sampled across a guest payload (SEH-guarded reads;
// streaming can decommit the range). Returns 0 only on unreadable payloads.
uint64_t SamplePayloadFingerprint(uint8_t* base, uint32_t addr, uint32_t size) {
  if (addr == 0 || size < 8) {
    return 0;
  }
  uint64_t h = 1469598103934665603ull;
  for (uint32_t k = 0; k < 16; ++k) {
    const uint32_t off = uint32_t(uint64_t(size - 8) * k / 15u) & ~7u;
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + addr + off, sizeof(v))) {
      return 0;
    }
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

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

struct RendererState {
  ID3D12Device* device = nullptr;
  ID3D12RootSignature* root_signature = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12PipelineState* pso_cullback = nullptr;  // two_sided_sheet meshes (see MeshBuffers)
  ID3D12PipelineState* pso_nodepth = nullptr;
  // environment.transparent sub-pass: straight alpha blend, depth test on,
  // z-write OFF; items drawn back-to-front after all opaque items.
  ID3D12PipelineState* pso_transparent = nullptr;
  // Hair sub-passes: transparent blend state with cull BACK / cull FRONT
  // (the game's cac_hair/defaulthair two-pass draw order).
  ID3D12PipelineState* pso_hair_a = nullptr;
  ID3D12PipelineState* pso_hair_b = nullptr;
  DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
  ID3D12DescriptorHeap* rtv_heap = nullptr;  // slot 0 = output, slot 1 = MSAA
  ID3D12DescriptorHeap* dsv_heap = nullptr;
  ID3D12Resource* depth = nullptr;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  ID3D12Resource* rtv_resource = nullptr;
  // MSAA: the scene draws into msaa_color (+ MSAA depth) and a fullscreen
  // pass averages the samples into the guest output texture (the deferred
  // command list has no ResolveSubresource).
  uint32_t msaa = 1;
  ID3D12Resource* msaa_color = nullptr;
  ID3D12PipelineState* resolve_pso = nullptr;
  uint32_t msaa_srv_slot = 0;
  bool msaa_srv_allocated = false;
  // Popup background blur (see kBlurShaderSource): two intermediates at the
  // game's fixed 1152x640 internal resolution (RTV heap slots 5/6) + an SRV
  // slot re-pointed at the guest output each blur frame. Steady state for
  // blur_tex is RENDER_TARGET.
  static constexpr uint32_t kBlurWidth = 1152;
  static constexpr uint32_t kBlurHeight = 640;
  ID3D12Resource* blur_tex[2] = {nullptr, nullptr};
  uint32_t blur_srv[2] = {0, 0};
  uint32_t output_srv_slot = 0;
  bool output_srv_allocated = false;
  ID3D12PipelineState* pso_blur = nullptr;
  ID3D12PipelineState* pso_blur_blit = nullptr;
  ID3D12PipelineState* pso_blur_down = nullptr;
  // Selection outline (see DrawItem::selected): selected items re-render
  // into a single-sample R8 mask at OUTPUT resolution (RTV slot 7: a
  // low-res mask stairstepped the contour centerline at 4K) and a
  // fullscreen edge-detect pass with fixed UV-fraction tap pitch adds the
  // blue outline onto the resolved output (postfx_edgedetectstencil
  // equivalent). Mask steady state is RENDER_TARGET.
  ID3D12Resource* outline_mask = nullptr;
  uint32_t outline_mask_width = 0;
  uint32_t outline_mask_height = 0;
  uint32_t outline_mask_srv = 0;
  bool outline_mask_srv_allocated = false;
  ID3D12PipelineState* pso_outline_mask = nullptr;
  ID3D12PipelineState* pso_outline_edge = nullptr;
  uint32_t rtv_size = 0;
  std::unordered_map<uint32_t, MeshBuffers> meshes;
  // Buffers replaced by re-decode, kept alive until the GPU has finished the
  // submission that last referenced them.
  std::vector<std::pair<ID3D12Resource*, uint64_t>> retired;
  // Texture SRV staging: CPU-only heap; slots copied into the command
  // processor's one-use shader-visible descriptors per draw.
  ID3D12DescriptorHeap* srv_heap = nullptr;
  uint32_t srv_size = 0;
  uint32_t srv_next = 0;
  // Guest-texture SRV slot recycling: decodes are unbounded over a session
  // (streaming re-decodes, registration prewarm) and a monotonic allocator
  // exhausts the 8192-slot heap; every later decode then fails and renders
  // white. Retired slots wait out their last-referencing submission before
  // rejoining the free list (the descriptor may still be copied from while
  // the frame is in flight).
  std::vector<uint32_t> srv_free;
  std::vector<std::pair<uint32_t, uint64_t>> retired_srv_slots;
  std::unordered_map<uint32_t, GuestTexture> textures;
  GuestTexture white;
  // Water environment CUBE maps (t6): separate cache; same guest object
  // addresses decode differently (6 faces, TextureCube SRV).
  std::unordered_map<uint32_t, GuestTexture> cube_textures;
  GuestTexture white_cube;
  // Bone palette ring: persistent-mapped upload buffer, one region per
  // in-flight frame.
  static constexpr uint32_t kBoneRegionSize = 1u << 20;
  static constexpr uint32_t kBoneRegions = 4;
  ID3D12Resource* bone_ring = nullptr;
  uint8_t* bone_ring_cpu = nullptr;
  uint32_t bone_ring_offset = 0;
  // 2D overlay (HUD/APT replay): alpha-blended depth-less pipeline drawing
  // the captured inline vertices from a per-frame upload ring.
  ID3D12PipelineState* pso_2d = nullptr;
  // In-world neon splines, drawn inside the MSAA scene pass (depth test on,
  // no z-write): darken = straight alpha, default = additive glow.
  ID3D12PipelineState* pso_spline_darken = nullptr;
  ID3D12PipelineState* pso_spline_default = nullptr;
  // Dynamic CSM shadows: casters render
  // into a 3-tile (depth, coverage) atlas with MIN blend (depth clear 1,
  // "uncoverage" clear 1 -> covered texels write 0), then the game's exact
  // Gaussian-coverage + depth-dilation blur runs per tile (5-tap cascade 0,
  // 3-tap cascade 1, format-convert-only cascade 2) into the atlas the
  // scene pass samples at t5.
  ID3D12PipelineState* pso_shadow_caster = nullptr;
  ID3D12PipelineState* pso_shadow_blur = nullptr;
  ID3D12Resource* shadow_raw = nullptr;    // caster pass target (RTV slot 2)
  ID3D12Resource* shadow_mid = nullptr;    // hblur output (RTV slot 3)
  ID3D12Resource* shadow_final = nullptr;  // vblur output, sampled (RTV slot 4)
  uint32_t shadow_tile = 0;                // per-cascade tile size (atlas = 3*tile x tile)
  uint32_t shadow_srv_raw = 0;
  uint32_t shadow_srv_mid = 0;
  uint32_t shadow_srv_final = 0;
  // After a shadow pass all three textures sit in PIXEL_SHADER_RESOURCE;
  // the next pass transitions them back to RENDER_TARGET first.
  bool shadow_in_srv_state = false;
  // Per-frame shadow constant buffer ring (root CBV b1).
  static constexpr uint32_t kShadowCbRegions = 4;
  ID3D12Resource* shadow_cb = nullptr;
  uint8_t* shadow_cb_cpu = nullptr;
  static constexpr uint32_t kUiRegionSize = 1u << 20;
  static constexpr uint32_t kUiRegions = 4;
  ID3D12Resource* ui_ring = nullptr;
  uint8_t* ui_ring_cpu = nullptr;
  // Textures resolved from raw fetch-constant words: the 2D path binds its
  // textures through the device fetch shadow, not renderengine objects.
  // Keyed by an FNV hash of the 6 fetch words.
  std::unordered_map<uint64_t, GuestTexture> textures_2d;
  bool failed = false;
  bool announced = false;
};

RendererState g_r;

// Allocate a guest-texture SRV slot, preferring the recycled free list.
// Fixed/global slots (white, blur chain, MSAA, outline) keep the monotonic
// allocator; they are never retired.
bool AllocGuestSrvSlot(uint32_t& slot) {
  if (!g_r.srv_free.empty()) {
    slot = g_r.srv_free.back();
    g_r.srv_free.pop_back();
    return true;
  }
  if (g_r.srv_next >= 8192) {
    return false;
  }
  slot = g_r.srv_next++;
  return true;
}

// Retire a guest texture's GPU resources AND its SRV slot; everything waits
// out `submission` before being freed/recycled.
void RetireGuestTexture(const GuestTexture& t, uint64_t submission) {
  if (t.texture) g_r.retired.emplace_back(t.texture, submission);
  if (t.upload) g_r.retired.emplace_back(t.upload, submission);
  if (t.valid) g_r.retired_srv_slots.emplace_back(t.srv_slot, submission);
}

// ---- Staged texture decode (worker-thread half) ---------------------------
// The texture decoders normally finish by recording GPU copies into the
// deferred command list, pushing a barrier and creating the SRV: all
// render-thread-only. When `g_tex_stage_out` is set (decode worker), they
// stop after filling the upload resource and export what the render-thread
// commit needs instead. D3D12 resource creation and upload mapping are
// free-threaded, so everything up to that point is safe off-thread.
struct StagedMipCopy {
  uint32_t offset, pitch, w, h;  // upload footprint per mip
};
struct StagedTexCommit {
  DXGI_FORMAT copy_format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
  UINT swizzle_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  uint32_t mip_count = 0;
  // Cube map (environment cubes): mips[0..5] are the six FACES (subresource
  // = index, single mip) and the SRV is TEXTURECUBE.
  bool cube = false;
  StagedMipCopy mips[16] = {};
};
thread_local StagedTexCommit* g_tex_stage_out = nullptr;

// Render-thread half: record the staged upload's copies + barrier, create
// the SRV, mark the texture live. On SRV-slot exhaustion the texture stays
// invalid (renders white; slot recycling makes this near-impossible).
void CommitStagedGuestTexture(const NativeGuestOutputRenderContext& context,
                              GuestTexture& gt, const StagedTexCommit& sc) {
  auto& list = context.d3d12.command_processor->GetDeferredCommandList();
  for (uint32_t m = 0; m < sc.mip_count; ++m) {
    const StagedMipCopy& p = sc.mips[m];
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = gt.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = m;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = gt.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = p.offset;
    src.PlacedFootprint.Footprint.Format = sc.copy_format;
    src.PlacedFootprint.Footprint.Width = p.w;
    src.PlacedFootprint.Footprint.Height = p.h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = p.pitch;
    list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        gt.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (!AllocGuestSrvSlot(gt.srv_slot)) {
    gt.valid = false;
    return;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = sc.srv_format;
  srv.Shader4ComponentMapping = sc.swizzle_mapping;
  if (sc.cube) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = 1;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = sc.mip_count;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += size_t(gt.srv_slot) * g_r.srv_size;
  g_r.device->CreateShaderResourceView(gt.texture, &srv, slot);
  gt.valid = true;
}

// Worker-pool result plumbing (see the prewarm queue globals above; these
// live here because they need the resource/item types).
struct StagedTexResult {
  uint32_t key = 0;        // guest texture object address (object-keyed cache)
  uint64_t words_key = 0;  // != 0: words-keyed cache (g_r.textures_2d) instead
  bool cube = false;       // environment cube (g_r.cube_textures)
  GuestTexture gt;
  StagedTexCommit commit;
  bool valid = false;
};
struct PrewarmResult {
  DrawItem item;
  MeshBuffers buffers;
  bool mesh_valid = false;
  std::vector<StagedTexResult> textures;
};
std::mutex g_prewarm_out_mutex;
std::vector<PrewarmResult> g_prewarm_out;
// Failed builds (buffer objects not initialized yet) land here; the render
// thread re-injects them each frame so retries are frame-paced instead of
// hot-spinning the workers.
std::vector<PrewarmEntry> g_prewarm_retry;  // under g_prewarm_out_mutex

// Face-normal shading uses the camera-relative world position: interpolating
// absolute world coordinates (hundreds of meters) destroys ddx/ddy precision
// and produces per-pixel noise.
const char kShaderSource[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 world;
  row_major float4x4 mvp;
  float4 tint;
  float4 cam_pos;
  // Per-material color multiplier (w > 0 enables): CAS hair is a grayscale
  // texture times the character's hair color.
  float4 mat_tint;
  // x = macroOverlayUVScale, y = macroOverlayOpacity, z > 0 = macro overlay
  // texture bound at t3, w > 0 = environment.decal item with its `decal`
  // art bound at t4 (composited in-shader over the diffuse, exactly like
  // the game's decalenvironment PS: lerp by the art's alpha; opaque).
  // Macro and decal art are INDEPENDENT: decal ground/wall sections carry
  // the same macrooverlay as their non-decal neighbors, sharing one slot
  // dropped the macro grime there and rendered alternating plaza sections
  // ~1.4x too bright (the large-scale ground checkerboard).
  float4 overlay;
  // x > 0 = environment.transparent item (alpha-blended sub-pass): shading
  // follows the game's transparentenvironment.fx; the opaque pass's
  // alpha-test turned the soft mist gradients into solid white cloud blobs.
  // yzw = the global distance-fog RAMP (scale/bias/exponent from main-pass
  // VS c5), and mat_tint doubles as the linear-space fog COLOR (rgb +
  // transmittance scale in w, VS c6) for transparent items; the root
  // signature is capped at 64 DWORDs, so fog rides in slots transparent
  // items never use otherwise. Fog is currently applied to transparent
  // items only (the km-distant mist sheets; everything else we render is
  // near enough for fog to be negligible).
  float4 misc;
};
// Per-frame dynamic-shadow (CSM) receiver constants, captured from the
// game's world-material PIXEL banks.
cbuffer S : register(b1) {
  float4 sh_x;      // light-space X row (xyz) + translation (w)   [PS c0]
  float4 sh_y;      // light-space Y row                           [PS c3]
  float4 sh_z;      // depth row (height ramp)                     [PS c4]
  float4 sh_c1;     // cascade 1 scale.xy + offset.zw              [PS c1]
  float4 sh_c2;     // cascade 2 scale.xy + offset.zw              [PS c2]
  float4 sh_color;  // shadow color rgb [PS c8] + its luma in w
  float4 sh_misc;   // x = depth bias [PS c5.x], y = enable
  // Exact world-shading frame rows (consumed by the env-family branch):
  float4 sh_sun;    // xyz = sun direction [PS c6], w = scene exposure [c10.x]
  float4 sh_env;    // x = material multiplier [PS c11.y], yz = tree lightmap
                    // scale/floor [tree PS c0.xy], w = tree tint mult [c4.y]
  float4 sh_fogp;   // xyz = global fog ramp scale/bias/exp [VS c5],
                    // w = proxyworld scale [proxy PS c3.y]
  float4 sh_fogc;   // fog color rgb + transmittance scale in w [VS c6]
  // dynamicobject.fx frame-global lighting rows (see FrameScene::dynobj_rows).
  float4 dyn_sun;   // xyz = sun direction (PS c9), w = scene exposure (c13.x)
  float4 dyn_amb;   // xyz = flat ambient (c15.rgb), w = bounce scale (c15.w)
  float4 dyn_misc;  // x = material multiplier (c14.y),
                    // y = static world-shadow floor (c8.w)
};
// Character-family lighting (defaultcharacter.fx and friends): canonical
// per-draw rows captured from the guest PIXEL constant bank at palette
// capture (CaptureCharLighting has the per-family register maps; the math
// below was validated offline by executing the game's own pixel shaders).
// Enabled per draw via cam_pos.w = family (0 = not a character / capture
// failed -> the legacy empirical shading below).
cbuffer CH : register(b2) {
  float4 ch_light;  // xyz = sun direction, w = hair fresnel power
  float4 ch_key;    // rgb = key (sun) color, w = exposure
  float4 ch_amb;    // rgb = flat ambient, w = SH ambient multiplier / hair ambient
  float4 ch_sh[9];  // SH irradiance rows, pre-scaled (see capture);
                    // vehicles keep spec color + power in row 0 instead
  float4 ch_tintA;  // CAC diffuse tint / livingworld red-mask tint (w = apply)
  float4 ch_tintB;  // livingworld blue-mask tint / hair fresnel tint (w = strand-alpha scale)
  float4 ch_misc;   // x = alpha out, y = family
};
Texture2D<float4> diffuse : register(t0);
Texture2D<float4> lightmap : register(t1);
Texture2D<float4> macro : register(t3);
Texture2D<float4> decal_art : register(t4);
Texture2D<float2> shadow_atlas : register(t5);
TextureCube<float4> env_cube : register(t6);
// Raw bone palette: 3 float4 rows per bone, column-vector affine [R | t],
// applied with explicit dots (StructuredBuffer<float4x4> default packing is
// column-major and would silently transpose the matrices).
StructuredBuffer<float4> bones : register(t2);
SamplerState smp : register(s0);
// s1 = bilinear CLAMP (shared with the 2D pass). Decal art must clamp: the
// art UV runs far outside [0,1] across big ground sheets and the art's
// transparent border keeps everything outside the single placement clear;
// wrap sampling tiled the graffiti across the whole plaza.
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float3 rpos : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
  float3 nrm : TEXCOORD3;
  float2 uv3 : TEXCOORD4;
};
VSOut vs_main(float3 p : POSITION, float2 uv : TEXCOORD0, float2 uv2 : TEXCOORD1,
              float4 bw : BLENDWEIGHT0, uint4 bi : BLENDINDICES0,
              float3 nrm : NORMAL0, float2 uv3 : TEXCOORD2) {
  VSOut o;
  float4 mp = float4(p, 1.0);
  float3 n = nrm;
  // tint.g > 0 marks a skinned item: the bone palette (row-vector matrices)
  // maps model space to world space; mvp is then just view*proj.
  float wsum = dot(bw, float4(1, 1, 1, 1));
  if (tint.g > 0.0 && wsum > 0.001) {
    float3 skinned = float3(0, 0, 0);
    float3 sn = float3(0, 0, 0);
    // Guest blend indices are plain bone numbers (verified live: byte
    // streams like 02 00 03 01); bone k = palette rows 3k..3k+2.
    [unroll] for (int k = 0; k < 4; ++k) {
      uint row = bi[k] * 3u;
      skinned += bw[k] * float3(dot(mp, bones[row]), dot(mp, bones[row + 1]),
                                dot(mp, bones[row + 2]));
      sn += bw[k] * float3(dot(n, bones[row].xyz), dot(n, bones[row + 1].xyz),
                           dot(n, bones[row + 2].xyz));
    }
    mp = float4(skinned / wsum, 1.0);
    n = sn;
  } else {
    n = mul(n, (float3x3)world);
  }
  o.pos = mul(mp, mvp);
  o.rpos = mul(mp, world).xyz - cam_pos.xyz;
  o.uv = uv;
  o.uv2 = uv2;
  o.nrm = n;
  o.uv3 = uv3;
  return o;
}
// Native CSM atlas sample at a world position: finest covering cascade,
// s = saturate(infront + 1 - coverage). Returns 1 (lit) when uncovered or
// shadows are off. extra_bias suppresses receiver self-shadow acne on
// surfaces that are themselves casters (characters, held board). A NEGATIVE
// extra_bias selects the game's dynamicobject receive bias instead: a
// per-cascade literal 0.007 (finest) / 0.015 (outer) replacing sh_misc.x
// (from the dynamicobject_defaultPS ucode). Props
// are casters themselves, and with only the world bias (c5.x = 0 in every
// capture) their flat tops compared against their own atlas depth; the
// lit/dark whole-surface flicker on benches, signs and sails as the
// camera-following cascades drifted frame to frame.
float SampleCsmShadow(float3 wp, float extra_bias) {
  if (sh_misc.y <= 0.0) {
    return 1.0;
  }
  float2 lsv = float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
  float2 luv = 0.0;
  float casc = 0.0;
  float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
  if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
  float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
  if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
  if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
  if (casc <= 0.0) {
    return 1.0;
  }
  float bias = extra_bias < 0.0 ? (casc > 1.5 ? 0.015 : 0.007)
                                : sh_misc.x + extra_bias;
  float rd = dot(sh_z.xyz, wp) + sh_z.w - bias;
  float2 suv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0,
                      luv.y * -0.5 + 0.5);
  float2 sm2 = shadow_atlas.Sample(smp_clamp, suv);
  return saturate((sm2.x >= rd ? 1.0 : 0.0) + (1.0 - sm2.y));
}
float4 ps_main(VSOut i) : SV_Target {
  if (tint.a > 0.0) {
    return tint;
  }
  float4 albedo = diffuse.Sample(smp, i.uv);
  // Alpha-tested foliage/fences; opaque formats sample alpha = 1. Character
  // diffuse packs GLOSS in alpha; never clip characters. tint.g > 0 marks
  // them: set for bones-bound skinned items AND for ropa cloth garments
  // rendered rigid (sim-active player tees, clipping their gloss alpha
  // discarded every pixel: the invisible-shirt bug; their decode writes
  // zero blend weights, so the VS skinning branch stays off).
  if (tint.g == 0.0 && overlay.w < 0.5 && cam_pos.w > -20.5) {
    // environment.transparent alpha-tests its SQUARED alpha at ref 16/255
    // (transparentenvironment.xml: ALPHAREF 16, PS outputs a = diffuse.a^2).
    // Exact env families (cam_pos.w < 0) use the game's world ALPHAREF 30.
    // dynamicobject items (cam_pos.w <= -21) are excluded here and clip in
    // their own branch (only the .alphatest variant tests, at ALPHAREF 30).
    float aref = cam_pos.w < -0.5 ? 0.1176 : 0.35;
    clip(misc.x > 0.0 ? albedo.a * albedo.a - 0.0627 : albedo.a - aref);
  }
  // dynamicobject.fx props (cam_pos.w = -(20 + variant): -21 default,
  // -22 alphatest). Rigid movable objects (dispensers, dumpsters, benches,
  // cans). Lit with the game's own dynamicobject model (verified exact):
  // key sun light + bounce + flat ambient, gated by
  // the CSM shadow (the static world-shadow map is approximated as fully lit
  // - its floor c8.w bounds it), then fog -> exposure -> tonemap -> sqrt and
  // the postfx uber 1.41. v1 uses the geometric normal (cross of the mesh
  // tangent/binormal) with the flat normal-map kd 0.93429; the base/detail/
  // spec normal maps are v2.
  if (cam_pos.w < -20.5) {
    if (cam_pos.w < -21.5) {
      clip(albedo.a - 0.1176);  // dynamicobject.alphatest: ALPHAREF 30
    }
    float3 dlin = albedo.rgb * albedo.rgb;
    float3 n = dot(i.nrm, i.nrm) > 0.01
                   ? normalize(i.nrm)
                   : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float ndl = dot(n, dyn_sun.xyz);
    float key = saturate(ndl);  // key light gated on N.L >= 0
    float bounce = saturate(dot(n, float3(-dyn_sun.x, dyn_sun.y, -dyn_sun.z)));
    // CSM shadow (shared receiver rows at t5); the static world-shadow map is
    // not rendered natively, so its term is 1 and the min collapses to the
    // CSM (bounded below by the c8.w floor, dyn_misc.y). extra_bias -1 =
    // the game's own per-cascade dynamicobject receive bias (props are
    // casters; without it their flat tops self-shadow and flicker).
    float s = SampleCsmShadow(i.rpos + cam_pos.xyz, -1.0);
    float shadow = min(s * (ndl >= 0.0 ? 1.0 : 0.0), max(1.0, dyn_misc.y));
    float3 lighting = key * shadow + bounce * dyn_amb.w + dyn_amb.rgb;
    // GetTangentLight with the neutral (flat) normal map: 0.39 * 2.39562.
    float3 lin = lighting * 0.93429 * dlin;
    // Fog -> exposure -> tonemap -> sqrt, then the 1.41 uber scene multiplier.
    float fdist = length(i.rpos);
    float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    float3 fog_rgb = sh_fogc.rgb * f1;
    float fog_a = (1.0 + sh_fogc.a * f1) * dyn_misc.x;  // x material multiplier
    float3 xe = (lin * fog_a + fog_rgb) * dyn_sun.w;
    float3 t1 = saturate(1.0 - xe);
    float3 tm = max(xe * 0.25 + 0.75, 1.0) - t1 * t1;
    return float4(saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41), 1.0);
  }
  // Character families: the game's own lighting in LINEAR space (diffuse is
  // gamma -> square it), then the exact tone chain from the disassembly and
  // the postfx uber's 1.41 scene multiplier (which the empirical world
  // shading already folds into its constants; without it characters sit
  // ~30% darker than their surroundings, measured on an F11 A/B pair).
  if (cam_pos.w > 0.5) {
    float fam = cam_pos.w;
    float3 dlin = albedo.rgb * albedo.rgb;
    float3 cn = dot(i.nrm, i.nrm) > 0.01
                    ? normalize(i.nrm)
                    : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float ndl = saturate(dot(cn, ch_light.xyz));
    float3 vd = -normalize(i.rpos);
    float3 lin;
    float out_a = 1.0;
    if (fam > 5.5) {
      // Traffic vehicles (vehicle.fx fam 6 body / vehicle_glass.fx fam 7
      // windows, disassembled from vehicle_defaultPS): paint recolor where
      // the diffuse green channel is below the mask threshold (red-channel
      // mask * colorize_red + blue-channel mask * colorize_blue, the taxi
      // yellow), key light + the livingworld flat ambient, phong specular
      // along the reflected sun, and an environment-cube reflection scaled
      // by fresnel and the gloss packed in the diffuse alpha. Glass keeps
      // only the reflection terms (its tint rows are zero) and blends at
      // the captured alpha. overlay.y > 0 = the material's cube resolved
      // at t6 (same convention as water).
      float3 sel;
      if (fam < 6.5) {
        sel = dlin.g > 0.001225
                  ? dlin
                  : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
      } else {
        sel = ch_tintA.rgb;
      }
      // DXN panel normal map (the material's `normal` channel, riding the
      // macro slot; overlay.z > 0 = resolved). The vertex layout carries no
      // tangent frame, so build a screen-space cotangent frame from the
      // position/uv derivatives; it reproduces the authored panel shading
      // including mirrored UV islands. Skipping the map entirely shades the
      // hinged panels by their vertex normals, which face away from the sun
      // - the dark ambient-blue "misdrawn shadow" that stopped at the door
      // seam (verified against the ucode: flat map = the artifact, real
      // map = the emulated car).
      float3 vn = cn;
      if (overlay.z > 0.5) {
        float2 nm = macro.Sample(smp, i.uv).rg * 2.0 - 1.0;
        float3 dp1 = ddx(i.rpos), dp2 = ddy(i.rpos);
        float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
        float3 dp2p = cross(dp2, cn), dp1p = cross(cn, dp1);
        float3 tt = dp2p * du1.x + dp1p * du2.x;
        float3 bb = dp2p * du1.y + dp1p * du2.y;
        float im = rsqrt(max(max(dot(tt, tt), dot(bb, bb)), 1e-12));
        vn = normalize(nm.x * tt * im + nm.y * bb * im +
                       cn * sqrt(saturate(1.0 - dot(nm, nm))));
      }
      float vndl = saturate(dot(vn, ch_light.xyz));
      float3 rfl = ch_light.xyz - 2.0 * dot(vn, ch_light.xyz) * vn;
      // The sun spec is gated on N.L >= 0 (the ucode multiplies the spec
      // term by an sge result); the cube reflection is not.
      float spec = pow(saturate(dot(vd, -rfl)), max(ch_sh[0].w, 1.0)) *
                   (dot(vn, ch_light.xyz) >= 0.0 ? 1.0 : 0.0);
      float fres = pow(1.0 - saturate(dot(vn, vd)), max(ch_light.w, 1.0));
      float3 cube = float3(0.0, 0.0, 0.0);
      if (overlay.y > 0.5) {
        cube = env_cube.Sample(smp, reflect(-vd, vn)).rgb;
        cube *= cube;  // the PS consumes the cube squared (linear space)
      }
      // Gloss = the diffuse alpha SQUARED: the ucode squares the whole
      // diffuse fetch (linear-space decode), alpha included; raw alpha
      // over-specs ~5x and mottles the body panels.
      float gloss = fam < 6.5 ? albedo.a * albedo.a : 1.0;
      lin = sel * (ch_key.rgb * vndl + ch_amb.rgb) +
            (spec * ch_sh[0].rgb + cube) * fres * gloss;
      out_a = ch_misc.x;
    } else if (fam > 3.5) {
      // Hair (cac_hair / defaulthair): key on a wrapped N.L ramp + flat
      // ambient, fresnel rim tint on a steeper ramp; strand coverage from
      // the mesh's "alpha" channel at the raw second texcoord (bound at t4)
      // - alpha-blended in the sorted sub-pass (hair drawn opaque is the
      // blocky-helmet look).
      float fres = pow(1.0 - saturate(dot(cn, vd)), max(ch_light.w, 1.0));
      float3 hl = ch_key.rgb * (saturate(ndl * 0.75 + 0.25) + ch_amb.w) +
                  ch_tintB.rgb * fres * saturate(ndl * 1.75 + 0.25);
      lin = dlin * hl;
      out_a = saturate(decal_art.Sample(smp, i.uv2).r * ch_tintB.w);
    } else if (fam > 2.5) {
      // livingworld pedestrians: the diffuse is a stamp-mask atlas: red
      // regions recolor with tintA, blue with tintB (judged in linear
      // space; real-color regions have green above the threshold).
      // The game's character PSes multiply the key light by the CSM shadow
      // (tap >= ray = lit); characters are casters themselves, so an extra
      // receiver bias suppresses self-shadow acne while the body-onto-board
      // / body-onto-NPC shading survives (the sun-axis depth gap there is
      // tens of cm). Without this the held skateboard, a big flat surface
      // that is almost always inside the skater's own shadow, renders
      // fully sunlit (near-white) against the emulated dark deck.
      float csm = SampleCsmShadow(i.rpos + cam_pos.xyz, 0.012);
      float3 sel = dlin.g > 0.001225
                       ? dlin
                       : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
      lin = sel * (ch_key.rgb * ndl * csm + ch_amb.rgb);
    } else {
      // defaultcharacter / CAC pieces: key light + SH irradiance ambient,
      // key gated by the CSM shadow (see the livingworld comment above).
      float csm = SampleCsmShadow(i.rpos + cam_pos.xyz, 0.012);
      if (ch_tintA.w > 0.0) {
        dlin *= ch_tintA.rgb;
      }
      float3 irr = saturate(
          ch_sh[0].rgb + cn.x * ch_sh[1].rgb + cn.y * ch_sh[2].rgb +
          cn.z * ch_sh[3].rgb + (cn.x * cn.z) * ch_sh[4].rgb +
          (cn.z * cn.y) * ch_sh[5].rgb + (cn.y * cn.x) * ch_sh[6].rgb +
          (cn.z * cn.z) * ch_sh[7].rgb +
          (cn.x * cn.x - cn.y * cn.y) * ch_sh[8].rgb);
      lin = dlin * (ch_key.rgb * ndl * csm + irr * ch_amb.w);
    }
    // Exact tone chain: sqrt(0.5 * (max(x*E/4 + 0.75, 1) - sat(1 - x*E)^2)).
    float E = max(ch_key.w, 0.01);
    float3 t1 = saturate(1.0 - lin * E);
    float3 tm = max(lin * 0.25 * E + 0.75, 1.0) - t1 * t1;
    float3 cc = saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41);
    return float4(cc, out_a);
  }
  // Exact world-material families (cam_pos.w = -family): hand-ported from
  // the game's own pixel shaders and verified per-pixel against them with
  // an offline ucode interpreter. All texture
  // colors linearize IN-SHADER as x^2 (the fetch signs are unsigned on every
  // world texture). Families: 1 baseenvironment, 2 defaultenvironment,
  // 3/4 decalenvironment(_tileable), 5/6 reflective(_simple), 7 alphatest,
  // 8 environmentdiffuse, 9/10 tree(animate), 11 proxyworld,
  // 12 incandescent. v1 runs with NEUTRAL normal/detail maps (kd is the
  // exact flat-map constant 0.39 * 2.39562); spec/reflection masks bind at
  // t4 (overlay.w == 3) on families without decal art.
  if (cam_pos.w < -0.5) {
    float fam = -cam_pos.w;
    float3 dlin = albedo.rgb * albedo.rgb;
    // Global distance fog (VS c5/c6, captured per frame): every world PS
    // ends with col * fog.a + fog.rgb before exposure/tonemap.
    float fdist = length(i.rpos);
    float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    float3 fog_rgb = sh_fogc.rgb * f1;
    float fog_a = 1.0 + sh_fogc.a * f1;
    float expo = sh_sun.w;
    float3 lin;
    float out_a = 1.0;
    bool reduced_tone = false;
    if (fam > 8.5) {
      // tree/treeanimate: D^2 * max(lm^2, floor) * scale [* tint mult];
      // proxyworld/incandescent: D^2 * scale. No shadow receive, no kd, no
      // material multiplier on the fog term.
      if (fam < 10.5) {
        float3 lmg = lightmap.Sample(smp, i.uv2).rgb;
        lin = dlin * max(lmg * lmg, sh_env.z) * sh_env.y;
        if (fam < 9.5) {
          lin *= sh_env.w;
        }
        out_a = albedo.a;
      } else {
        lin = dlin * (fam < 11.5 ? sh_fogp.w : 1.0);
      }
    } else {
      // Environment families: macro overlay (0.5-neutral, fades under decal
      // art), linear decal composite, lightmap squared and min-clamped
      // against (CSM s + shadow color), kd, phong spec vs the shader's
      // fixed literal light, cube reflection on 5/6.
      float3 ov = float3(1.0, 1.0, 1.0);
      if (overlay.z > 0.0) {
        float3 mo = macro.Sample(smp, i.uv * overlay.x).rgb;
        ov = saturate((mo - 0.5) * overlay.y + 0.5);
      }
      if (fam > 2.5 && fam < 4.5 && overlay.w > 0.5) {
        // overlay.w == 0 = art unresolved (white fallback alpha 1 would
        // whitewash the whole surface).
        float4 dk = overlay.w > 1.5 ? decal_art.Sample(smp, i.uv3)
                                    : decal_art.Sample(smp_clamp, i.uv3);
        dlin = lerp(dlin, dk.rgb * dk.rgb, dk.a);
        ov = lerp(float3(1.0, 1.0, 1.0), ov, 1.0 - dk.a);
      }
      float3 dcol = dlin * ov;
      // CSM shadow term s = sat(infront + 1 - coverage) from the native
      // atlas (same cascade select as the legacy receive path).
      float s = 1.0;
      if (sh_misc.y > 0.0) {
        float3 wp = i.rpos + cam_pos.xyz;
        float2 lsv =
            float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
        float rd = dot(sh_z.xyz, wp) + sh_z.w - sh_misc.x;
        float2 luv = 0.0;
        float casc = 0.0;
        float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
        if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
        float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
        if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
        if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
        if (casc > 0.0) {
          float2 suv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0,
                              luv.y * -0.5 + 0.5);
          float2 sm2 = shadow_atlas.Sample(smp_clamp, suv);
          s = saturate((sm2.x >= rd ? 1.0 : 0.0) + (1.0 - sm2.y));
        }
      }
      float3 lmg = lightmap.Sample(smp, i.uv2).rgb;
      float3 lml = min(lmg * lmg, s + sh_color.rgb);
      // GetTangentLight with the neutral (flat) normal map:
      // 0.39 * 2.39562 exactly.
      lin = lml * 0.93429 * dcol;
      if (overlay.w > 2.5) {
        // spec/ecc/refmask at t4: phong vs the FIXED literal light
        // (-0.14, 0.5, 0.9), power 10 + 290*ecc, tint (2.1, 1.8, 1.5),
        // scaled by the clamped lightmap green and the spec mask.
        float4 masks = decal_art.Sample(smp, i.uv);
        float3 wn = dot(i.nrm, i.nrm) > 0.01
                        ? normalize(i.nrm)
                        : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
        float3 vd = -normalize(i.rpos);
        float3 Ls = float3(-0.14, 0.5, 0.9);
        float3 refl = Ls - 2.0 * wn * dot(wn, Ls);
        float bp = saturate(dot(vd, -refl));
        float ks = pow(max(bp, 1e-6), 10.0 + 290.0 * masks.y);
        lin += ks * float3(2.1, 1.8, 1.5) * lml.g * masks.x;
        if (fam > 4.5 && fam < 6.5) {
          // Cube reflection: reflect(E, wN) with xy negated (the source's
          // ref_vec.xy *= -1), luminosity lerped toward 1 by
          // 0.3 * sat(4*refmask - 2.6), x refmask x reflection_scale 1.5.
          float3 rv = vd - 2.0 * wn * dot(vd, wn);
          float3 cube = env_cube.Sample(smp, float3(-rv.x, -rv.y, rv.z)).rgb;
          float rl = 0.3 * saturate(4.0 * masks.z - 2.6);
          float lum = lml.g + rl * (1.0 - lml.g);
          lin += cube * lum * masks.z * 1.5;
        }
      }
      if (fam > 6.5) {
        out_a = albedo.a;
        reduced_tone = fam > 7.5;  // environmentdiffuse's cheap tonemap
      }
      fog_a *= sh_env.x;  // material multiplier (PS c11.y)
    }
    // Fog -> exposure -> tonemap -> sqrt, then the postfx uber's measured
    // 1.41 scene multiplier (same as the character branch).
    float3 xe = (lin * fog_a + fog_rgb) * expo;
    float3 t1e = saturate(1.0 - xe);
    float3 tme = reduced_tone ? 1.0 - t1e * t1e
                              : max(xe * 0.25 + 0.75, 1.0) - t1e * t1e;
    float3 cce = saturate(sqrt(max(tme * 0.5, 0.0)) * 1.41);
    return float4(cce, out_a);
  }
  // Macro overlay: large-scale grime/cracks multiplied over the diffuse at
  // uv * macroOverlayUVScale, faded by macroOverlayOpacity: the ground and
  // wall weathering. WHITE is the neutral (materials without weathering
  // bind a 16x16 "default_white"). The game multiplies it ONCE in its
  // linear (squared) color space, so the gamma-space equivalent is
  // sqrt(m); a direct multiply doubles the darkening (harsh black
  // patchwork vs the emulated subtle weathering).
  if (overlay.z > 0.0 && misc.x < 1.5) {  // water reuses overlay.z (ripple map flag)
    float4 m = macro.Sample(smp, i.uv * overlay.x);
    albedo.rgb *= lerp(float3(1.0, 1.0, 1.0), sqrt(m.rgb), overlay.y * m.a);
  }
  // environment.decal surfaces: the paint/graffiti art (t4) is composited
  // over the base diffuse by ITS alpha, opaque output; these meshes ARE
  // the wall/ground there. The art maps with uv3, the second half-pair of
  // the packed half4 first texcoord (validated offline: sampling with the
  // tiling uv0 repeats it: "Stereo Stereo Stereo"; the fmt-26 second
  // element is the lightmap unwrap, not the decal's).
  if (overlay.w > 0.0) {
    // overlay.w == 2 marks environment.decal_tileable: the art tiles across
    // the surface (rock/cliff faces) and must WRAP; clamp stretched the
    // border texels into giant streaks. Single placements clamp (their
    // transparent border keeps everything outside the placement clear).
    float4 dk = overlay.w > 1.5 ? decal_art.Sample(smp, i.uv3)
                                : decal_art.Sample(smp_clamp, i.uv3);
    albedo.rgb = lerp(albedo.rgb, dk.rgb, dk.a);
  }
  // tint.r > 0 marks items with a lightmap bound (2x baked lighting);
  // otherwise fall back to derivative face shading. The lighting term stays
  // separate from the albedo so the CSM receive below can min-clamp IT, the
  // way the game's GetShadowedLightMap clamps the lightmap lighting.
  float3 light;
  if (tint.b > 0.0) {
    light = float3(1.0, 1.0, 1.0);  // unlit (sky dome)
  } else if (tint.r > 0.0) {
    light = lightmap.Sample(smp, i.uv2).rgb * 2.0;
  } else {
    // Smooth per-vertex normal when the mesh has one; face normal from
    // position derivatives otherwise.
    float3 n = dot(i.nrm, i.nrm) > 0.01 ? normalize(i.nrm)
                                        : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    light = abs(dot(n, normalize(float3(0.4, 0.8, 0.3)))) * 0.35 + 0.75;
  }
  // Dynamic CSM shadow receive (world geometry + rigid props; characters
  // need the game's separate PCF/bias variant; skipping them avoids
  // self-shadow acne, and the ground shadow is 95% of the visible effect).
  // Exact receiver math from the baseenvironment PS disassembly: finest
  // cascade whose |ls| < 0.99 wins; shadow = saturate(infront + 1 -
  // coverage), then the game min-clamps the LINEAR lighting term:
  //   light_linear = min(light_linear, s + c8.rgb)
  // Full shadow clamps to the dim bluish c8 ambient, the penumbra saturates
  // wherever the clamp exceeds the lit level (which is what keeps the edge
  // crisp), and surfaces already darker than the clamp, baked shade under
  // bridges/trees, show NO dynamic shadow at all. Our light term is
  // gamma-space (light^2 ~ the game's linear term: the lightmap x2 folds
  // its x4 linear multiplier), so the clamp maps to min(light, sqrt(s+c8))
  // per channel. A fixed-denominator curve here read pitch-black and
  // double-darkened baked shade.
  if (sh_misc.y > 0.0 && tint.g == 0.0 && tint.b == 0.0 && misc.x == 0.0) {
    float3 wp = i.rpos + cam_pos.xyz;
    float2 lsv = float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
    float rd = dot(sh_z.xyz, wp) + sh_z.w - sh_misc.x;
    float2 luv = 0.0;
    float casc = 0.0;
    float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
    if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
    float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
    if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
    if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
    if (casc > 0.0) {
      float2 uv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0, luv.y * -0.5 + 0.5);
      float2 m = shadow_atlas.Sample(smp_clamp, uv);
      float s = saturate((m.x >= rd ? 1.0 : 0.0) + (1.0 - m.y));
      light = min(light, sqrt(s + sh_color.rgb));
    }
  }
  float3 lit = albedo.rgb * light;
  if (mat_tint.w > 0.0 && misc.x == 0.0) {
    lit *= mat_tint.rgb;
  }
  if (misc.x > 1.5) {
    // water.* (flowingwater.fx approximation): the real shader is
    // near-black diffuse + dual time-scrolled ripple-normal taps + an
    // environment-cube reflection + sun specular. We have no cube map
    // bound, so the reflection term is the frame fog color (the best
    // single approximation of the surroundings' haze tone) scaled by a
    // fresnel curve; the ripple normal perturbs both the fresnel and a sun
    // sparkle along the captured CSM light axis. The lightmap (x2) keeps
    // the baked bridge/wall shading on the surface. Calibrated against the
    // canal capture (emulated mid-canal mean ~(24,28,32)/255).
    float t = overlay.x;
    float2 rip;
    if (overlay.z > 0.0) {
      // Dual scrolled taps of the material's ripple normal map (macro slot).
      float2 wuv = i.uv * 6.0;
      float3 n1 = macro.Sample(smp, wuv + t * float2(0.11, 0.06)).rgb;
      float3 n2 = macro.Sample(smp, wuv * 1.71 - t * float2(0.07, 0.13)).rgb;
      rip = (n1.xy + n2.xy) - 1.0;
    } else {
      // Normal map unresolved: procedural ripples from world position.
      // Wavelengths ~0.4-1m (emulated ripples are decimeter-scale); low
      // frequencies formed meter-wide chevron interference bands that read
      // as giant arrows on the surface.
      float3 wp = i.rpos + cam_pos.xyz;
      rip = 0.35 * float2(sin(wp.x * 9.7 + wp.z * 6.1 + t * 2.3) +
                              0.6 * sin(wp.x * 17.3 - wp.z * 11.9 + t * 3.4),
                          cos(wp.x * 7.1 - wp.z * 13.7 + t * 2.7) +
                              0.6 * cos(wp.x * 12.9 + wp.z * 18.3 + t * 3.1));
    }
    float3 wn = normalize(float3(rip.x * 0.4, 2.0, rip.y * 0.4));
    float3 vd = -normalize(i.rpos);
    float fres = pow(1.0 - saturate(dot(vd, wn)), 3.0);
    // The flowing-water lightmap unwrap decodes unreliably (bands across
    // atlas gutters), so the water term deliberately ignores it: near-black
    // body + ripple-perturbed cube reflection + sun sparkle.
    // Deep body: the water "diffuse" is a faint STRIPE MASK (max 24/255,
    // WaterFallFoamAlpha, a lookup for the real shader, not a color). The
    // game consumes it in linear space where 0.09^2 vanishes; squaring here
    // likewise kills the visible blue/black banding. overlay.w > 0 = no
    // diffuse channel at all (ocean.default); body is zero there, NOT the
    // white fallback (ocean.fx: diffTerm = (0,0,0), color is all reflection).
    float3 col = overlay.w > 0.5 ? 0.0 : albedo.rgb * albedo.rgb * 0.6;
    // Reflection tint: the environment cube when resolved (t6); otherwise a
    // haze derived from the frame fog color, lifted toward neutral so dark
    // dusk fog doesn't collapse the water to black (fit: emulated canal
    // mean ~(24,28,32)/255 with fog color (0.02,0.07,0.13)).
    float3 renv = overlay.y > 0.0
                      ? env_cube.Sample(smp, reflect(-vd, wn)).rgb
                      : mat_tint.rgb * 0.5 + 0.06;
    col += renv * (0.55 + 0.45 * fres);
    if (sh_misc.y > 0.0) {
      float3 h = normalize(vd + normalize(-sh_z.xyz));
      col += pow(saturate(dot(wn, h)), 90.0) * 0.35;            // sun sparkle
    }
    float fade = saturate(length(i.rpos) * misc.y + misc.z);
    if (misc.w != 1.0) {
      fade = pow(max(fade, 1e-4), misc.w);
    }
    col = sqrt(max(col * col * saturate(1.0 + fade * mat_tint.w) +
                   fade * mat_tint.rgb, 0.0));
    // Opaque: the game's murk hides the canal bed entirely (and our bed
    // shading is untrustworthy under water: striped lightmap unwraps).
    return float4(col, 1.0);
  }
  if (misc.x > 0.0) {
    // transparentenvironment.fx (Skate 2 source; disassembled Skate 3 PS
    // matches): outcolor.rgb = diffTerm * diffuse.rgb * diffuse.a; the rgb
    // is premultiplied by alpha ONCE IN THE SHADER on top of the a^2 blend
    // factor, so wisps thin out as ~a^3. That cubic falloff is most of the
    // emulated "sparse clouds" look. Fog is applied in the game's linear
    // (squared) color space: fade = saturate(dist * ramp.x + ramp.y)^ramp.z
    // toward the fog color, transmittance = 1 + fade * fogcolor.w.
    lit *= albedo.a;
    float fade = saturate(length(i.rpos) * misc.y + misc.z);
    if (misc.w != 1.0) {
      fade = pow(max(fade, 1e-4), misc.w);
    }
    lit = sqrt(max(lit * lit * saturate(1.0 + fade * mat_tint.w) +
                   fade * mat_tint.rgb, 0.0));
    return float4(lit, albedo.a * albedo.a);
  }
  return float4(lit, 1.0);
}
// Shadow caster pass: vs_main runs with mvp = (world *) lightVP built from
// the captured receiver rows, so SV_Position.z IS the light-space depth
// (the height-ramp row; viewport z range 0..1, DepthClip off so casters
// outside the 12 m depth window clamp like the game accepts). MIN blend on
// both channels against a (1, 1) clear: R accumulates the min depth, G
// drops to 0 where any caster drew ("uncoverage"; the blur pass converts
// to the game's coverage convention).
float2 ps_shadow_caster(VSOut i) : SV_Target {
  return float2(i.pos.z, 0.0);
}
)";

// Shadow blur: the game's shadow_h/vblur passes, exact semantics from the
// disassembled Xenos ucode. One fullscreen-triangle draw
// per tile per direction; taps stay inside the tile (clamped). Coverage is
// Gaussian-blurred; depth keeps the exact center value where covered and
// dilates (binary-coverage-weighted neighbour average) into the penumbra
// otherwise, so depth compares stay valid across the blurred edge.
// b0 floats: c0 = (dir.x, dir.y, ntaps, src_is_raw), c1 = (w0, w1, w2, 0),
// c2 = (tile_x0, tile_x1, tile_y1, 0). ntaps 0 = format-convert only
// (cascade 2 is never blurred).
const char kShadowBlurSource[] = R"(
cbuffer C : register(b0) {
  float4 c0;
  float4 c1;
  float4 c2;
};
Texture2D<float2> src : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 ps_main(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy);
  int ntaps = int(c0.z);
  float w[3] = {c1.x, c1.y, c1.z};
  float2 center = src.Load(int3(p, 0));
  float ccen = c0.w > 0.0 ? 1.0 - center.y : center.y;
  float cov = 0.0;
  float dsum = 0.0;
  float dcnt = 0.0;
  [unroll] for (int k = -2; k <= 2; ++k) {
    if (abs(k) > ntaps) continue;
    int2 q = p + int2(c0.xy) * k;
    q.x = clamp(q.x, int(c2.x), int(c2.y));
    q.y = clamp(q.y, 0, int(c2.z));
    float2 t = src.Load(int3(q, 0));
    float c = c0.w > 0.0 ? 1.0 - t.y : t.y;
    cov += w[abs(k)] * c;
    if (k != 0) {
      float hard = c > 0.001 ? 1.0 : 0.0;
      dcnt += hard;
      dsum += hard * t.x;
    }
  }
  float depth = ccen > 0.001 ? center.x : (dcnt > 0.0 ? dsum / dcnt : center.x);
  return float2(depth, saturate(cov));
}
)";

// Fullscreen MSAA resolve: the deferred command list has no
// ResolveSubresource, so average the samples in a pixel shader (SAMPLES is
// injected as a compile-time macro).
const char kResolveShaderSource[] = R"(
Texture2DMS<float4> src : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy);
  float4 c = 0;
  [unroll] for (int k = 0; k < SAMPLES; ++k) {
    c += src.Load(p, k);
  }
  return c / SAMPLES;
}
)";

// Popup background blur: EXACT port of the game's dedicated pass chain
// (captured from a live frame with the Rewards popup up):
//   blur_hBlurPS:  11 gaussian taps along +X over the finished frame,
//                  offsets k * 0.0003125 * PS c0.x (c0.x = 8 -> 0.0025/tap,
//                  +/-1.25% of the buffer), literal weights below (sum 1);
//   blur_vBlurPS:  the same kernel along +Y;
//   postfx_basictex: plain fullscreen REPLACE of the frame with the result.
// The game runs it at its fixed 1152x640 internal resolution and the
// console's bilinear upscale of that buffer is what reads as the "frosted
// glass" lattice, so the passes here render into 1152x640 intermediates and
// stretch back, reproducing both the kernel and the lattice.
const char kBlurShaderSource[] = R"(
cbuffer C : register(b0) {
  float4 dir;  // xy = blur axis, z = kernel scale (the game's PS c0.x, 8)
};
Texture2D<float4> src : register(t0);
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  // Literal kernel from the blur_hBlurPS ucode (c251..c255).
  const float w[6] = {0.2005654, 0.1769984, 0.1216491, 0.0651141,
                      0.0271436, 0.0088122};
  float2 step = dir.xy * (0.0003125 * dir.z);
  float3 c = src.SampleLevel(smp_clamp, i.uv, 0).rgb * w[0];
  [unroll] for (int k = 1; k < 6; ++k) {
    c += src.SampleLevel(smp_clamp, i.uv + step * k, 0).rgb * w[k];
    c += src.SampleLevel(smp_clamp, i.uv - step * k, 0).rgb * w[k];
  }
  return float4(c, 1.0);
}
// Prefiltered downsample of the (higher-res) native output into the game's
// 1152x640 blur space (dir.xy = source texel size). A 4x4 grid of bilinear
// taps covers reduction ratios up to ~4x (4K -> 1152 is 3.33x); narrower
// footprints undersampled the scale and the aliased detail crawled as the
// scene animated: the whole blurred backdrop shimmered, worst around
// high-contrast edges.
float4 ps_down(VSOut i) : SV_Target {
  float3 c = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      float2 off = float2(float(x) - 1.5, float(y) - 1.5) * dir.xy;
      c += src.SampleLevel(smp_clamp, i.uv + off, 0).rgb;
    }
  }
  return float4(c / 16.0, 1.0);
}
// postfx_basictex: oC0 = tex (verified: `max oC0, r0, r0`).
float4 ps_blit(VSOut i) : SV_Target {
  return float4(src.SampleLevel(smp_clamp, i.uv, 0).rgb, 1.0);
}
)";

// Selected-object outline composite (park editor / object mover): port of
// postfx_edgedetectstencilPS. The game stencil-marks the selected object,
// resolves stencil to a 576x320 texture and adds |threshold crossings| x
// the outline color (PS c0, the editor blue) onto the frame; our mask pass
// renders the selected items into a small R8 target instead (see
// pso_outline_mask) and this pass composites additively over the resolved
// output. Tap offsets are 1/576 x 1/320 UV fractions like the original, so
// the contour thickness matches at any output resolution.
const char kOutlineShaderSource[] = R"(
cbuffer C : register(b0) {
  float4 color;  // rgb = outline color (guest edge-detect PS c0)
};
Texture2D<float4> src : register(t0);
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  // Averaged 5x5 neighborhood of the (binary, full-resolution) mask with a
  // fixed UV-fraction pitch -> a smooth 0..1 ramp across the silhouette
  // boundary; the peaked profile 4m(1-m) turns the ramp into an
  // antialiased line of constant screen-fraction width. The core is
  // whitened (the emulated line goes through the game's uber grade +
  // bloom, which lifts its center toward white). A literal port:
  // binarized crossing counts on the 576x320 grid, stairstepped visibly
  // at native output resolutions.
  const float2 t = float2(0.8 / 1152.0, 0.8 / 640.0);
  float m = 0.0;
  [unroll] for (int y = -2; y <= 2; ++y) {
    [unroll] for (int x = -2; x <= 2; ++x) {
      m += src.SampleLevel(smp_clamp, i.uv + float2(x, y) * t, 0).r;
    }
  }
  m /= 25.0;
  float band = saturate(6.4 * m * (1.0 - m));
  float core = band * band * band;
  // Additive blend (ONE, ONE) onto the frame; alpha untouched (ZERO, ONE).
  return float4(color.rgb * band + 0.5 * core, 0.0);
}
)";

// 2D/APT overlay shader. Verified against a captured HUD draw stream:
// vertices are {float4 pos, float2 uv} in 1280x720
// APT movie space; the game's VS constants apply as clip = ortho * (world *
// pos) with c0..c3 the ortho rows, c4..c7 the element's transform rows and
// c8 the color multiplier, used exactly as staged.
const char kShader2dSource[] = R"(
cbuffer C : register(b0) {
  float4 m[10];  // m[0..3] proj rows, m[4..7] world rows, m[8] color,
                 // m[9].x = apply D3D9 half-pixel (2D ortho draws only)
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};
VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0, float4 color : COLOR0) {
  float4 wp = float4(dot(p, m[4]), dot(p, m[5]), dot(p, m[6]), dot(p, m[7]));
  VSOut o;
  o.pos = float4(dot(wp, m[0]), dot(wp, m[1]), dot(wp, m[2]), dot(wp, m[3]));
  // D3D9 half-pixel convention: the art bakes half-texel UVs expecting
  // pixel centers at integer coordinates; without this the clock-face
  // quadrant tiles show their wrapped border rows as dark seam lines.
  // Scaled for the 2D ortho; must not apply to 3D (world-space
  // SimpleDraw markers), where m[0].x is a projection scale.
  if (m[9].x > 0.0) {
    o.pos.x -= 0.5 * m[0].x * o.pos.w;
    o.pos.y -= 0.5 * m[1].y * o.pos.w;
  }
  o.uv = uv;
  o.color = color;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  return tex.Sample(smp, i.uv) * m[8] * i.color;
}
)";

// In-world neon spline shader (waypoint arrows / marker beams). The guest
// B-spline VS is evaluated on the CPU at publish time, so the VS here is a
// clip-space passthrough; the two PS variants transcribe the game's own
// spline.fx (Skate-Shaders repo): "default" = additive glow with the
// squared-gamma trick, "darken" = straight-alpha backdrop dimming. The
// gradient texture uses the wrap/aniso sampler like the original i_diffuse
// (U runs 0..N along the band).
const char kShaderSplineSource[] = R"(
cbuffer C : register(b0) {
  float4 intensity;  // i_intensity as staged (x = default gain, y = darken gain)
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float fade : TEXCOORD1;
};
VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0, float fade : TEXCOORD1) {
  VSOut o;
  o.pos = p;
  o.uv = uv;
  o.fade = fade;
  return o;
}
// Exact transcription of the Skate 3 spline pixel shaders (disassembled
// ucode, NOT the older Skate 2 spline.fx source: EA added in-shader sqrt
// gamma compensation). default: oC0.rgb = sqrt(rgb^2 * i.x * fade / a);
// darken: oC0.rgb = sqrt(rgb^2 * i.y), oC0.a = sqrt(a * i.y * fade).
// Getting this wrong is visible: a linear 1/a over-brightens the low-alpha
// glow fringe (fuzzy, blown-out edges) and a linear darken alpha weakens the
// backdrop dimming that makes the neon read as solid.
float4 ps_default(VSOut i) : SV_Target {
  float4 c = tex.Sample(smp, i.uv);
  float3 sq = c.rgb * c.rgb * (intensity.x * i.fade / max(c.a, 1.0 / 255.0));
  return float4(sqrt(abs(sq)), 1.0);
}
float4 ps_darken(VSOut i) : SV_Target {
  float4 c = tex.Sample(smp, i.uv);
  float3 rgb = sqrt(abs(c.rgb * c.rgb * intensity.y));
  float a = sqrt(abs(c.a * intensity.y * i.fade));
  return float4(rgb, a);
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
// unorm4 blend weights, u8x4 blend indices, float3 normal, float2 decal_uv}
// (56-byte stride). decal_uv = the 3rd/4th halves of a half4 first texcoord
// (the packed second UV pair environment.decal art is mapped with), equal to
// the first pair when the element has only two halves.
// vb_payload/ib_payload: optional pre-copied guest payload snapshots (the
// dynamic cloth decode jobs snapshot on the guest thread and convert on a
// worker, see DynDecodeJob); when null the payloads are read live from
// guest memory with the guarded copy.
bool DecodeMesh(ID3D12Device* device, uint8_t* base, const DrawItem& item,
                MeshBuffers& out, const uint8_t* vb_payload = nullptr,
                const uint8_t* ib_payload = nullptr) {
  const uint32_t num_verts = item.vb_bytes / item.stride;
  if (num_verts == 0) return false;
  // This runs on the render thread; the guest payloads were valid on the
  // game thread this frame but streaming can decommit them in between.
  // Copy them out with the lock-free guarded copy (never VirtualQuery here:
  // the VAD lock stalls behind the guest streaming threads while panning).
  static thread_local std::vector<uint8_t> vb_scratch;
  static thread_local std::vector<uint8_t> ib_scratch;
  if (vb_payload == nullptr) {
    vb_scratch.resize(item.vb_bytes);
    if (!GuestTryCopy(vb_scratch.data(), base + item.vb_addr, item.vb_bytes)) {
      return false;
    }
    vb_payload = vb_scratch.data();
  }
  if (!item.cloth_quads && ib_payload == nullptr) {
    ib_scratch.resize(size_t(item.ib_count) * 2);
    if (!GuestTryCopy(ib_scratch.data(), base + item.ib_addr, size_t(item.ib_count) * 2)) {
      return false;
    }
    ib_payload = ib_scratch.data();
  }
  ID3D12Resource* vb = CreateUploadBuffer(device, size_t(num_verts) * 56);
  ID3D12Resource* ib = CreateUploadBuffer(device, size_t(item.ib_count) * 2);
  if (!vb || !ib) {
    if (vb) vb->Release();
    if (ib) ib->Release();
    return false;
  }

  // two_sided_sheet detection eligibility (see MeshBuffers): small static
  // triangle-list meshes only. Strips alternate winding per triangle and
  // skinned/cloth meshes deform, so both stay on the uncull(ed) PSO.
  bool detect_sheet =
      !item.skinned && !item.cloth_quads && item.ib_count >= 6 && item.ib_count <= 8192;
  for (const DrawEntry& de : item.draws) {
    if (de.prim != 4) {
      detect_sheet = false;
    }
  }
  std::vector<float> sheet_pos;
  if (detect_sheet) {
    sheet_pos.reserve(size_t(num_verts) * 3);
  }

  const uint8_t* src_vb = vb_payload;
  float* dst = nullptr;
  uint32_t garbage = 0;
  int min_bi = 255;
  int max_bi = -1;
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
        case 37:  // k_32_32_FLOAT (hair strand-alpha UV; xenos enum 37)
        case 38:  // k_32_32_32_32_FLOAT (use xy)
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
        case 26: {  // k_16_16_16_16 (snorm; use xy: unwrap UV sets)
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
    dst[v * 14 + 0] = x;
    dst[v * 14 + 1] = y;
    dst[v * 14 + 2] = z;
    if (detect_sheet) {
      sheet_pos.push_back(x);
      sheet_pos.push_back(y);
      sheet_pos.push_back(z);
    }
    decode_uv(item.uv_fmt, item.uv_offset, dst[v * 14 + 3], dst[v * 14 + 4]);
    decode_uv(item.uv2_fmt, item.uv2_offset, dst[v * 14 + 5], dst[v * 14 + 6]);
    // The second texcoord is the lightmap/decal UNWRAP, stored as an
    // absolute value with tangent handedness in the sign bits, in BOTH the
    // s16-snorm (fmt 26, decal meshes) and half-float (fmt 31, world tiles)
    // encodings (decalenvironment VS: o0.zw = |uv|; baseenvironment VS:
    // maxs r2.zw = |r4.xy|). Raw signed values sample mirrored atlas cells.
    // Exception: hair meshes, their second texcoord is the strand-alpha
    // UV, passed RAW by the hair VS (o0.zw = uv, float2).
    if (item.char_family < 4) {
      dst[v * 14 + 5] = std::fabs(dst[v * 14 + 5]);
      dst[v * 14 + 6] = std::fabs(dst[v * 14 + 6]);
    }
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
      for (int k = 0; k < 4; ++k) {
        const uint8_t weight = uint8_t(bw >> (k * 8));
        const uint8_t index = uint8_t(bi >> (k * 8));
        if (weight != 0) {
          if (index < min_bi) min_bi = index;
          if (index > max_bi) max_bi = index;
        }
      }
    }
    std::memcpy(&dst[v * 14 + 7], &bw, 4);
    std::memcpy(&dst[v * 14 + 8], &bi, 4);
    // Decal-art UV: second half pair of a half4 first texcoord, else uv0.
    if (item.uv_fmt == 32) {
      decode_uv(31, item.uv_offset + 4, dst[v * 14 + 12], dst[v * 14 + 13]);
    } else {
      dst[v * 14 + 12] = dst[v * 14 + 3];
      dst[v * 14 + 13] = dst[v * 14 + 4];
    }
    // Vertex normal: k_10_11_11 packed (x 11 bits, y 11 bits, z 10 bits,
    // LSB to MSB, signed). Zero when absent -> derivative face-normal
    // fallback in the shader.
    const auto unpack_10_11_11 = [&](uint32_t offset, float out[3]) {
      const uint32_t word = SwapU32(*reinterpret_cast<const uint32_t*>(
          src_vb + size_t(v) * item.stride + offset));
      const int32_t ix = int32_t(word << 21) >> 21;
      const int32_t iy = int32_t((word >> 11) << 21) >> 21;
      const int32_t iz = int32_t((word >> 22) << 22) >> 22;
      out[0] = float(ix) / 1023.0f;
      out[1] = float(iy) / 1023.0f;
      out[2] = float(iz) / 511.0f;
    };
    float n3[3] = {0.0f, 0.0f, 0.0f};
    if (item.env_family != 0 && item.env_family <= 6 && item.uv2_fmt == 26) {
      // Exact world families: the REAL vertex normal is packed in the
      // lightmap-unwrap element (fmt 26 s16x4): zw = normal.xy (snorm), and
      // the unwrap xy SIGN bits carry the handedness; sign.y flips
      // normal.z (baseenvironment VS: vNormal.z = signs.y * sqrt(1 - xy^2);
      // signs = saturate(65535 * lm.xy) * 2 - 1). The k_10_11_11 element on
      // these meshes is the BINORMAL, not the normal; using it as the
      // normal breaks the sun/spec terms of the exact shading.
      const uint8_t* q = src_vb + size_t(v) * item.stride + item.uv2_offset;
      const int16_t sx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q)));
      const int16_t sy = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 2)));
      const int16_t nx = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 4)));
      const int16_t ny = int16_t(SwapU16(*reinterpret_cast<const uint16_t*>(q + 6)));
      n3[0] = nx / 32767.0f;
      n3[1] = ny / 32767.0f;
      const float d = 1.0f - n3[0] * n3[0] - n3[1] * n3[1];
      n3[2] = (sy > 0 ? 1.0f : -1.0f) * std::sqrt(d > 0.0f ? d : 0.0f);
      (void)sx;  // sign.x = tangent handedness, unused until normal maps
    } else if (item.normal_fmt == 16) {
      // (Vehicle meshes: the game's VS derives cross(tangent, binormal)
      // instead, but the stored usage-3 normal MATCHES it on every vertex
      // with a non-degenerate tangent frame and stays sane on the ~13%
      // interior verts where t is parallel to b and the cross vanishes,
      // measured in capture, so the stored normal is used.)
      unpack_10_11_11(item.normal_offset, n3);
    } else if (item.tb_fmt == 16) {
      // NPC character meshes carry no normal element; the game's VS
      // derives it as cross(tangent, binormal) (usage 6 x usage 7; verified
      // against the emulated VS outputs). Without this the strong
      // character N.L shading exposes the face-normal fallback as visible
      // low-poly facets on every pedestrian.
      float t3[3], b3[3];
      unpack_10_11_11(item.tangent_offset, t3);
      unpack_10_11_11(item.binormal_offset, b3);
      n3[0] = t3[1] * b3[2] - t3[2] * b3[1];
      n3[1] = t3[2] * b3[0] - t3[0] * b3[2];
      n3[2] = t3[0] * b3[1] - t3[1] * b3[0];
    }
    dst[v * 14 + 9] = n3[0];
    dst[v * 14 + 10] = n3[1];
    dst[v * 14 + 11] = n3[2];
  }
  vb->Unmap(0, nullptr);
  // Blend indices outside the captured palette read garbage rows and mangle
  // the vertex. Indices are plain bone numbers, bone k = palette rows 3k.
  // (Dyn decode jobs carry no palette: the item's bones ride the scene item,
  // not the decode; skip the check there.)
  if (item.skinned && max_bi >= 0 && !item.bones.empty()) {
    const uint32_t palette_bones = uint32_t(item.bones.size() / 12);
    if (uint32_t(max_bi) >= palette_bones) {
      REXLOG_WARN(
          "native-scene: skinned mesh {:08X} blend index range {}..{} exceeds "
          "captured palette of {} bones",
          item.mesh, min_bi, max_bi, palette_bones);
    }
  }
  if (garbage != 0) {
    REXLOG_WARN(
        "native-scene: mesh {:08X} decoded {} of {} verts outside bbox "
        "({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) fmt {} stride {} vb {:08X}",
        item.mesh, garbage, num_verts, item.bbox_min[0], item.bbox_min[1], item.bbox_min[2],
        item.bbox_max[0], item.bbox_max[1], item.bbox_max[2], item.pos_fmt, item.stride,
        item.vb_addr);
  }

  uint16_t* dst_ib = nullptr;
  ib->Map(0, nullptr, reinterpret_cast<void**>(&dst_ib));
  if (item.cloth_quads) {
    // Quad-list topology with no guest index buffer (live vertex range
    // already exact from the draw args): two triangles per quad.
    const uint32_t quads = item.ib_count / 6;
    for (uint32_t q = 0; q < quads; ++q) {
      const uint16_t v = uint16_t(q * 4);
      uint16_t* o = dst_ib + q * 6;
      o[0] = v;
      o[1] = uint16_t(v + 1);
      o[2] = uint16_t(v + 2);
      o[3] = v;
      o[4] = uint16_t(v + 2);
      o[5] = uint16_t(v + 3);
    }
  } else {
    const uint16_t* src_ib = reinterpret_cast<const uint16_t*>(ib_payload);
    for (uint32_t i = 0; i < item.ib_count; ++i) {
      dst_ib[i] = SwapU16(src_ib[i]);
    }
  }
  // Front/back sheet pattern: opposite-winding twin triangles a few mm-cm
  // apart ALONG THE NORMAL. The two sides are often triangulated along
  // OPPOSITE quad diagonals (downtown lamppost posters: sheets 4mm apart,
  // twin centroids 0.26m apart in-plane), so twins are matched by
  // plane-to-plane distance with an in-plane tolerance scaled to triangle
  // size; raw centroid distance misses them. >=60% twinned marks the mesh
  // double-sided (banners are 100%); O(T^2) but only for <=8192-index
  // static tri-list meshes and only once per decode.
  bool two_sided = false;
  if (detect_sheet) {
    struct SheetTri {
      float c[3];
      float n[3];
      float edge;  // longest edge length (in-plane tolerance scale)
    };
    std::vector<SheetTri> tris;
    tris.reserve(item.ib_count / 3);
    for (const DrawEntry& de : item.draws) {
      if (uint64_t(de.start_index) + de.index_count > item.ib_count) {
        continue;
      }
      for (uint32_t i = 0; i + 2 < de.index_count; i += 3) {
        const uint32_t a = uint32_t(dst_ib[de.start_index + i]) + de.base_vertex;
        const uint32_t b = uint32_t(dst_ib[de.start_index + i + 1]) + de.base_vertex;
        const uint32_t c = uint32_t(dst_ib[de.start_index + i + 2]) + de.base_vertex;
        if (a >= num_verts || b >= num_verts || c >= num_verts) {
          continue;
        }
        const float* pa = &sheet_pos[size_t(a) * 3];
        const float* pb = &sheet_pos[size_t(b) * 3];
        const float* pc = &sheet_pos[size_t(c) * 3];
        const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-8f) {
          continue;
        }
        SheetTri t;
        for (int k = 0; k < 3; ++k) {
          t.c[k] = (pa[k] + pb[k] + pc[k]) / 3.0f;
          t.n[k] = n[k] / len;
        }
        const float e3[3] = {pc[0] - pb[0], pc[1] - pb[1], pc[2] - pb[2]};
        const float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        const float l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        const float l3 = e3[0] * e3[0] + e3[1] * e3[1] + e3[2] * e3[2];
        t.edge = std::sqrt(std::max(l1, std::max(l2, l3)));
        tris.push_back(t);
      }
    }
    if (tris.size() >= 2) {
      std::vector<char> used(tris.size(), 0);
      size_t twins = 0;
      for (size_t i = 0; i < tris.size(); ++i) {
        if (used[i]) {
          continue;
        }
        for (size_t j = i + 1; j < tris.size(); ++j) {
          if (used[j]) {
            continue;
          }
          const float dx = tris[i].c[0] - tris[j].c[0];
          const float dy = tris[i].c[1] - tris[j].c[1];
          const float dz = tris[i].c[2] - tris[j].c[2];
          const float d2 = dx * dx + dy * dy + dz * dz;
          // Plane separation along i's normal must be small (back-to-back
          // sheets); in-plane offset up to the triangle scale (opposite
          // diagonal splits put twin centroids half a quad apart).
          const float along = std::fabs(dx * tris[i].n[0] + dy * tris[i].n[1] +
                                        dz * tris[i].n[2]);
          const float lat_limit = 0.5f * (tris[i].edge + tris[j].edge);
          if (along > 0.05f || d2 - along * along > lat_limit * lat_limit) {
            continue;
          }
          const float dot = tris[i].n[0] * tris[j].n[0] + tris[i].n[1] * tris[j].n[1] +
                            tris[i].n[2] * tris[j].n[2];
          if (dot < -0.9f) {
            used[i] = 1;
            used[j] = 1;
            twins += 2;
            break;
          }
        }
      }
      // Fully twinned sheets (banners ~100%), OR a meaningful twin patch
      // inside a larger single-sided mesh (harbor sailboats: the twinned
      // SAIL rides a hull/mast mesh at 4-8% twins; the coincident copies
      // sit below far-field depth precision and z-fight into per-frame
      // lit/dark shimmer at range). The twins themselves are the evidence
      // the mesh was authored for culling-on; coincident opposite-normal
      // copies would z-fight on console too, so cull the whole mesh.
      // EXCEPT alpha-tested foliage (tree fams 9/10, envsimple.alphatest 7,
      // alphatest dynobj, transparent, and unclassified fam 0): a z-fight
      // between twinned leaf cards is invisible through the alpha test, so
      // twins do NOT prove culling-on there; culling stripped every
      // single-sided leaf/branch card seen from its back (the missing-
      // foliage regression). Those keep the strict fully-twinned rule.
      const bool partial_rule_ok =
          !item.transparent && item.dynobj != 2 && item.env_family != 0 &&
          item.env_family != 7 && item.env_family != 9 && item.env_family != 10;
      two_sided = twins * 10 >= tris.size() * 6 ||
                  (partial_rule_ok && twins >= 12 && twins * 33 >= tris.size());
      if (two_sided) {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
          REXLOG_INFO("native-scene: two-sided sheet mesh {:08X} ({} indices, {}/{} twins) -> cull-back",
                      item.mesh, item.ib_count, twins, tris.size());
        }
      }
    }
  }
  ib->Unmap(0, nullptr);

  out.vb = vb;
  out.ib = ib;
  out.vb_view = {vb->GetGPUVirtualAddress(), num_verts * 56u, 56u};
  out.ib_view = {ib->GetGPUVirtualAddress(), item.ib_count * 2u, DXGI_FORMAT_R16_UINT};
  out.two_sided_sheet = two_sided;
  return true;
}

// Decode a guest texture from its 6 fetch-constant words (host-endian),
// the v1-verified path: CPU untile block by block through the 0xA0000000
// physical mirror, endian swap, and create its SRV in the staging heap.
// The 3D path reads the words from renderengine::Texture objects; the 2D
// path passes the device fetch-shadow words directly.
// BC1/DXT1 block decode (both color modes) into 16 RGBA8 texels.
void DecodeBc1Block(const uint8_t* b, uint8_t px[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
  const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t col[4][4];
  const auto expand = [](uint16_t c, uint8_t* o) {
    o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    o[2] = uint8_t((c & 31) * 255 / 31);
    o[3] = 255;
  };
  expand(c0, col[0]);
  expand(c1, col[1]);
  if (c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      col[2][k] = uint8_t((2 * col[0][k] + col[1][k]) / 3);
      col[3][k] = uint8_t((col[0][k] + 2 * col[1][k]) / 3);
    }
    col[2][3] = 255;
    col[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      col[2][k] = uint8_t((col[0][k] + col[1][k]) / 2);
      col[3][k] = 0;
    }
    col[2][3] = 255;
    col[3][3] = 0;
  }
  uint32_t bits =
      uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(px[i], col[bits & 3u], 4);
    bits >>= 2;
  }
}

// Runtime-composed textures (lightmap atlas pages above all) ship with NO
// mip chain; sampling their razor-contrast mip 0 under minification
// shimmers, grazing-angle banner posters, distant thin geometry, because
// the pixel footprint spans many texels the aniso sampler cannot average at
// mip 0 alone. The game masked this with its 720p softness and the 4-tap
// lightmap filter; at native 4K we need real prefiltering. Small no-mip
// DXT1/8888 textures are decoded to RGBA8 and uploaded with a CPU
// box-filtered mip chain instead. Returns false to fall back to the plain
// single-mip path.
bool UploadGeneratedMips(const NativeGuestOutputRenderContext& context, uint8_t* base,
                         const rex::graphics::TextureInfo& info, uint32_t fetch_swizzle,
                         GuestTexture& out) {
  const rex::graphics::FormatInfo* format_info = info.format_info();
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  const uint32_t bytes_per_block_log2 = uint32_t(std::countr_zero(bytes_per_block));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const bool bc1 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_DXT1;

  // Guest mip 0 copy: same packed-base + tiled macro-row padding rules as
  // the plain path.
  uint32_t ox = 0, oy = 0;
  const uint32_t addr = info.GetMipLocation(0, &ox, &oy, true);
  if (addr == 0) {
    return false;
  }
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t min_size = info.memory.base_size;
  uint32_t size = min_size;
  const uint32_t cols = (width + block_w - 1) / block_w;
  const uint32_t rows = (height + block_h - 1) / block_h;
  if (info.is_tiled) {
    const uint32_t padded_rows = ((rows + oy) + 31u) & ~31u;
    size = std::max(size, padded_rows * pitch_blocks * bytes_per_block);
  }
  static thread_local std::vector<uint8_t> gen_scratch;
  gen_scratch.resize(size);
  if (!GuestTryCopy(gen_scratch.data(), base + (0xA0000000u | addr), size)) {
    if (min_size >= size ||
        !GuestTryCopy(gen_scratch.data(), base + (0xA0000000u | addr), min_size)) {
      return false;
    }
    size = min_size;
  }

  // Untile into linear block rows, endian-swap per row.
  std::vector<uint8_t> linear(size_t(cols) * rows * bytes_per_block);
  for (uint32_t by = 0; by < rows; ++by) {
    uint8_t* out_row = linear.data() + size_t(by) * cols * bytes_per_block;
    for (uint32_t bx = 0; bx < cols; ++bx) {
      uint32_t source_offset;
      if (info.is_tiled) {
        source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(bx + ox), int32_t(by + oy), pitch_blocks, bytes_per_block_log2));
      } else {
        source_offset = ((by + oy) * pitch_blocks + bx + ox) * bytes_per_block;
      }
      if (source_offset + bytes_per_block > size) {
        std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
        continue;
      }
      std::memcpy(out_row + size_t(bx) * bytes_per_block, gen_scratch.data() + source_offset,
                  bytes_per_block);
    }
    SwapGuestEndian(out_row, cols * bytes_per_block, info.endianness);
  }

  // Decode to RGBA8 mip 0.
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  if (bc1) {
    for (uint32_t by = 0; by < rows; ++by) {
      for (uint32_t bx = 0; bx < cols; ++bx) {
        uint8_t px[16][4];
        DecodeBc1Block(linear.data() + (size_t(by) * cols + bx) * bytes_per_block, px);
        for (uint32_t t = 0; t < 16; ++t) {
          const uint32_t x = bx * 4 + (t & 3u);
          const uint32_t y = by * 4 + (t >> 2);
          if (x < width && y < height) {
            std::memcpy(&rgba[(size_t(y) * width + x) * 4], px[t], 4);
          }
        }
      }
    }
  } else {  // k_8_8_8_8: rows are already RGBA8 after the endian swap
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(&rgba[size_t(y) * width * 4], linear.data() + size_t(y) * cols * 4,
                  size_t(width) * 4);
    }
  }

  // Box-filtered chain down to 1x1.
  std::vector<std::vector<uint8_t>> mips;
  mips.emplace_back(std::move(rgba));
  uint32_t mw = width, mh = height;
  while (mw > 1 || mh > 1) {
    const uint32_t nw = std::max(mw >> 1, 1u);
    const uint32_t nh = std::max(mh >> 1, 1u);
    const std::vector<uint8_t>& srcm = mips.back();
    std::vector<uint8_t> dstm(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      const uint32_t y0 = std::min(y * 2, mh - 1);
      const uint32_t y1 = std::min(y * 2 + 1, mh - 1);
      for (uint32_t x = 0; x < nw; ++x) {
        const uint32_t x0 = std::min(x * 2, mw - 1);
        const uint32_t x1 = std::min(x * 2 + 1, mw - 1);
        for (int k = 0; k < 4; ++k) {
          const uint32_t sum = srcm[(size_t(y0) * mw + x0) * 4 + k] +
                               srcm[(size_t(y0) * mw + x1) * 4 + k] +
                               srcm[(size_t(y1) * mw + x0) * 4 + k] +
                               srcm[(size_t(y1) * mw + x1) * 4 + k];
          dstm[(size_t(y) * nw + x) * 4 + k] = uint8_t((sum + 2) / 4);
        }
      }
    }
    mips.emplace_back(std::move(dstm));
    mw = nw;
    mh = nh;
  }
  const uint32_t mip_count = uint32_t(mips.size());

  // Upload plan + resource (RGBA8).
  struct Plan {
    uint32_t offset, pitch, w, h;
  };
  std::vector<Plan> plans(mip_count);
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    Plan& p = plans[m];
    p.w = std::max(width >> m, 1u);
    p.h = std::max(height >> m, 1u);
    p.pitch = (p.w * 4u + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) &
              ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    p.offset = (upload_size + (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u)) &
               ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u);
    upload_size = p.offset + p.pitch * p.h;
  }
  ID3D12Device* device = context.d3d12.device;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = UINT16(mip_count);
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&out.texture)))) {
    out.texture = nullptr;
    return false;
  }
  out.upload = CreateUploadBuffer(device, upload_size);
  if (!out.upload) {
    out.texture->Release();
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = nullptr;
  out.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapping));
  for (uint32_t m = 0; m < mip_count; ++m) {
    const Plan& p = plans[m];
    for (uint32_t y = 0; y < p.h; ++y) {
      std::memcpy(mapping + p.offset + size_t(y) * p.pitch, &mips[m][size_t(y) * p.w * 4],
                  size_t(p.w) * 4);
    }
  }
  out.upload->Unmap(0, nullptr);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe; the render thread records
    // the copies + barrier and creates the SRV (CommitStagedGuestTexture).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.swizzle_mapping =
        ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    sc.mip_count = std::min<uint32_t>(mip_count, 16);
    for (uint32_t m = 0; m < sc.mip_count; ++m) {
      sc.mips[m] = {plans[m].offset, plans[m].pitch, plans[m].w, plans[m].h};
    }
  } else {
    auto* command_processor = context.d3d12.command_processor;
    auto& list = command_processor->GetDeferredCommandList();
    for (uint32_t m = 0; m < mip_count; ++m) {
      const Plan& p = plans[m];
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = out.texture;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = m;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = out.upload;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = p.offset;
      src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      src.PlacedFootprint.Footprint.Width = p.w;
      src.PlacedFootprint.Footprint.Height = p.h;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = p.pitch;
      list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          out.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (!AllocGuestSrvSlot(out.srv_slot)) {
      return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping =
        ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    srv.Texture2D.MipLevels = mip_count;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(out.texture, &srv, slot);
  }
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = size;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

bool EnsureGuestTextureFromWords(const NativeGuestOutputRenderContext& context,
                                 uint8_t* base, const uint32_t words[6],
                                 GuestTexture& out) {
  std::memcpy(out.fetch_words, words, 6 * sizeof(uint32_t));

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
  if (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked ||
      info.width >= 8192 || info.height >= 8192 || info.memory.base_address == 0 ||
      info.memory.base_size == 0 || info.memory.base_size > 64u * 1024u * 1024u) {
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
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const uint32_t host_width = ((width + block_w - 1) / block_w) * block_w;
  const uint32_t host_height = ((height + block_h - 1) / block_h) * block_h;

  // Upload the guest MIP CHAIN, not just mip 0; sampling mip 0 at distance
  // is the source of the grass "TV static" and flickering floor/window
  // lines. Power-of-two sizes only (everything the game ships) so BC block
  // alignment holds on every level.
  const bool pow2 = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
  uint32_t mip_count = 1;
  if (pow2 && info.memory.mip_address != 0 &&
      REXCVAR_GET(skate3_native_render_scene_tex_mips)) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_count < avail && (width >> mip_count) >= 4 && (height >> mip_count) >= 4) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_count, &ox, &oy, true) == 0) {
        break;
      }
      ++mip_count;
    }
  }
  // No guest chain at all (runtime-composed lightmap pages): generate one,
  // see UploadGeneratedMips. Small DXT1/8888 textures only; falls back to
  // the plain single-mip path on any failure.
  if (mip_count == 1 && pow2 && REXCVAR_GET(skate3_native_render_scene_tex_mips) &&
      width >= 8 && height >= 8 && width <= 512 && height <= 512) {
    const auto base_fmt = rex::graphics::GetBaseFormat(info.format);
    if (base_fmt == xenos::TextureFormat::k_DXT1 ||
        base_fmt == xenos::TextureFormat::k_8_8_8_8) {
      GuestTexture gen = out;  // keeps fetch_words already copied
      if (UploadGeneratedMips(context, base, info, fetch.swizzle, gen)) {
        out = gen;
        return true;
      }
    }
  }

  // Copy the whole guest mip chain out up front with the lock-free guarded
  // copy (never VirtualQuery on the render thread: the VAD lock stalls
  // behind the guest streaming threads exactly while panning streams
  // textures in), truncating the chain at the first unreadable level.
  struct MipSrc {
    uint32_t addr, scratch_off, size, min_size, pitch_blocks, ox, oy;
  };
  MipSrc srcs[16] = {};
  static thread_local std::vector<uint8_t> tex_scratch;
  uint32_t scratch_total = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    uint32_t ox = 0, oy = 0;
    // Mip 0 through GetMipLocation too: textures <= 16 texels on a side
    // store their BASE level packed inside a 32x32 tile at a block offset.
    // Reading at (0,0) decoded garbage; the 16x16 "default_white" macro
    // overlay came out as pink/black blocks and multiplied giant soft black
    // blobs over every wall whose material uses it (validated: with the
    // packed offset it decodes pure white). Non-packed textures return the
    // plain base address with zero offsets.
    const uint32_t mip_addr = info.GetMipLocation(m, &ox, &oy, true);
    const auto ext = info.GetMipExtent(m, true);
    MipSrc& s = srcs[m];
    s.addr = mip_addr;
    s.pitch_blocks = m == 0 ? info.extent.block_pitch_h : ext.block_pitch_h;
    s.size = m == 0 ? info.memory.base_size : ext.all_blocks() * bytes_per_block;
    s.min_size = s.size;
    if (info.is_tiled) {
      // Tiled addressing swizzles across 32x32-BLOCK macro tiles, so a small
      // texture with a macro-aligned fetch pitch stores blocks far beyond its
      // naive linear size (the 32x32 DXT5 HUD compass icons, pitch 128
      // texels: last block at offset 5952 vs base_size 1024; the size guard
      // zeroed 48 of 64 blocks, "icons sliced off"). Copy whole macro rows;
      // if that over-reaches the committed allocation the copy loop below
      // falls back to the reported size.
      const uint32_t mh = std::max(height >> m, 1u);
      const uint32_t rows = (mh + block_h - 1) / block_h + oy;
      const uint32_t padded_rows = (rows + 31u) & ~31u;
      s.size = std::max(s.size, padded_rows * s.pitch_blocks * bytes_per_block);
    }
    s.ox = ox;
    s.oy = oy;
    s.scratch_off = scratch_total;
    scratch_total += s.size;
  }
  tex_scratch.resize(scratch_total);
  uint32_t mips_copied = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    MipSrc& s = srcs[m];
    if (!GuestTryCopy(tex_scratch.data() + s.scratch_off,
                      base + (0xA0000000u | s.addr), s.size)) {
      if (s.min_size >= s.size ||
          !GuestTryCopy(tex_scratch.data() + s.scratch_off,
                        base + (0xA0000000u | s.addr), s.min_size)) {
        break;
      }
      s.size = s.min_size;
    }
    ++mips_copied;
  }
  if (mips_copied == 0) {
    return false;
  }
  mip_count = mips_copied;

  // Per-mip upload footprints (D3D12 alignment rules).
  struct MipPlan {
    uint32_t offset, pitch, cols, rows;
  };
  MipPlan plans[16] = {};
  uint32_t upload_size = 0;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const uint32_t mw = std::max(width >> m, 1u);
    const uint32_t mh = std::max(height >> m, 1u);
    MipPlan& p = plans[m];
    p.cols = (mw + block_w - 1) / block_w;
    p.rows = (mh + block_h - 1) / block_h;
    p.pitch = (p.cols * bytes_per_block + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) &
              ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    p.offset = (upload_size + (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u)) &
               ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u);
    upload_size = p.offset + p.pitch * p.rows;
  }

  ID3D12Device* device = context.d3d12.device;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = host_width;
  desc.Height = host_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = UINT16(mip_count);
  desc.Format = host.resource_format;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&out.texture)))) {
    return false;
  }
  out.upload = CreateUploadBuffer(device, upload_size);
  if (!out.upload) {
    out.texture->Release();
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = nullptr;
  out.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapping));
  const bool swap_rb_565 =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_5_6_5;
  for (uint32_t m = 0; m < mip_count; ++m) {
    const MipPlan& p = plans[m];
    const uint32_t ox = srcs[m].ox;
    const uint32_t oy = srcs[m].oy;
    const uint32_t src_pitch_blocks = srcs[m].pitch_blocks;
    const uint32_t src_size = srcs[m].size;
    const uint8_t* guest = tex_scratch.data() + srcs[m].scratch_off;
    const uint32_t row_bytes = p.cols * bytes_per_block;
    for (uint32_t by = 0; by < p.rows; ++by) {
      uint8_t* out_row = mapping + p.offset + size_t(by) * p.pitch;
      for (uint32_t bx = 0; bx < p.cols; ++bx) {
        uint32_t source_offset;
        if (info.is_tiled) {
          source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(bx + ox), int32_t(by + oy), src_pitch_blocks, bytes_per_block_log2));
        } else {
          source_offset = ((by + oy) * src_pitch_blocks + bx + ox) * bytes_per_block;
        }
        if (source_offset + bytes_per_block > src_size) {
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
  }
  out.upload->Unmap(0, nullptr);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe; the render thread records
    // the copies + barrier and creates the SRV (CommitStagedGuestTexture).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    sc.swizzle_mapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
    sc.mip_count = std::min<uint32_t>(mip_count, 16);
    for (uint32_t m = 0; m < sc.mip_count; ++m) {
      sc.mips[m] = {plans[m].offset, plans[m].pitch, std::max(host_width >> m, 1u),
                    std::max(host_height >> m, 1u)};
    }
  } else {
    // Record the upload copies into the deferred command list.
    auto* command_processor = context.d3d12.command_processor;
    auto& list = command_processor->GetDeferredCommandList();
    for (uint32_t m = 0; m < mip_count; ++m) {
      const MipPlan& p = plans[m];
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = out.texture;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = m;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = out.upload;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = p.offset;
      src.PlacedFootprint.Footprint.Format = host.resource_format;
      src.PlacedFootprint.Footprint.Width = std::max(host_width >> m, 1u);
      src.PlacedFootprint.Footprint.Height = std::max(host_height >> m, 1u);
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = p.pitch;
      list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          out.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // SRV in the staging heap with the composed swizzle.
    if (!AllocGuestSrvSlot(out.srv_slot)) {
      return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = host.srv_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
    srv.Texture2D.MipLevels = mip_count;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(out.texture, &srv, slot);
  }
  // Payload sample for content revalidation (see GuestTexture).
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = srcs[0].size;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

// Fetch-constant read from a renderengine::Texture object (words [7..12]).
bool EnsureGuestTexture(const NativeGuestOutputRenderContext& context, uint8_t* base,
                        uint32_t tex_ptr, GuestTexture& out) {
  uint32_t words[6] = {};
  {
    uint32_t raw[6];
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw))) {
      return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
      words[i] = SwapU32(raw[i]);
    }
  }
  return EnsureGuestTextureFromWords(context, base, words, out);
}

// Environment CUBE map for the water reflection term (t6). Mip 0 only, six
// faces untiled independently (Xenos cubes are 2D-tiled per face slice).
bool EnsureGuestCubeTexture(const NativeGuestOutputRenderContext& context, uint8_t* base,
                            uint32_t tex_ptr, GuestTexture& out) {
  uint32_t words[6] = {};
  {
    uint32_t raw[6];
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw))) {
      return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
      words[i] = SwapU32(raw[i]);
    }
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
  if (info.dimension != xenos::DataDimension::kCube || info.memory.base_address == 0 ||
      info.width >= 1024 || info.height >= 1024) {
    return false;
  }
  HostTextureFormat host;
  if (!GetHostTextureFormat(info.format, host)) {
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
  const uint32_t block_w = format_info->block_width;
  const uint32_t block_h = format_info->block_height;
  const uint32_t host_width = ((width + block_w - 1) / block_w) * block_w;
  const uint32_t host_height = ((height + block_h - 1) / block_h) * block_h;
  const uint32_t cols = (width + block_w - 1) / block_w;
  const uint32_t rows = (height + block_h - 1) / block_h;
  const uint32_t pitch_blocks = info.extent.block_pitch_h;
  const uint32_t slice_blocks = info.extent.block_pitch_h * info.extent.block_pitch_v;
  const uint32_t slice_bytes = slice_blocks * bytes_per_block;
  const uint32_t total = slice_bytes * 6;
  if (total == 0 || total > 16u * 1024u * 1024u) {
    return false;
  }
  static thread_local std::vector<uint8_t> cube_scratch;
  cube_scratch.resize(total);
  if (!GuestTryCopy(cube_scratch.data(), base + (0xA0000000u | info.memory.base_address),
                    total)) {
    return false;
  }

  const uint32_t row_bytes = cols * bytes_per_block;
  const uint32_t pitch = (row_bytes + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) &
                         ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
  const uint32_t face_upload = ((pitch * rows + (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u)) &
                                ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u));
  ID3D12Device* device = context.d3d12.device;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = host_width;
  desc.Height = host_height;
  desc.DepthOrArraySize = 6;
  desc.MipLevels = 1;
  desc.Format = host.resource_format;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&out.texture)))) {
    out.texture = nullptr;
    return false;
  }
  out.upload = CreateUploadBuffer(device, face_upload * 6);
  if (!out.upload) {
    out.texture->Release();
    out.texture = nullptr;
    return false;
  }
  uint8_t* mapping = nullptr;
  out.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapping));
  for (uint32_t face = 0; face < 6; ++face) {
    const uint8_t* guest = cube_scratch.data() + size_t(face) * slice_bytes;
    for (uint32_t by = 0; by < rows; ++by) {
      uint8_t* out_row = mapping + size_t(face) * face_upload + size_t(by) * pitch;
      for (uint32_t bx = 0; bx < cols; ++bx) {
        uint32_t source_offset;
        if (info.is_tiled) {
          source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(bx), int32_t(by), pitch_blocks, bytes_per_block_log2));
        } else {
          source_offset = (by * pitch_blocks + bx) * bytes_per_block;
        }
        if (source_offset + bytes_per_block > slice_bytes) {
          std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
          continue;
        }
        std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + source_offset,
                    bytes_per_block);
      }
      SwapGuestEndian(out_row, row_bytes, info.endianness);
    }
  }
  out.upload->Unmap(0, nullptr);

  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe (mips[0..5] = the six faces).
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    sc.swizzle_mapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
    sc.cube = true;
    sc.mip_count = 6;
    for (uint32_t face = 0; face < 6; ++face) {
      sc.mips[face] = {uint32_t(face) * face_upload, pitch, host_width, host_height};
    }
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = total;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = false;  // live only after commit
    return true;
  }

  auto& list = context.d3d12.command_processor->GetDeferredCommandList();
  for (uint32_t face = 0; face < 6; ++face) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = out.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = face;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = out.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = size_t(face) * face_upload;
    src.PlacedFootprint.Footprint.Format = host.resource_format;
    src.PlacedFootprint.Footprint.Width = host_width;
    src.PlacedFootprint.Footprint.Height = host_height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        out.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (!AllocGuestSrvSlot(out.srv_slot)) {
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = host.srv_format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  srv.Shader4ComponentMapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
  srv.TextureCube.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
  device->CreateShaderResourceView(out.texture, &srv, slot);
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = total;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = true;
  return true;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) return false;
  ID3D12Device* device = context.d3d12.device;
  g_r.device = device;

  if (!g_r.root_signature) {
    D3D12_ROOT_PARAMETER params[10] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    // NOTE the 64-DWORD root-signature budget: 52 constants + 6 descriptor
    // tables (1 each) + 1 root SRV (2) + 2 root CBVs (2 each) = 64, FULL.
    // Going past 64 makes CreateRootSignature fail (renderer falls back to
    // emulated). Any further addition must pack into existing rows/tables.
    params[0].Constants.Num32BitValues = 52;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE srv_range[6] = {};
    srv_range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range[0].NumDescriptors = 1;
    srv_range[0].BaseShaderRegister = 0;
    srv_range[1] = srv_range[0];
    srv_range[1].BaseShaderRegister = 1;
    srv_range[2] = srv_range[0];
    srv_range[2].BaseShaderRegister = 3;  // macro overlay (t3)
    srv_range[3] = srv_range[0];
    srv_range[3].BaseShaderRegister = 4;  // environment.decal art (t4)
    srv_range[4] = srv_range[0];
    srv_range[4].BaseShaderRegister = 5;  // shadow atlas (t5)
    srv_range[5] = srv_range[0];
    srv_range[5].BaseShaderRegister = 6;  // water environment cube (t6)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range[0];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2] = params[1];
    params[2].DescriptorTable.pDescriptorRanges = &srv_range[1];
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 2;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[4] = params[1];
    params[4].DescriptorTable.pDescriptorRanges = &srv_range[2];
    params[5] = params[1];
    params[5].DescriptorTable.pDescriptorRanges = &srv_range[3];
    // Dynamic-shadow additions: per-frame receiver constants (b1) + the
    // blurred shadow atlas (t5).
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[6].Descriptor.ShaderRegister = 1;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[7] = params[1];
    params[7].DescriptorTable.pDescriptorRanges = &srv_range[4];
    // Water environment cube (t6): the flowingwater/ocean reflection term.
    params[8] = params[1];
    params[8].DescriptorTable.pDescriptorRanges = &srv_range[5];
    // Character lighting block (b2): the canonical per-draw rows captured
    // from the guest PS bank (CaptureCharLighting), sliced out of the bone
    // upload ring per character draw.
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[9].Descriptor.ShaderRegister = 2;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy = 8;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: bilinear CLAMP for the 2D overlay. The HUD fetch constants carry
    // clamp_x/clamp_y = 2 (clamp-to-edge), and the clock face is built from
    // MIRRORED quadrant tiles whose outer-edge UVs run past 1.0; wrap
    // sampling pulls the opposite edge of the art in as 1px seam lines
    // (bright rim row across the middle, dark center column at the edges).
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].MaxAnisotropy = 1;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 10;
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!context.d3d12.create_root_signature(context.d3d12.command_processor_user_data,
                                             &desc, &g_r.root_signature)) {
      REXLOG_ERROR("native-scene: root signature creation failed");
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.pso || g_r.rtv_format != context.d3d12.guest_output_format) {
    // MSAA level: the requested count, reduced to what the output format
    // supports (1 disables and renders directly into the guest output).
    const int32_t req = REXCVAR_GET(skate3_native_render_scene_msaa);
    uint32_t msaa = req >= 8 ? 8u : req >= 4 ? 4u : req >= 2 ? 2u : 1u;
    while (msaa > 1) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{
          context.d3d12.guest_output_format, msaa, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
      if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                &q, sizeof(q))) &&
          q.NumQualityLevels > 0) {
        break;
      }
      msaa /= 2;
    }
    g_r.msaa = msaa;

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
    D3D12_INPUT_ELEMENT_DESC input[7] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 48,
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
    pso.InputLayout = {input, 7};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = context.d3d12.guest_output_format;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = g_r.msaa;
    const HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso));
    // Culling variant for double-sided sheet props (banners/flags,
    // MeshBuffers::two_sided_sheet). The sheet whose winding-derived world
    // normal faces the camera is the one the game keeps (its lightmap cell
    // reproduces the emulated banner exactly: albedo x lm x 2 lands within
    // 3-8% of the F11 emulated reference on capture 1783387480); under our
    // pipeline those triangles are D3D12 BACK faces, so cull FRONT.
    // (CULL_BACK was tried first and kept the wrong sheet; banners rendered
    // the sun-side lightmap cell ~2.4x brighter than emulated.)
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    const HRESULT hr_cb =
        device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_cullback));
    if (FAILED(hr_cb)) {
      REXLOG_WARN("native-scene: cull-back PSO creation failed {:08X}", uint32_t(hr_cb));
      g_r.pso_cullback = nullptr;  // sheets fall back to the uncull(ed) PSO
    }
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // Transparent variant: straight alpha blend, depth-tested, no z-write.
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    const HRESULT hr_t =
        device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_transparent));
    // Hair passes: the game draws hair twice with the SAME shader; cull
    // BACK then cull FRONT (cac_hair.xml passes 0/1) so far-side strands
    // never composite over near-side ones (one uncull(ed) blended pass
    // interleaves them per triangle order = crunchy noise). Same blend /
    // z-write-off state as the transparent variant.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_hair_a)))) {
      g_r.pso_hair_a = nullptr;
    }
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_hair_b)))) {
      g_r.pso_hair_b = nullptr;
    }
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    const HRESULT hr2 =
        device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_nodepth));
    // Selection-outline mask: the same scene VS/PS (the tint.a > 0 solid
    // path renders flat 1.0) into the small single-sample R8 target. No
    // depth: the mask is the full silhouette. The guest marking pass is
    // depth-tested, so a partially occluded selection outlines slightly
    // differently, acceptable for the editor UI.
    pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    pso.SampleDesc.Count = 1;
    const HRESULT hr_om =
        device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_outline_mask));
    if (FAILED(hr_om)) {
      REXLOG_WARN("native-scene: outline mask PSO creation failed {:08X}",
                  uint32_t(hr_om));
      g_r.pso_outline_mask = nullptr;  // outline pass disables itself
    }
    pso.RTVFormats[0] = context.d3d12.guest_output_format;
    pso.SampleDesc.Count = g_r.msaa;
    vs->Release();
    ps->Release();
    if (errors) errors->Release();
    if (FAILED(hr) || FAILED(hr2) || FAILED(hr_t)) {
      REXLOG_ERROR("native-scene: PSO creation failed {:08X}/{:08X}/{:08X}", uint32_t(hr),
                   uint32_t(hr2), uint32_t(hr_t));
      g_r.failed = true;
      return false;
    }

    if (g_r.msaa > 1) {
      char samples[8];
      std::snprintf(samples, sizeof(samples), "%u", g_r.msaa);
      const D3D_SHADER_MACRO macros[] = {{"SAMPLES", samples}, {nullptr, nullptr}};
      ID3DBlob* rvs = nullptr;
      ID3DBlob* rps = nullptr;
      ID3DBlob* rerrors = nullptr;
      if (FAILED(D3DCompile(kResolveShaderSource, sizeof(kResolveShaderSource) - 1,
                            "native_resolve", macros, nullptr, "vs_main", "vs_5_0", 0, 0,
                            &rvs, &rerrors)) ||
          FAILED(D3DCompile(kResolveShaderSource, sizeof(kResolveShaderSource) - 1,
                            "native_resolve", macros, nullptr, "ps_main", "ps_5_0", 0, 0,
                            &rps, &rerrors))) {
        REXLOG_ERROR(
            "native-scene: resolve shader compile failed: {}",
            rerrors ? static_cast<const char*>(rerrors->GetBufferPointer()) : "?");
        g_r.failed = true;
        return false;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC rp{};
      rp.pRootSignature = g_r.root_signature;
      rp.VS = {rvs->GetBufferPointer(), rvs->GetBufferSize()};
      rp.PS = {rps->GetBufferPointer(), rps->GetBufferSize()};
      rp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      rp.SampleMask = UINT_MAX;
      rp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      rp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      rp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      rp.NumRenderTargets = 1;
      rp.RTVFormats[0] = context.d3d12.guest_output_format;
      rp.SampleDesc.Count = 1;
      const HRESULT hr3 =
          device->CreateGraphicsPipelineState(&rp, IID_PPV_ARGS(&g_r.resolve_pso));
      rvs->Release();
      rps->Release();
      if (rerrors) rerrors->Release();
      if (FAILED(hr3)) {
        REXLOG_ERROR("native-scene: resolve PSO creation failed {:08X}", uint32_t(hr3));
        g_r.failed = true;
        return false;
      }
    }
    {
      // Popup background blur pipelines (blur_hBlur/vBlur port + basictex
      // replace): fullscreen-triangle passes, no vertex buffer.
      ID3DBlob* bvs = nullptr;
      ID3DBlob* bps = nullptr;
      ID3DBlob* bblit = nullptr;
      ID3DBlob* bdown = nullptr;
      ID3DBlob* berr = nullptr;
      if (FAILED(D3DCompile(kBlurShaderSource, sizeof(kBlurShaderSource) - 1,
                            "native_blur", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0,
                            &bvs, &berr)) ||
          FAILED(D3DCompile(kBlurShaderSource, sizeof(kBlurShaderSource) - 1,
                            "native_blur", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0,
                            &bps, &berr)) ||
          FAILED(D3DCompile(kBlurShaderSource, sizeof(kBlurShaderSource) - 1,
                            "native_blur", nullptr, nullptr, "ps_blit", "ps_5_0", 0, 0,
                            &bblit, &berr)) ||
          FAILED(D3DCompile(kBlurShaderSource, sizeof(kBlurShaderSource) - 1,
                            "native_blur", nullptr, nullptr, "ps_down", "ps_5_0", 0, 0,
                            &bdown, &berr))) {
        REXLOG_ERROR("native-scene: blur shader compile failed: {}",
                     berr ? static_cast<const char*>(berr->GetBufferPointer()) : "?");
        g_r.failed = true;
        return false;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC bp{};
      bp.pRootSignature = g_r.root_signature;
      bp.VS = {bvs->GetBufferPointer(), bvs->GetBufferSize()};
      bp.PS = {bps->GetBufferPointer(), bps->GetBufferSize()};
      bp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      bp.SampleMask = UINT_MAX;
      bp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      bp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      bp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      bp.NumRenderTargets = 1;
      bp.RTVFormats[0] = context.d3d12.guest_output_format;
      bp.SampleDesc.Count = 1;
      if (g_r.pso_blur) g_r.pso_blur->Release();
      if (g_r.pso_blur_blit) g_r.pso_blur_blit->Release();
      if (g_r.pso_blur_down) g_r.pso_blur_down->Release();
      const HRESULT hb1 = device->CreateGraphicsPipelineState(&bp, IID_PPV_ARGS(&g_r.pso_blur));
      bp.PS = {bblit->GetBufferPointer(), bblit->GetBufferSize()};
      const HRESULT hb2 =
          device->CreateGraphicsPipelineState(&bp, IID_PPV_ARGS(&g_r.pso_blur_blit));
      bp.PS = {bdown->GetBufferPointer(), bdown->GetBufferSize()};
      const HRESULT hb3 =
          device->CreateGraphicsPipelineState(&bp, IID_PPV_ARGS(&g_r.pso_blur_down));
      bvs->Release();
      bps->Release();
      bblit->Release();
      bdown->Release();
      if (berr) berr->Release();
      if (FAILED(hb1) || FAILED(hb2) || FAILED(hb3)) {
        REXLOG_ERROR("native-scene: blur PSO creation failed {:08X}/{:08X}/{:08X}",
                     uint32_t(hb1), uint32_t(hb2), uint32_t(hb3));
        g_r.pso_blur = nullptr;
        g_r.pso_blur_blit = nullptr;  // blur unavailable; everything else runs
        g_r.pso_blur_down = nullptr;
      }
    }
    {
      // Selection-outline edge composite (postfx_edgedetectstencil port):
      // fullscreen triangle over the resolved output, additive blend.
      ID3DBlob* ovs = nullptr;
      ID3DBlob* ops = nullptr;
      ID3DBlob* oerr = nullptr;
      if (FAILED(D3DCompile(kOutlineShaderSource, sizeof(kOutlineShaderSource) - 1,
                            "native_outline", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0,
                            &ovs, &oerr)) ||
          FAILED(D3DCompile(kOutlineShaderSource, sizeof(kOutlineShaderSource) - 1,
                            "native_outline", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0,
                            &ops, &oerr))) {
        REXLOG_ERROR("native-scene: outline shader compile failed: {}",
                     oerr ? static_cast<const char*>(oerr->GetBufferPointer()) : "?");
        g_r.failed = true;
        return false;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC op{};
      op.pRootSignature = g_r.root_signature;
      op.VS = {ovs->GetBufferPointer(), ovs->GetBufferSize()};
      op.PS = {ops->GetBufferPointer(), ops->GetBufferSize()};
      op.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      op.BlendState.RenderTarget[0].BlendEnable = TRUE;
      op.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
      op.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
      op.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
      op.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
      op.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
      op.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
      op.SampleMask = UINT_MAX;
      op.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      op.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      op.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      op.NumRenderTargets = 1;
      op.RTVFormats[0] = context.d3d12.guest_output_format;
      op.SampleDesc.Count = 1;
      if (g_r.pso_outline_edge) g_r.pso_outline_edge->Release();
      const HRESULT ho = device->CreateGraphicsPipelineState(&op, IID_PPV_ARGS(&g_r.pso_outline_edge));
      ovs->Release();
      ops->Release();
      if (oerr) oerr->Release();
      if (FAILED(ho)) {
        REXLOG_WARN("native-scene: outline edge PSO creation failed {:08X}", uint32_t(ho));
        g_r.pso_outline_edge = nullptr;  // outline unavailable; everything else runs
      }
    }
    {
      // 2D overlay pipeline: standard alpha blend, no depth, drawn into the
      // resolved guest output (sample count 1).
      ID3DBlob* uvs = nullptr;
      ID3DBlob* ups = nullptr;
      ID3DBlob* uerrors = nullptr;
      if (FAILED(D3DCompile(kShader2dSource, sizeof(kShader2dSource) - 1, "native_2d",
                            nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &uvs,
                            &uerrors)) ||
          FAILED(D3DCompile(kShader2dSource, sizeof(kShader2dSource) - 1, "native_2d",
                            nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ups,
                            &uerrors))) {
        REXLOG_ERROR("native-scene: 2D shader compile failed: {}",
                     uerrors ? static_cast<const char*>(uerrors->GetBufferPointer())
                             : "?");
        g_r.failed = true;
        return false;
      }
      D3D12_INPUT_ELEMENT_DESC input2d[3] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24,
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
      D3D12_GRAPHICS_PIPELINE_STATE_DESC up{};
      up.pRootSignature = g_r.root_signature;
      up.VS = {uvs->GetBufferPointer(), uvs->GetBufferSize()};
      up.PS = {ups->GetBufferPointer(), ups->GetBufferSize()};
      auto& rt = up.BlendState.RenderTarget[0];
      rt.BlendEnable = TRUE;
      // Straight (non-premultiplied) alpha, verified against the decoded
      // HUD clock art: the glass-sheen texture is white RGB at low alpha
      // (premultiplied blending blows it out into an opaque white blob).
      rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      rt.BlendOp = D3D12_BLEND_OP_ADD;
      rt.SrcBlendAlpha = D3D12_BLEND_ONE;
      rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      up.SampleMask = UINT_MAX;
      up.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      up.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      up.InputLayout = {input2d, 3};
      up.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      up.NumRenderTargets = 1;
      up.RTVFormats[0] = context.d3d12.guest_output_format;
      up.SampleDesc.Count = 1;
      const HRESULT hr4 = device->CreateGraphicsPipelineState(&up, IID_PPV_ARGS(&g_r.pso_2d));
      uvs->Release();
      ups->Release();
      if (uerrors) uerrors->Release();
      if (FAILED(hr4)) {
        REXLOG_ERROR("native-scene: 2D PSO creation failed {:08X}", uint32_t(hr4));
        g_r.failed = true;
        return false;
      }
    }
    {
      // In-world spline pipelines: drawn inside the scene pass (MSAA sample
      // count, depth test LESS_EQUAL, no z-write) with the game's own blend
      // states (spline.xml): darken = straight alpha, default = additive.
      ID3DBlob* svs = nullptr;
      ID3DBlob* spd = nullptr;
      ID3DBlob* spk = nullptr;
      ID3DBlob* serrors = nullptr;
      if (FAILED(D3DCompile(kShaderSplineSource, sizeof(kShaderSplineSource) - 1,
                            "native_spline", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0,
                            &svs, &serrors)) ||
          FAILED(D3DCompile(kShaderSplineSource, sizeof(kShaderSplineSource) - 1,
                            "native_spline", nullptr, nullptr, "ps_default", "ps_5_0", 0,
                            0, &spd, &serrors)) ||
          FAILED(D3DCompile(kShaderSplineSource, sizeof(kShaderSplineSource) - 1,
                            "native_spline", nullptr, nullptr, "ps_darken", "ps_5_0", 0,
                            0, &spk, &serrors))) {
        REXLOG_ERROR("native-scene: spline shader compile failed: {}",
                     serrors ? static_cast<const char*>(serrors->GetBufferPointer())
                             : "?");
        g_r.failed = true;
        return false;
      }
      D3D12_INPUT_ELEMENT_DESC input_spline[3] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 24,
           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
      D3D12_GRAPHICS_PIPELINE_STATE_DESC sp{};
      sp.pRootSignature = g_r.root_signature;
      sp.VS = {svs->GetBufferPointer(), svs->GetBufferSize()};
      sp.SampleMask = UINT_MAX;
      sp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      sp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      sp.RasterizerState.DepthClipEnable = TRUE;
      sp.DepthStencilState.DepthEnable = TRUE;
      sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
      sp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
      sp.InputLayout = {input_spline, 3};
      sp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      sp.NumRenderTargets = 1;
      sp.RTVFormats[0] = context.d3d12.guest_output_format;
      sp.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      sp.SampleDesc.Count = g_r.msaa;
      auto& srt = sp.BlendState.RenderTarget[0];
      srt.BlendEnable = TRUE;
      srt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      srt.BlendOp = D3D12_BLEND_OP_ADD;
      srt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      // darken pass: straight alpha.
      sp.PS = {spk->GetBufferPointer(), spk->GetBufferSize()};
      srt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
      srt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      srt.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
      srt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      const HRESULT hr5 =
          device->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&g_r.pso_spline_darken));
      // default pass: additive glow.
      sp.PS = {spd->GetBufferPointer(), spd->GetBufferSize()};
      srt.SrcBlend = D3D12_BLEND_ONE;
      srt.DestBlend = D3D12_BLEND_ONE;
      srt.SrcBlendAlpha = D3D12_BLEND_ONE;
      srt.DestBlendAlpha = D3D12_BLEND_ONE;
      const HRESULT hr6 =
          device->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&g_r.pso_spline_default));
      svs->Release();
      spd->Release();
      spk->Release();
      if (serrors) serrors->Release();
      if (FAILED(hr5) || FAILED(hr6)) {
        REXLOG_ERROR("native-scene: spline PSO creation failed {:08X}/{:08X}",
                     uint32_t(hr5), uint32_t(hr6));
        g_r.failed = true;
        return false;
      }
    }
    {
      // Dynamic-shadow pipelines: casters (scene VS with a light-space
      // "view-proj", MIN-blend depth/uncoverage accumulation, no depth
      // buffer, depth clip OFF so casters outside the 12 m height window
      // clamp like the game accepts) + the per-tile blur/convert pass.
      ID3DBlob* cvs = nullptr;
      ID3DBlob* cps = nullptr;
      ID3DBlob* bvs = nullptr;
      ID3DBlob* bps = nullptr;
      ID3DBlob* werrors = nullptr;
      if (FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "native_scene",
                            nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &cvs, &werrors)) ||
          FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "native_scene",
                            nullptr, nullptr, "ps_shadow_caster", "ps_5_0", 0, 0, &cps,
                            &werrors)) ||
          FAILED(D3DCompile(kShadowBlurSource, sizeof(kShadowBlurSource) - 1,
                            "native_shadow_blur", nullptr, nullptr, "vs_main", "vs_5_0", 0,
                            0, &bvs, &werrors)) ||
          FAILED(D3DCompile(kShadowBlurSource, sizeof(kShadowBlurSource) - 1,
                            "native_shadow_blur", nullptr, nullptr, "ps_main", "ps_5_0", 0,
                            0, &bps, &werrors))) {
        REXLOG_ERROR("native-scene: shadow shader compile failed: {}",
                     werrors ? static_cast<const char*>(werrors->GetBufferPointer())
                             : "?");
        g_r.failed = true;
        return false;
      }
      D3D12_GRAPHICS_PIPELINE_STATE_DESC cp{};
      cp.pRootSignature = g_r.root_signature;
      cp.VS = {cvs->GetBufferPointer(), cvs->GetBufferSize()};
      cp.PS = {cps->GetBufferPointer(), cps->GetBufferSize()};
      auto& crt = cp.BlendState.RenderTarget[0];
      crt.BlendEnable = TRUE;
      crt.SrcBlend = D3D12_BLEND_ONE;
      crt.DestBlend = D3D12_BLEND_ONE;
      crt.BlendOp = D3D12_BLEND_OP_MIN;
      crt.SrcBlendAlpha = D3D12_BLEND_ONE;
      crt.DestBlendAlpha = D3D12_BLEND_ONE;
      crt.BlendOpAlpha = D3D12_BLEND_OP_MIN;
      crt.RenderTargetWriteMask =
          D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
      cp.SampleMask = UINT_MAX;
      cp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      cp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      cp.RasterizerState.DepthClipEnable = FALSE;
      cp.InputLayout = {input, 7};
      cp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      cp.NumRenderTargets = 1;
      cp.RTVFormats[0] = DXGI_FORMAT_R16G16_UNORM;
      cp.SampleDesc.Count = 1;
      const HRESULT hr7 =
          device->CreateGraphicsPipelineState(&cp, IID_PPV_ARGS(&g_r.pso_shadow_caster));
      D3D12_GRAPHICS_PIPELINE_STATE_DESC bp{};
      bp.pRootSignature = g_r.root_signature;
      bp.VS = {bvs->GetBufferPointer(), bvs->GetBufferSize()};
      bp.PS = {bps->GetBufferPointer(), bps->GetBufferSize()};
      bp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      bp.SampleMask = UINT_MAX;
      bp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      bp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      bp.RasterizerState.DepthClipEnable = TRUE;
      bp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      bp.NumRenderTargets = 1;
      bp.RTVFormats[0] = DXGI_FORMAT_R16G16_UNORM;
      bp.SampleDesc.Count = 1;
      const HRESULT hr8 =
          device->CreateGraphicsPipelineState(&bp, IID_PPV_ARGS(&g_r.pso_shadow_blur));
      cvs->Release();
      cps->Release();
      bvs->Release();
      bps->Release();
      if (werrors) werrors->Release();
      if (FAILED(hr7) || FAILED(hr8)) {
        REXLOG_ERROR("native-scene: shadow PSO creation failed {:08X}/{:08X}",
                     uint32_t(hr7), uint32_t(hr8));
        g_r.failed = true;
        return false;
      }
    }
    REXLOG_INFO("native-scene: pipelines created (MSAA x{})", g_r.msaa);
    g_r.rtv_format = context.d3d12.guest_output_format;
  }

  if (!g_r.rtv_heap) {
    // Slots 0/1 = guest output / MSAA color; 2+ = APT render-to-texture
    // targets.
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_r.rtv_heap)))) {
      g_r.failed = true;
      return false;
    }
    g_r.rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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

  if (!g_r.ui_ring) {
    g_r.ui_ring = CreateUploadBuffer(
        device, size_t(RendererState::kUiRegionSize) * RendererState::kUiRegions);
    if (!g_r.ui_ring ||
        FAILED(g_r.ui_ring->Map(0, nullptr,
                                reinterpret_cast<void**>(&g_r.ui_ring_cpu)))) {
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.shadow_raw && REXCVAR_GET(skate3_native_render_scene_shadows)) {
    // Dynamic-shadow atlas chain: raw casters -> hblur intermediate ->
    // blurred final (the texture the scene pass samples). Three fixed-size
    // R16G16_UNORM targets (the game's atlas is fmt 25 = 16_16 fixed point;
    // half-float ulp at the typical ~0.85 depth is ~6 mm of world height,
    // too coarse for board/feet-height casters 1-2 cm off the ground),
    // 3 tiles of tile x tile each; RTV heap slots 2/3/4.
    g_r.shadow_tile = uint32_t(REXCVAR_GET(skate3_native_render_scene_shadow_tile));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = g_r.shadow_tile * 3;
    desc.Height = g_r.shadow_tile;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    clear.Color[0] = 1.0f;  // depth: far
    clear.Color[1] = 1.0f;  // "uncoverage": empty
    ID3D12Resource** targets[3] = {&g_r.shadow_raw, &g_r.shadow_mid, &g_r.shadow_final};
    uint32_t* srv_slots[3] = {&g_r.shadow_srv_raw, &g_r.shadow_srv_mid,
                              &g_r.shadow_srv_final};
    for (int t = 0; t < 3; ++t) {
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                 IID_PPV_ARGS(targets[t])))) {
        REXLOG_ERROR("native-scene: shadow atlas creation failed");
        g_r.failed = true;
        return false;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += size_t(2 + t) * g_r.rtv_size;
      device->CreateRenderTargetView(*targets[t], nullptr, rtv);
      *srv_slots[t] = g_r.srv_next++;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = desc.Format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
      slot.ptr += size_t(*srv_slots[t]) * g_r.srv_size;
      device->CreateShaderResourceView(*targets[t], &srv, slot);
    }
    g_r.shadow_in_srv_state = false;
    REXLOG_INFO("native-scene: shadow atlas created ({}x{} tiles)", g_r.shadow_tile,
                g_r.shadow_tile);
  }
  if (!g_r.shadow_cb) {
    // Always created (even with shadows off): the scene PS declares b1 and
    // a root CBV must be bound; a zeroed block disables the shadow branch.
    g_r.shadow_cb = CreateUploadBuffer(device, 256u * RendererState::kShadowCbRegions);
    if (!g_r.shadow_cb ||
        FAILED(g_r.shadow_cb->Map(0, nullptr,
                                  reinterpret_cast<void**>(&g_r.shadow_cb_cpu)))) {
      g_r.failed = true;
      return false;
    }
  }

  if (g_r.blur_tex[0] == nullptr && g_r.pso_blur != nullptr) {
    // Popup background blur intermediates at the game's fixed 1152x640
    // internal resolution (the bilinear stretch back to the output is what
    // produces the authentic frosted-glass lattice). RTV heap slots 5/6.
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = RendererState::kBlurWidth;
    desc.Height = RendererState::kBlurHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = context.d3d12.guest_output_format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    bool ok = true;
    for (int t = 0; t < 2 && ok; ++t) {
      if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(&g_r.blur_tex[t])))) {
        ok = false;
        break;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += size_t(5 + t) * g_r.rtv_size;
      device->CreateRenderTargetView(g_r.blur_tex[t], nullptr, rtv);
      g_r.blur_srv[t] = g_r.srv_next++;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = desc.Format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
      slot.ptr += size_t(g_r.blur_srv[t]) * g_r.srv_size;
      device->CreateShaderResourceView(g_r.blur_tex[t], &srv, slot);
    }
    if (ok && !g_r.output_srv_allocated) {
      g_r.output_srv_slot = g_r.srv_next++;
      g_r.output_srv_allocated = true;
    }
    if (!ok) {
      REXLOG_ERROR("native-scene: blur intermediate creation failed; blur disabled");
      for (int t = 0; t < 2; ++t) {
        if (g_r.blur_tex[t]) g_r.blur_tex[t]->Release();
        g_r.blur_tex[t] = nullptr;
      }
      g_r.pso_blur = nullptr;  // leaked PSO acceptable on this cold path
    }
  }

  if ((g_r.outline_mask == nullptr ||
       g_r.outline_mask_width != context.guest_output_width ||
       g_r.outline_mask_height != context.guest_output_height) &&
      g_r.pso_outline_mask != nullptr && g_r.pso_outline_edge != nullptr) {
    // Selection-outline mask: single-sample R8 target at output resolution
    // (a 1152x640 mask left the contour centerline visibly stairstepped;
    // the mask's own rasterization aliasing survives any amount of
    // downstream filtering). RTV slot 7.
    if (g_r.outline_mask) {
      g_r.retired.emplace_back(g_r.outline_mask,
                               context.d3d12.command_processor->GetCurrentSubmission());
      g_r.outline_mask = nullptr;
    }
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = context.guest_output_width;
    desc.Height = context.guest_output_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                               IID_PPV_ARGS(&g_r.outline_mask)))) {
      REXLOG_ERROR("native-scene: outline mask creation failed; outline disabled");
      g_r.pso_outline_edge = nullptr;
    } else {
      g_r.outline_mask_width = context.guest_output_width;
      g_r.outline_mask_height = context.guest_output_height;
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += size_t(7) * g_r.rtv_size;
      device->CreateRenderTargetView(g_r.outline_mask, nullptr, rtv);
      if (!g_r.outline_mask_srv_allocated) {
        g_r.outline_mask_srv = g_r.srv_next++;
        g_r.outline_mask_srv_allocated = true;
      }
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = desc.Format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
      slot.ptr += size_t(g_r.outline_mask_srv) * g_r.srv_size;
      device->CreateShaderResourceView(g_r.outline_mask, &srv, slot);
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
  if (!g_r.white_cube.valid) {
    // 1x1x6 mid-gray fallback cube for the water reflection slot (t6): a
    // TextureCube SRV must always be bound where the shader declares one.
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&g_r.white_cube.texture)))) {
      g_r.failed = true;
      return false;
    }
    g_r.white_cube.upload = CreateUploadBuffer(device, 512 * 6);
    if (!g_r.white_cube.upload) {
      g_r.failed = true;
      return false;
    }
    uint8_t* mapping = nullptr;
    g_r.white_cube.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapping));
    for (uint32_t f = 0; f < 6; ++f) {
      std::memset(mapping + f * 512, 0x80, 4);
    }
    g_r.white_cube.upload->Unmap(0, nullptr);
    auto& list = context.d3d12.command_processor->GetDeferredCommandList();
    for (uint32_t f = 0; f < 6; ++f) {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = g_r.white_cube.texture;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = f;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = g_r.white_cube.upload;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = f * 512;
      src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      src.PlacedFootprint.Footprint.Width = 1;
      src.PlacedFootprint.Footprint.Height = 1;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = 256;
      list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          g_r.white_cube.texture,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_r.white_cube.srv_slot = g_r.srv_next++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(g_r.white_cube.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(g_r.white_cube.texture, &srv, slot);
    g_r.white_cube.valid = true;
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
    desc.SampleDesc.Count = g_r.msaa;
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

    if (g_r.msaa > 1) {
      // MSAA color target (RTV heap slot 1) + its Texture2DMS SRV for the
      // fullscreen resolve pass. Lives in RENDER_TARGET state between frames.
      if (g_r.msaa_color) {
        g_r.retired.emplace_back(g_r.msaa_color,
                                 context.d3d12.command_processor->GetCurrentSubmission());
        g_r.msaa_color = nullptr;
      }
      D3D12_RESOURCE_DESC cdesc = desc;
      cdesc.Format = context.d3d12.guest_output_format;
      cdesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_CLEAR_VALUE cclear{};
      cclear.Format = cdesc.Format;
      cclear.Color[0] = 0.25f;
      cclear.Color[1] = 0.35f;
      cclear.Color[2] = 0.55f;
      cclear.Color[3] = 1.0f;
      if (FAILED(g_r.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &cdesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
              &cclear, IID_PPV_ARGS(&g_r.msaa_color)))) {
        g_r.failed = true;
        return false;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE msaa_rtv =
          g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      msaa_rtv.ptr += g_r.rtv_size;
      device->CreateRenderTargetView(g_r.msaa_color, nullptr, msaa_rtv);
      if (!g_r.msaa_srv_allocated) {
        g_r.msaa_srv_slot = g_r.srv_next++;
        g_r.msaa_srv_allocated = true;
      }
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = cdesc.Format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
      slot.ptr += size_t(g_r.msaa_srv_slot) * g_r.srv_size;
      device->CreateShaderResourceView(g_r.msaa_color, &srv, slot);
    }
  }

  if (g_r.rtv_resource != context.d3d12.guest_output_resource) {
    device->CreateRenderTargetView(context.d3d12.guest_output_resource, nullptr,
                                   g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart());
    g_r.rtv_resource = context.d3d12.guest_output_resource;
  }
  return true;
}

// ---- Warm decode (loading-screen prewarm + gameplay warmup) --------------
// Decodes, within `deadline`, every GPU resource `item`'s draw would
// resolve: the mesh VB/IB (with fingerprint revalidation) and each texture
// slot its material binds. Mirrors the draw loop's miss paths, including
// negative-caching of failed texture decodes and retiring replaced
// resources, so the caches end up exactly as drawing would leave them and
// the takeover frame has nothing left to pay. Work past the deadline counts
// as deferred. Cached textures get ONE content revalidation per warmup
// (recheck_frame gate; the frame counter is frozen while yielded): a
// texture pre-decoded while its payload was still streaming in is healed
// here, under budget, instead of by an unbudgeted re-decode burst on the
// takeover frame.
struct WarmCounters {
  uint32_t decodes = 0;
  uint32_t deferred = 0;
};

void WarmItemResources(const NativeGuestOutputRenderContext& context, uint8_t* base,
                       uint64_t frame_number, const DrawItem& item,
                       std::chrono::steady_clock::time_point deadline,
                       WarmCounters& wc) {
  auto* command_processor = context.d3d12.command_processor;
  const auto within = [&] { return std::chrono::steady_clock::now() < deadline; };

  bool need_mesh = false;
  auto mit = g_r.meshes.find(item.mesh);
  if (mit == g_r.meshes.end()) {
    need_mesh = true;
  } else if (mit->second.fingerprint != item.fingerprint &&
             REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
    // Streaming filled/replaced the payload after an earlier (pre)decode.
    if (within()) {
      const uint64_t submission = command_processor->GetCurrentSubmission();
      g_r.retired.emplace_back(mit->second.vb, submission);
      g_r.retired.emplace_back(mit->second.ib, submission);
      g_r.meshes.erase(mit);
      need_mesh = true;
    } else {
      ++wc.deferred;
    }
  }
  if (need_mesh) {
    if (!within()) {
      ++wc.deferred;
    } else {
      ++wc.decodes;
      MeshBuffers buffers;
      if (DecodeMesh(g_r.device, base, item, buffers)) {
        buffers.fingerprint = item.fingerprint;
        g_r.meshes.emplace(item.mesh, buffers);
      }
      // Failures retry through the draw path (logged + counted there).
    }
  }

  const auto warm_texture = [&](uint32_t tex_ptr) {
    if (tex_ptr == 0) {
      return;
    }
    auto it = g_r.textures.find(tex_ptr);
    if (it != g_r.textures.end()) {
      if (!it->second.valid || frame_number < it->second.recheck_frame ||
          !REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        return;  // negative caches retry via the draw path's schedule
      }
      if (!within()) {
        ++wc.deferred;
        return;
      }
      it->second.recheck_frame = frame_number + 16;
      const uint64_t fp = SamplePayloadFingerprint(base, it->second.payload_addr,
                                                   it->second.payload_size);
      if (fp == 0 || fp == it->second.payload_fp) {
        return;
      }
      RetireGuestTexture(it->second, command_processor->GetCurrentSubmission());
      g_r.textures.erase(it);
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTexture(context, base, tex_ptr, gt);
    if (!gt.valid) {
      // Negative-cache exactly like the draw path so a permanently
      // unreadable texture cannot hold warmup open. Guarded reads: a decode
      // can fail precisely BECAUSE the object is unreadable right now (the
      // prewarm sees objects mid-load); a raw re-read would fault.
      if (gt.fetch_words[0] == 0 && gt.fetch_words[1] == 0) {
        for (uint32_t k = 0; k < 6; ++k) {
          uint32_t w = 0;
          if (!GuestTryLoadU32(base, tex_ptr + (7 + k) * 4, &w)) {
            break;
          }
          gt.fetch_words[k] = w;
        }
      }
      gt.retry_after_frame = frame_number + 120;
    }
    g_r.textures.emplace(tex_ptr, gt);
  };
  // Draw-time fetch-word bindings (streamed artwork / decal ad overrides)
  // share the words-keyed cache with the 2D pass.
  const auto warm_fetch_words = [&](const uint32_t words[6]) {
    if (words[1] == 0) {
      return;
    }
    const uint64_t fkey = FetchWordsKey(words);
    if (g_r.textures_2d.contains(fkey)) {
      return;
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    g_r.textures_2d.emplace(fkey, gt);
  };

  warm_fetch_words(item.diffuse_fetch);
  warm_texture(item.diffuse_tex);
  if (REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
    warm_texture(item.lightmap_tex);
  }
  if (REXCVAR_GET(skate3_native_render_scene_macro)) {
    warm_texture(item.macro_tex);
  }
  if (item.water || item.char_family >= 6) {
    warm_texture(item.water_normal);
  }
  if (item.decal && REXCVAR_GET(skate3_native_render_scene_decals)) {
    warm_texture(item.decal_art);
    warm_fetch_words(item.decal_fetch);
  }
  if (item.char_family >= 4 && item.char_family <= 5) {
    warm_texture(item.hair_alpha_tex);
  }
  if (item.env_family != 0 && !item.decal && item.env_family != 10) {
    warm_texture(item.spec_tex);
  }
  // Environment cube (negative-cached like the draw path).
  if ((item.water || item.char_family >= 6 ||
       (item.env_family >= 5 && item.env_family <= 6)) &&
      item.water_env != 0 && !g_r.cube_textures.contains(item.water_env)) {
    if (!within()) {
      ++wc.deferred;
    } else {
      ++wc.decodes;
      GuestTexture c{};
      if (!EnsureGuestCubeTexture(context, base, item.water_env, c)) {
        if (c.upload) c.upload->Release();
        if (c.texture) c.texture->Release();
        c = GuestTexture{};
        c.valid = false;
      }
      g_r.cube_textures.emplace(item.water_env, c);
    }
  }
}

// ---- Steady-state miss routing (render thread -> decode workers) ----------
// Draw-path cache misses for STATIC content (world meshes, material
// textures) enqueue here instead of decoding inline on the render thread:
// the item skips / renders white / keeps its previous decode for the 1-3
// frames the workers need, instead of stalling the frame for the decode
// (measured ~10 ms avg, ~70 ms max per texture, the panning lag spikes).
// Dynamic payloads (skinned, cloth, ropa) still decode inline: their buffers
// change every frame, so an async result would always be stale.
void EnqueueMeshMiss(uint32_t mesh) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_prewarm_queue.size() < 65536 && g_miss_inflight_mesh.insert(mesh).second) {
    g_prewarm_queue.push_back({mesh, 8});
    g_prewarm_cv.notify_one();
  }
}

void EnqueueTexMiss(uint32_t tex) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_prewarm_queue.size() < 65536 && g_miss_inflight_tex.insert(tex).second) {
    g_prewarm_queue.push_back({0, 0, tex});
    g_prewarm_cv.notify_one();
  }
}

// Words-keyed texture miss (streamed-artwork posters / event ads): the art
// exists only as draw-time fetch words. Decoded unbudgeted inline these were
// a traversal hitch (a poster decode costs the same ~10 ms as any texture);
// while a decode is in flight the item falls back to its channel diffuse
// (the placeholder poster), not white.
void EnqueueWordsMiss(uint64_t key, const uint32_t words[6]) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_prewarm_queue.size() < 65536 && g_miss_inflight_words.insert(key).second) {
    PrewarmEntry e{0, 0, 0, key};
    std::memcpy(e.words, words, sizeof(e.words));
    g_prewarm_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// Environment-cube miss: one cube decode measured up to ~100 ms inline;
// the gray fallback cube shows for the 1-3 frames the workers need instead.
void EnqueueCubeMiss(uint32_t tex) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_prewarm_queue.size() < 65536 && g_miss_inflight_tex.insert(tex).second) {
    PrewarmEntry e{0, 0, tex};
    e.cube = true;
    g_prewarm_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// ---- Prewarm decode workers -----------------------------------------------
// Process one registered mesh on a worker: build the item (same walk and
// payload fingerprint as the capture, so cache entries are identical),
// decode the mesh into upload-heap buffers, and stage every material
// texture up to a filled upload resource. Results go to g_prewarm_out for
// the render thread's commit.
void ProcessPrewarmEntry(uint8_t* base, const PrewarmEntry& e) {
  if (e.mesh == 0 && e.tex == 0 && e.wkey != 0) {
    // Words-keyed texture miss (posters/ads, see EnqueueWordsMiss): stage
    // the decode from the captured fetch words.
    StagedTexResult tr;
    tr.words_key = e.wkey;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.backend = NativeGuestOutputBackend::kD3D12;
    stage_ctx.d3d12.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, e.words, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;
    res.mesh_valid = false;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  if (e.mesh == 0 && e.tex != 0) {
    // Steady-state texture / environment-cube miss (see EnqueueTexMiss /
    // EnqueueCubeMiss): stage the decode up to a filled upload resource; the
    // commit records the GPU copies + SRV and swaps the cache entry.
    StagedTexResult tr;
    tr.key = e.tex;
    tr.cube = e.cube;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.backend = NativeGuestOutputBackend::kD3D12;
    stage_ctx.d3d12.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = e.cube ? EnsureGuestCubeTexture(stage_ctx, base, e.tex, tr.gt)
                      : EnsureGuestTexture(stage_ctx, base, e.tex, tr.gt);
    g_tex_stage_out = nullptr;
    if (!tr.valid && tr.gt.fetch_words[0] == 0 && tr.gt.fetch_words[1] == 0) {
      for (uint32_t k = 0; k < 6; ++k) {
        uint32_t w = 0;
        if (!GuestTryLoadU32(base, e.tex + (7 + k) * 4, &w)) {
          break;
        }
        tr.gt.fetch_words[k] = w;
      }
    }
    PrewarmResult res;
    res.item.mesh = 0;  // texture-only result (DrawItem::mesh has no default)
    res.mesh_valid = false;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  uint8_t record[0x60];
  DrawItem item{};
  if (!GuestTryCopy(record, base + e.mesh, sizeof(record)) ||
      !BuildItemFromMesh(base, e.mesh, item)) {
    // Buffer objects can finish initializing shortly after registration;
    // retries are re-injected frame-paced by the render thread.
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
      if (e.retries > 0) {
        g_prewarm_retry.push_back({e.mesh, uint16_t(e.retries - 1)});
      } else {
        g_prewarm_dropped.fetch_add(1, std::memory_order_relaxed);
        dropped = true;
      }
    }
    if (dropped) {
      // A dropped draw-path miss must leave the in-flight set so a later
      // frame can retry it (the payload may finish streaming in).
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      g_miss_inflight_mesh.erase(e.mesh);
    }
    return;
  }
  // One representative draw entry so DecodeMesh's two-sided-sheet detection
  // keys off the real primitive type (empty draw lists would pass it
  // unconditionally). Guarded: island params live outside the validated
  // mesh record.
  const uint32_t num_islands = REX_LOAD_U32(e.mesh + 0x38);
  const uint32_t island_params = REX_LOAD_U32(e.mesh + 0x44);
  uint32_t prim0 = 0;
  if (num_islands != 0 && num_islands < 4096 &&
      GuestTryLoadU32(base, island_params, &prim0)) {
    item.draws.push_back(DrawEntry{prim0, 0, 0, item.ib_count});
  }

  PrewarmResult res;
  res.mesh_valid = DecodeMesh(g_r.device, base, item, res.buffers);
  if (res.mesh_valid) {
    res.buffers.fingerprint = item.fingerprint;
  }

  // Stage the material textures (cube maps stay on the render thread:
  // rare, and their decode has its own path). Dedupe through the shared
  // per-load set: workers cannot read the render thread's g_r caches, so a
  // texture cached from a previous map decodes once more per load; the
  // commit discards the duplicate.
  const auto stage_texture = [&](uint32_t tex_ptr) {
    if (tex_ptr == 0) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      if (!g_prewarm_tex_seen.insert(tex_ptr).second) {
        return;
      }
    }
    StagedTexResult tr;
    tr.key = tex_ptr;
    // Staged mode uses only context.d3d12.device (copies/barrier/SRV are
    // exported for the commit), so a device-only context suffices.
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.backend = NativeGuestOutputBackend::kD3D12;
    stage_ctx.d3d12.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTexture(stage_ctx, base, tex_ptr, tr.gt);
    g_tex_stage_out = nullptr;
    if (!tr.valid && tr.gt.fetch_words[0] == 0 && tr.gt.fetch_words[1] == 0) {
      for (uint32_t k = 0; k < 6; ++k) {
        uint32_t w = 0;
        if (!GuestTryLoadU32(base, tex_ptr + (7 + k) * 4, &w)) {
          break;
        }
        tr.gt.fetch_words[k] = w;
      }
    }
    res.textures.push_back(std::move(tr));
  };
  stage_texture(item.diffuse_tex);
  if (REXCVAR_GET(skate3_native_render_scene_lightmaps)) {
    stage_texture(item.lightmap_tex);
  }
  if (REXCVAR_GET(skate3_native_render_scene_macro)) {
    stage_texture(item.macro_tex);
  }
  if (item.water || item.char_family >= 6) {
    stage_texture(item.water_normal);
  }
  if (item.decal && REXCVAR_GET(skate3_native_render_scene_decals)) {
    stage_texture(item.decal_art);
  }
  if (item.char_family >= 4 && item.char_family <= 5) {
    stage_texture(item.hair_alpha_tex);
  }
  if (item.env_family != 0 && !item.decal && item.env_family != 10) {
    stage_texture(item.spec_tex);
  }

  res.item = std::move(item);
  std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
  g_prewarm_out.push_back(std::move(res));
}

void PrewarmWorkerLoop() {
#if defined(_WIN32)
  // Below-normal priority: the workers flood in exactly when the guest's
  // single-threaded world ACTIVATION runs (registration is the final load
  // phase), and at normal priority they stretch the game's own black
  // window at the loading->gameplay boundary. At below-normal they only
  // soak idle cores and the guest always wins the contention.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  for (;;) {
    if (!SceneEnabled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    PrewarmEntry e{};
    DynDecodeJob dyn;
    bool have_dyn = false;
    {
      std::unique_lock<std::mutex> lock(g_prewarm_mutex);
      g_prewarm_cv.wait_for(lock, std::chrono::milliseconds(500), [] {
        return !g_prewarm_queue.empty() || !g_dyn_jobs.empty();
      });
      if (!g_dyn_jobs.empty()) {
        // Dynamic cloth first: these are per-frame payloads whose result
        // should land at the very next commit.
        dyn = std::move(g_dyn_jobs.front());
        g_dyn_jobs.erase(g_dyn_jobs.begin());
        have_dyn = true;
      } else if (!g_prewarm_queue.empty()) {
        e = g_prewarm_queue.back();
        g_prewarm_queue.pop_back();
      } else {
        continue;
      }
    }
    uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
    if (base == nullptr || g_r.device == nullptr) {
      if (!have_dyn) {
        std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
        g_prewarm_retry.push_back(e);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (have_dyn) {
      PrewarmResult res;
      res.mesh_valid =
          DecodeMesh(g_r.device, base, dyn.item, res.buffers, dyn.vb.data(),
                     dyn.ib.empty() ? nullptr : dyn.ib.data());
      if (res.mesh_valid) {
        res.buffers.fingerprint = dyn.item.fingerprint;
        res.buffers.dyn_seq = dyn.seq;
        res.item = std::move(dyn.item);
        std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
        g_prewarm_out.push_back(std::move(res));
      }
      continue;
    }
    ProcessPrewarmEntry(base, e);
  }
}

// Lazily start the decode workers (process-lifetime, parked on the queue's
// condition variable when idle). Only started once the pipeline exists;
// the workers create D3D12 resources through g_r.device.
void EnsurePrewarmWorkers() {
  if (g_r.device == nullptr ||
      g_prewarm_workers_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  const unsigned hw = std::max(4u, std::thread::hardware_concurrency());
  const unsigned n = std::clamp(hw / 4u, 2u, 4u);
  for (unsigned i = 0; i < n; ++i) {
    std::thread(PrewarmWorkerLoop).detach();
  }
  REXLOG_INFO("native-scene: {} prewarm decode workers started", n);
}

// Render-thread commit of the workers' results: record GPU copies +
// barriers, create SRVs, insert into the caches, re-inject retries.
// Microseconds per item, safe to run every frame (loading, settle and
// gameplay alike).
void PrewarmCommit(const NativeGuestOutputRenderContext& context,
                   uint64_t frame_number, bool loading = false) {
  {
    std::scoped_lock lock(g_prewarm_out_mutex, g_prewarm_mutex);
    if (!g_prewarm_retry.empty()) {
      g_prewarm_queue.insert(g_prewarm_queue.end(), g_prewarm_retry.begin(),
                             g_prewarm_retry.end());
      g_prewarm_retry.clear();
      g_prewarm_cv.notify_all();
    }
  }
  std::vector<PrewarmResult> done;
  {
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    // Soft cap per frame: during the activation burst the workers can
    // complete hundreds of items between two refreshes, and committing
    // them all at once records thousands of copies + SRVs into a single
    // frame (measured ~96 us per item; a 256 batch is ~25 ms). Behind a
    // loading screen that is fine; in gameplay the cap stays small so a
    // streaming burst spreads over a few frames instead of hitching one.
    const size_t kMaxCommitPerFrame = loading ? 256 : 32;
    if (g_prewarm_out.size() <= kMaxCommitPerFrame) {
      done.swap(g_prewarm_out);
    } else {
      done.assign(std::make_move_iterator(g_prewarm_out.end() - kMaxCommitPerFrame),
                  std::make_move_iterator(g_prewarm_out.end()));
      g_prewarm_out.resize(g_prewarm_out.size() - kMaxCommitPerFrame);
    }
  }
  if (done.empty()) {
    return;
  }
  const auto commit_t0 = PerfClock::now();
  bool committed_tex = false;
  auto* command_processor = context.d3d12.command_processor;
  for (PrewarmResult& r : done) {
    if (r.mesh_valid) {
      auto mit = g_r.meshes.find(r.item.mesh);
      if (mit != g_r.meshes.end() &&
          (mit->second.fingerprint == r.buffers.fingerprint ||
           mit->second.dyn_seq > r.buffers.dyn_seq)) {
        // Identical content already cached (lost the race against the draw
        // path / an earlier result), or a NEWER dynamic decode already
        // landed (multi-worker reordering must not step the cloth
        // backwards): the staged buffers were never referenced by any
        // submission.
        r.buffers.vb->Release();
        r.buffers.ib->Release();
      } else {
        if (mit != g_r.meshes.end()) {
          // Stale decode (miss-driven revalidation heal): swap it out. The
          // old buffers may be referenced by the in-flight submission.
          const uint64_t submission = command_processor->GetCurrentSubmission();
          g_r.retired.emplace_back(mit->second.vb, submission);
          g_r.retired.emplace_back(mit->second.ib, submission);
          g_r.meshes.erase(mit);
        }
        g_r.meshes.emplace(r.item.mesh, r.buffers);
      }
    }
    for (StagedTexResult& t : r.textures) {
      if (t.cube) {
        // Environment cube: lands in the cube cache. Cubes are static
        // assets: an existing valid entry wins; a failed decode
        // negative-caches like the old inline path did.
        auto cit = g_r.cube_textures.find(t.key);
        if (cit != g_r.cube_textures.end() && cit->second.valid) {
          if (t.gt.texture) t.gt.texture->Release();
          if (t.gt.upload) t.gt.upload->Release();
          continue;
        }
        if (cit != g_r.cube_textures.end()) {
          RetireGuestTexture(cit->second, command_processor->GetCurrentSubmission());
          g_r.cube_textures.erase(cit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
        }
        g_r.cube_textures.emplace(t.key, t.gt);
        continue;
      }
      if (t.words_key != 0) {
        // Words-keyed result (posters/ads): lands in the 2D/words cache.
        auto wit = g_r.textures_2d.find(t.words_key);
        if (wit != g_r.textures_2d.end()) {
          const bool same_content = t.valid && wit->second.valid &&
                                    t.gt.payload_fp == wit->second.payload_fp;
          if (same_content || (!t.valid && wit->second.valid)) {
            if (t.gt.texture) t.gt.texture->Release();
            if (t.gt.upload) t.gt.upload->Release();
            continue;
          }
          RetireGuestTexture(wit->second, command_processor->GetCurrentSubmission());
          g_r.textures_2d.erase(wit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
        } else {
          t.gt.retry_after_frame = frame_number + 120;
        }
        g_r.textures_2d.emplace(t.words_key, t.gt);
        continue;
      }
      auto tit = g_r.textures.find(t.key);
      if (tit != g_r.textures.end()) {
        const bool same_content =
            t.valid && tit->second.valid &&
            std::memcmp(t.gt.fetch_words, tit->second.fetch_words,
                        sizeof(t.gt.fetch_words)) == 0 &&
            t.gt.payload_fp == tit->second.payload_fp;
        if (same_content || (!t.valid && tit->second.valid)) {
          // The cached decode is as good or better ("keep the old decode
          // when the payload became unreadable": mips stream out at range).
          // A FAILED fresh decode backs the surviving entry's retry clock
          // off (+30): the payload usually stays unreadable for a while
          // (mid-stream upload), and the un-throttled loop re-queued a
          // doomed decode every 4 frames for its whole duration.
          if (!t.valid) {
            tit->second.retry_after_frame = frame_number + 30;
          }
          if (t.gt.texture) t.gt.texture->Release();
          if (t.gt.upload) t.gt.upload->Release();
          continue;
        }
        // Miss-driven revalidation heal (words/payload changed, or a failed
        // entry that now decodes): swap the entry, retiring the old decode.
        RetireGuestTexture(tit->second, command_processor->GetCurrentSubmission());
        g_r.textures.erase(tit);
      }
      if (t.valid) {
        CommitStagedGuestTexture(context, t.gt, t.commit);
        committed_tex = true;
      } else {
        t.gt.retry_after_frame = frame_number + 120;
        // Failed decodes render white; log each once (capped) so white
        // meshes stay attributable to a specific texture.
        static std::unordered_set<uint32_t> logged_failed;
        if (logged_failed.size() < 64 && logged_failed.insert(t.key).second) {
          REXLOG_INFO(
              "native-scene: texture decode FAILED obj={:08X} fetch=[{:08X} {:08X} "
              "{:08X} {:08X} {:08X} {:08X}]",
              t.key, t.gt.fetch_words[0], t.gt.fetch_words[1], t.gt.fetch_words[2],
              t.gt.fetch_words[3], t.gt.fetch_words[4], t.gt.fetch_words[5]);
        }
      }
      g_r.textures.emplace(t.key, t.gt);
    }
    g_prewarm_done.fetch_add(1, std::memory_order_relaxed);
  }
  // Release the miss-in-flight keys so later revalidation cycles can enqueue
  // these again (erasing keys that were never in the sets is harmless).
  {
    std::lock_guard<std::mutex> lock(g_prewarm_mutex);
    for (const PrewarmResult& r : done) {
      if (r.item.mesh != 0) {
        g_miss_inflight_mesh.erase(r.item.mesh);
      }
      for (const StagedTexResult& t : r.textures) {
        if (t.words_key != 0) {
          g_miss_inflight_words.erase(t.words_key);
        } else {
          g_miss_inflight_tex.erase(t.key);
        }
      }
    }
  }
  if (committed_tex) {
    context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
  }
  g_pw_commit.Add(uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - commit_t0)
          .count()));
}

bool RenderScene(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  if (!SceneEnabled() || context.backend != NativeGuestOutputBackend::kD3D12) {
    return false;
  }
  // While the game reports menus / pause / loading (presence context 0x8001
  // == 0), yield to the emulated output: the native scene neither renders
  // the 2D UI (the pause menu was invisible) nor the menu's world backdrop
  // materials (everything drew untextured white). The emulated frame is
  // complete and correct there; native rendering resumes on unpause.
  {
    static bool s_in_menus = false;
    const bool in_menus = rex::graphics::ultrawide_debug::Skate3GameplayContextValue() == 0;
    if (in_menus != s_in_menus) {
      s_in_menus = in_menus;
      if (in_menus) {
        REXLOG_INFO(
            "native-scene: menus/pause/loading - yielding to emulated output "
            "(presence context)");
        // Arena addresses are reused across map loads; let the next load's
        // registrations re-queue meshes (and re-stage textures) at reused
        // addresses, and drop the cached item cores built from them.
        ClearItemCache();
        std::lock_guard<std::mutex> lock(g_prewarm_mutex);
        g_prewarm_seen.clear();
        g_prewarm_tex_seen.clear();
      } else {
        size_t queued = 0;
        {
          std::lock_guard<std::mutex> lock(g_prewarm_mutex);
          queued = g_prewarm_queue.size();
        }
        REXLOG_INFO(
            "native-scene: gameplay (prewarm: {} meshes decoded, {} dropped, {} still "
            "queued)",
            g_prewarm_done.load(std::memory_order_relaxed),
            g_prewarm_dropped.load(std::memory_order_relaxed), queued);
      }
    }
    if (in_menus) {
      // Re-arm the takeover gate for the next stretch of gameplay (map
      // load, unpause).
      g_warmup_armed.store(true, std::memory_order_relaxed);
      {
        // Freshness gate: only scenes published AFTER this point qualify;
        // g_scene still holds the previous map's last scene all through the
        // loading screen (BuildFrameScene stops publishing without a
        // perspective view).
        std::lock_guard<std::mutex> lock(g_scene_mutex);
        g_warmup_fresh_generation = (g_scene ? g_scene->generation : 0) + 1;
      }
      // Build the pipelines / render targets behind the loading screen so
      // the one-time PSO compilation (~200 ms) never lands on a gameplay
      // frame.
      if (!g_r.failed && g_r.pso == nullptr) {
        EnsurePipeline(context);
      }
      // THE loading-screen heavy lifting runs on the prewarm decode WORKER
      // POOL (a serial render-thread drain both tanked the loading spinner
      // to ~13 fps and still left 1500 of a map's ~2600 meshes undecoded at
      // takeover; map-change loads register most of the world in their
      // final seconds and drop the guest to 10-25 fps while doing it).
      // Here the render thread only commits finished results: record the
      // GPU copies, create SRVs, insert into the caches.
      if (REXCVAR_GET(skate3_native_render_scene_prewarm_budget_ms) > 0 &&
          !g_r.failed && g_r.pso != nullptr) {
        EnsurePrewarmWorkers();
        PrewarmCommit(context, g_frames_rendered.load(std::memory_order_relaxed),
                      /*loading=*/true);
      }
      return false;
    }
  }
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return false;
  }

  const auto render_t0 = PerfClock::now();
  const auto perf_ns_since = [](PerfClock::time_point t0) {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
            .count());
  };

  std::shared_ptr<const FrameScene> scene_ptr;
  {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    if (!g_scene || g_scene->items.empty()) {
      return false;
    }
    scene_ptr = g_scene;
  }
  const FrameScene& scene = *scene_ptr;

  if (!EnsurePipeline(context)) {
    return false;
  }
  // Flush any barriers pushed by lazy resource creation (white texture).
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  auto* command_processor = context.d3d12.command_processor;
  auto& list = command_processor->GetDeferredCommandList();

  // Free retired buffers (and recycle retired SRV slots) whose
  // last-referencing submission has completed.
  if (!g_r.retired.empty() || !g_r.retired_srv_slots.empty()) {
    const uint64_t completed = command_processor->GetCompletedSubmission();
    std::erase_if(g_r.retired, [completed](const auto& entry) {
      if (entry.second < completed) {
        entry.first->Release();
        return true;
      }
      return false;
    });
    std::erase_if(g_r.retired_srv_slots, [completed](const auto& entry) {
      if (entry.second < completed) {
        g_r.srv_free.push_back(entry.first);
        return true;
      }
      return false;
    });
  }

  // Debug-dialog cache flushes: retire every cached decode (freed once the
  // GPU is done with the current submission) so hot-toggled decode settings
  // rebuild the world with the new rules this frame.
  if (g_flush_textures.exchange(false, std::memory_order_relaxed)) {
    const uint64_t submission = command_processor->GetCurrentSubmission();
    for (auto& [key, t] : g_r.textures) {
      RetireGuestTexture(t, submission);
    }
    g_r.textures.clear();
    for (auto& [key, t] : g_r.textures_2d) {
      RetireGuestTexture(t, submission);
    }
    g_r.textures_2d.clear();
    REXLOG_INFO("native-scene: texture cache flushed (debug dialog)");
  }
  if (g_flush_meshes.exchange(false, std::memory_order_relaxed)) {
    const uint64_t submission = command_processor->GetCurrentSubmission();
    for (auto& [key, m] : g_r.meshes) {
      if (m.vb) g_r.retired.emplace_back(m.vb, submission);
      if (m.ib) g_r.retired.emplace_back(m.ib, submission);
    }
    g_r.meshes.clear();
    ClearItemCache();  // decode-affecting toggles should re-walk items too
    REXLOG_INFO("native-scene: mesh cache flushed (debug dialog)");
  }

  // Reset this frame's bone ring region (shared by the shadow casters and
  // the main pass: the shadow pass allocates first, the main pass appends).
  const uint64_t frame_number = g_frames_rendered.load(std::memory_order_relaxed);
  const uint32_t bone_region =
      uint32_t(frame_number % RendererState::kBoneRegions) *
      RendererState::kBoneRegionSize;
  g_r.bone_ring_offset = 0;

  // ---- Loading -> gameplay takeover (seamless boot / map-change loads) ----
  // The loading-screen prewarm (menus branch above) already decoded the
  // registered world behind the load; the FIRST substantial post-load scene
  // renders natively right away. Gates kept: staleness (g_scene holds the
  // PREVIOUS map's scene through the whole load; rendering it shows
  // old-map garbage) and min items (the capture holds a near-empty scene
  // for a few frames while the game fades in; taking over there shows a
  // black/void world; the brief yield shows the game's own fade instead).
  // The frames after takeover run a budgeted settle pass for whatever
  // prewarm missed (dynamic entities, late textures), and the draw path's
  // miss budgets are clamped while settling so leftovers render white/skip
  // for a frame instead of freezing the takeover frame.
  const int32_t warmup_ms = REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms);
  bool settling = false;
  if (warmup_ms > 0 && REXCVAR_GET(skate3_native_render_scene_debug) == 0) {
    if (g_warmup_armed.load(std::memory_order_relaxed)) {
      if (scene.generation < g_warmup_fresh_generation ||
          scene.items.size() <
              size_t(REXCVAR_GET(skate3_native_render_scene_warmup_min_items))) {
        // Stale or fade-in scene: yield (brief, a few frames). Keep
        // committing worker results meanwhile; every pre-takeover frame
        // counts on map changes.
        PrewarmCommit(context, frame_number);
        return false;
      }
      g_warmup_armed.store(false, std::memory_order_relaxed);
      g_settle_until_frame = frame_number + 120;
      size_t queued = 0;
      {
        std::lock_guard<std::mutex> lock(g_prewarm_mutex);
        queued = g_prewarm_queue.size();
      }
      REXLOG_INFO(
          "native-scene: taking over natively ({} items; prewarm {} done / {} dropped "
          "/ {} queued)",
          scene.items.size(), g_prewarm_done.load(std::memory_order_relaxed),
          g_prewarm_dropped.load(std::memory_order_relaxed), queued);
    }
    settling = frame_number < g_settle_until_frame;
    if (settling) {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
      WarmCounters wc;
      for (const DrawItem& item : scene.items) {
        WarmItemResources(context, base, frame_number, item, deadline, wc);
      }
      if (wc.deferred != 0) {
        // Still behind: keep the settle pass (and the draw-path budget
        // clamp) alive until a frame clears with budget to spare.
        g_settle_until_frame =
            std::max<uint64_t>(g_settle_until_frame, frame_number + 8);
      }
      if (wc.decodes > 0) {
        context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
      }
    }
  }

  if (!g_r.announced) {
    g_r.announced = true;
    REXLOG_INFO("native-scene: rendering natively ({} items, {}x{})", scene.items.size(),
                context.guest_output_width, context.guest_output_height);
  }

  // Commit finished worker decodes every frame (streamed arenas decode on
  // the workers before their first draw instead of hitching the first frame
  // that sees them). Near-no-op when nothing completed. The workers also
  // serve the draw path's steady-state misses (EnqueueMeshMiss/EnqueueTexMiss),
  // so make sure they exist even on a session that never showed a loading
  // screen with the pipeline up.
  EnsurePrewarmWorkers();
  PrewarmCommit(context, frame_number);

  // ---- Dynamic-shadow atlas pass ----
  // Renders the frame's dynamic casters (skinned characters + rigid
  // non-identity-world props: exactly the game's caster list) into the
  // three cascade tiles with the captured light rows, then applies the
  // game's coverage blur + depth dilation. Runs before the main pass so the
  // scene shader can sample the finished atlas.
  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  const auto shadow_t0 = PerfClock::now();
  const float* sh = scene.shadow_rows;
  const int32_t debug_mode = REXCVAR_GET(skate3_native_render_scene_debug);
  if (REXCVAR_GET(skate3_native_render_scene_shadows) && scene.shadow_valid &&
      g_r.shadow_raw != nullptr && g_r.pso_shadow_caster != nullptr &&
      g_r.pso_shadow_blur != nullptr && debug_mode == 0) {
    struct Caster {
      const DrawItem* item;
      uint32_t bone_offset;
      bool bones;
    };
    std::vector<Caster> casters;
    static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (const DrawItem& item : scene.items) {
      if (item.transparent || item.unlit || item.cloth_quads) {
        continue;
      }
      const bool skinned = item.skinned && !item.bones.empty();
      if (!skinned && std::memcmp(item.world, kIdent, sizeof(kIdent)) == 0) {
        continue;  // static world geometry never casts (baked into lightmaps)
      }
      // NO inline decode here (this block used to re-decode every cloth
      // garment every frame, ~2.9 ms, the 160 fps cap during real play).
      // The dyn decode jobs / worker miss queue keep the cache fresh, one
      // frame behind the sim; a first-sight caster shadows 1-2 frames late.
      if (!g_r.meshes.contains(item.mesh)) {
        continue;
      }
      Caster c{&item, 0, false};
      if (skinned) {
        const uint32_t bytes = uint32_t(item.bones.size() * sizeof(float));
        const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
        if (offset + bytes > RendererState::kBoneRegionSize) {
          continue;
        }
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.bones.data(), bytes);
        g_r.bone_ring_offset = offset + bytes;
        c.bone_offset = offset;
        c.bones = true;
      }
      casters.push_back(c);
    }
    if (!casters.empty()) {
      if (g_r.shadow_in_srv_state) {
        for (ID3D12Resource* res : {g_r.shadow_raw, g_r.shadow_mid, g_r.shadow_final}) {
          context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                                res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
        g_r.shadow_in_srv_state = false;
      }
      const uint32_t tile = g_r.shadow_tile;
      D3D12_CPU_DESCRIPTOR_HANDLE raw_rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      raw_rtv.ptr += size_t(2) * g_r.rtv_size;
      const FLOAT shadow_clear[4] = {1.0f, 1.0f, 0.0f, 0.0f};
      list.D3DClearRenderTargetView(raw_rtv, shadow_clear, 0, nullptr);
      list.D3DOMSetRenderTargets(1, &raw_rtv, FALSE, nullptr);
      list.D3DSetGraphicsRootSignature(g_r.root_signature);
      list.D3DSetPipelineState(g_r.pso_shadow_caster);
      list.SetDescriptorHeaps(g_r.srv_heap, nullptr);
      // Unused by the caster shaders, but never leave root CBVs unset.
      list.D3DSetGraphicsRootConstantBufferView(
          9, g_r.bone_ring->GetGPUVirtualAddress() + bone_region);
      for (int ci = 0; ci < 3; ++ci) {
        // Cascade scale/offset (cascade 0 = identity; PS c1/c2 for 1/2).
        float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
        if (ci == 1) {
          sx = sh[4]; sy = sh[5]; ox = sh[6]; oy = sh[7];
        } else if (ci == 2) {
          sx = sh[8]; sy = sh[9]; ox = sh[10]; oy = sh[11];
        }
        // Light view-proj, row-vector convention: clip.x = ls_i.x,
        // clip.y = ls_i.y, clip.z = the height-ramp depth, clip.w = 1,
        // columns built from the receiver rows c0/c3/c4.
        float lightvp[16];
        for (int r = 0; r < 3; ++r) {
          lightvp[r * 4 + 0] = sh[0 + r] * sx;
          lightvp[r * 4 + 1] = sh[12 + r] * sy;
          lightvp[r * 4 + 2] = sh[16 + r];
          lightvp[r * 4 + 3] = 0.0f;
        }
        lightvp[12] = sh[3] * sx + ox;
        lightvp[13] = sh[15] * sy + oy;
        lightvp[14] = sh[19];
        lightvp[15] = 1.0f;
        D3D12_VIEWPORT vp{float(tile) * ci, 0.0f, float(tile), float(tile), 0.0f, 1.0f};
        list.RSSetViewport(vp);
        D3D12_RECT rc{LONG(tile) * ci, 0, LONG(tile) * (ci + 1), LONG(tile)};
        list.RSSetScissorRect(rc);
        for (const Caster& c : casters) {
          auto mit = g_r.meshes.find(c.item->mesh);
          if (mit == g_r.meshes.end()) {
            continue;  // undecodable this frame; casts once the workers land
          }
          // A stale fingerprint is fine here: cloth decodes ride one frame
          // behind the sim by design (dyn decode jobs).
          float constants[52] = {};
          std::memcpy(constants, c.item->world, sizeof(c.item->world));
          float* mvp = constants + 16;
          for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) {
              float sum = 0.0f;
              for (int k = 0; k < 4; ++k) {
                sum += c.item->world[r * 4 + k] * lightvp[k * 4 + col];
              }
              mvp[r * 4 + col] = sum;
            }
          }
          constants[33] = c.bones ? 1.0f : 0.0f;  // tint.g = skinned branch
          list.D3DSetGraphicsRoot32BitConstants(0, 52, constants, 0);
          list.D3DSetGraphicsRootShaderResourceView(
              3, g_r.bone_ring->GetGPUVirtualAddress() + bone_region +
                     (c.bones ? c.bone_offset : 0));
          list.D3DIASetVertexBuffers(0, 1, &mit->second.vb_view);
          list.D3DIASetIndexBuffer(&mit->second.ib_view);
          for (const DrawEntry& draw : c.item->draws) {
            if (draw.prim == 4) {
              list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            } else if (draw.prim == 6) {
              list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            } else {
              continue;
            }
            list.D3DDrawIndexedInstanced(draw.index_count, 1, draw.start_index,
                                         draw.base_vertex, 0);
            ++shadow_draws;
          }
        }
      }
      // Blur/convert chain: raw -> (hblur) -> mid -> (vblur) -> final. The
      // game's kernels: 5-tap Gaussian coverage cascade 0, 3-tap cascade 1,
      // format-convert only for cascade 2 (weights (1,0,0), 0 taps), plus
      // depth dilation into the penumbra. One fullscreen-triangle draw per
      // tile per direction, taps clamped inside the tile.
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                            g_r.shadow_raw, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
      list.D3DSetPipelineState(g_r.pso_shadow_blur);
      list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      const float kernels[3][3] = {{0.292082f, 0.233881f, 0.120078f},
                                   {0.667243f, 0.166379f, 0.0f},
                                   {1.0f, 0.0f, 0.0f}};
      const float ntaps[3] = {2.0f, 1.0f, 0.0f};
      const auto blur_pass = [&](int ci, bool horizontal, uint32_t src_slot,
                                 uint32_t dst_rtv_slot, bool src_raw) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(dst_rtv_slot) * g_r.rtv_size;
        list.D3DOMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{float(tile) * ci, 0.0f, float(tile), float(tile), 0.0f, 1.0f};
        list.RSSetViewport(vp);
        D3D12_RECT rc{LONG(tile) * ci, 0, LONG(tile) * (ci + 1), LONG(tile)};
        list.RSSetScissorRect(rc);
        const float bc[12] = {horizontal ? 1.0f : 0.0f, horizontal ? 0.0f : 1.0f,
                              ntaps[ci], src_raw ? 1.0f : 0.0f,
                              kernels[ci][0], kernels[ci][1], kernels[ci][2], 0.0f,
                              float(tile) * ci, float(tile) * (ci + 1) - 1.0f,
                              float(tile) - 1.0f, 0.0f};
        list.D3DSetGraphicsRoot32BitConstants(0, 12, bc, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE src = g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
        src.ptr += size_t(src_slot) * g_r.srv_size;
        context.d3d12.set_graphics_root_descriptor_table(
            context.d3d12.command_processor_user_data, 1, src);
        list.D3DDrawInstanced(3, 1, 0, 0);
      };
      for (int ci = 0; ci < 3; ++ci) {
        blur_pass(ci, true, g_r.shadow_srv_raw, 3, true);
      }
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                            g_r.shadow_mid, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
      for (int ci = 0; ci < 3; ++ci) {
        blur_pass(ci, false, g_r.shadow_srv_mid, 4, false);
      }
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                            g_r.shadow_final, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
      g_r.shadow_in_srv_state = true;
      shadow_ready = shadow_draws > 0;
    }
  }
  g_pw_shadow.Add(perf_ns_since(shadow_t0));

  // The scene draws into the MSAA target when enabled (resolved into the
  // guest output at the end of the pass), or straight into the guest output.
  const bool msaa_on = g_r.msaa > 1 && g_r.msaa_color != nullptr && g_r.resolve_pso != nullptr;
  const D3D12_CPU_DESCRIPTOR_HANDLE output_rtv =
      g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv = output_rtv;
  if (msaa_on) {
    scene_rtv.ptr += g_r.rtv_size;
  } else {
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          context.d3d12.guest_output_resource,
                                          context.d3d12.guest_output_initial_state,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
  }

  const bool use_depth = debug_mode != 4;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_r.dsv_heap->GetCPUDescriptorHandleForHeapStart();
  const FLOAT clear_color[4] = {0.25f, 0.35f, 0.55f, 1.0f};
  list.D3DClearRenderTargetView(scene_rtv, clear_color, 0, nullptr);
  if (use_depth) {
    list.D3DClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list.D3DOMSetRenderTargets(1, &scene_rtv, FALSE, &dsv);
  } else {
    list.D3DOMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);
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
  // Per-item PSO tracking for the opaque pass: two_sided_sheet meshes swap
  // to the backface-culling variant (see MeshBuffers), everything else uses
  // the pass PSO. Only meaningful while use_depth; the transparent sub-pass
  // sets its own PSO and is not switched per item.
  ID3D12PipelineState* scene_pso_bound = use_depth ? g_r.pso : g_r.pso_nodepth;
  // Our own persistent shader-visible SRV heap for the whole pass. These are
  // the last commands of the submission, so displacing the emulated GPU's
  // heap binding is safe.
  list.SetDescriptorHeaps(g_r.srv_heap, nullptr);

  // Per-frame dynamic-shadow bindings: receiver constants at b1 (a small
  // per-frame CBV ring: the 52-float root-constant block is full) and the
  // blurred atlas at t5 (white when no atlas was rendered this frame; the
  // shader is also gated by sh_misc.y).
  if (g_r.shadow_cb != nullptr) {
    const uint32_t cb_offset =
        uint32_t(frame_number % RendererState::kShadowCbRegions) * 256u;
    float* cb = reinterpret_cast<float*>(g_r.shadow_cb_cpu + cb_offset);
    std::memset(cb, 0, 256);
    if (shadow_ready) {
      std::memcpy(cb + 0, sh + 0, 4 * sizeof(float));    // sh_x   = PS c0
      std::memcpy(cb + 4, sh + 12, 4 * sizeof(float));   // sh_y   = PS c3
      std::memcpy(cb + 8, sh + 16, 4 * sizeof(float));   // sh_z   = PS c4
      std::memcpy(cb + 12, sh + 4, 4 * sizeof(float));   // sh_c1  = PS c1
      std::memcpy(cb + 16, sh + 8, 4 * sizeof(float));   // sh_c2  = PS c2
      cb[20] = sh[32];                                   // sh_color = PS c8
      cb[21] = sh[33];
      cb[22] = sh[34];
      cb[23] = 0.299f * sh[32] + 0.587f * sh[33] + 0.114f * sh[34];
      cb[24] = sh[20];  // depth bias (PS c5.x)
      cb[25] = 1.0f;    // enable
    }
    // Exact world-shading frame rows (valid whenever the env-family PS bank
    // was captured this frame, independent of the shadow ATLAS being
    // rendered; draw_item only selects the exact branch when
    // scene.shadow_valid).
    if (scene.shadow_valid) {
      cb[28] = sh[24];  // sun direction (PS c6), for the normal-map kd (v2)
      cb[29] = sh[25];
      cb[30] = sh[26];
      cb[31] = sh[40];  // scene exposure (PS c10.x)
      cb[32] = sh[45];  // material multiplier (PS c11.y)
      cb[33] = scene.family_rows[0];  // tree lightmap scale (tree PS c0.x)
      cb[34] = scene.family_rows[1];  // tree lightmap floor (tree PS c0.y)
      cb[35] = scene.family_rows[2];  // tree tint multiplier (tree PS c4.y)
      cb[36] = scene.fog_ramp[0];     // global fog: sat(d*x+y)^z
      cb[37] = scene.fog_ramp[1];
      cb[38] = scene.fog_ramp[2];
      cb[39] = scene.family_rows[3];  // proxyworld scale (proxy PS c3.y)
      std::memcpy(cb + 40, scene.fog_color, 4 * sizeof(float));
    }
    // dynamicobject.fx frame rows (dyn_sun/dyn_amb/dyn_misc at cb[44..55]).
    if (scene.dynobj_valid) {
      cb[44] = scene.dynobj_rows[0];  // sun dir (PS c9)
      cb[45] = scene.dynobj_rows[1];
      cb[46] = scene.dynobj_rows[2];
      cb[47] = scene.dynobj_rows[3];  // scene exposure (c13.x)
      cb[48] = scene.dynobj_rows[4];  // flat ambient rgb (c15.xyz)
      cb[49] = scene.dynobj_rows[5];
      cb[50] = scene.dynobj_rows[6];
      cb[51] = scene.dynobj_rows[7];  // bounce scale (c15.w)
      cb[52] = scene.dynobj_rows[8];  // material multiplier (c14.y)
      cb[53] = scene.dynobj_rows[9];  // static world-shadow floor (c8.w)
    }
    list.D3DSetGraphicsRootConstantBufferView(
        6, g_r.shadow_cb->GetGPUVirtualAddress() + cb_offset);
    // b2 (character lighting) default: point at the ring base so the root
    // CBV is never left unset; character draws re-point it per item.
    list.D3DSetGraphicsRootConstantBufferView(
        9, g_r.bone_ring->GetGPUVirtualAddress() + bone_region);
    D3D12_GPU_DESCRIPTOR_HANDLE atlas = g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    atlas.ptr += size_t(shadow_ready ? g_r.shadow_srv_final : g_r.white.srv_slot) *
                 g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 7, atlas);
  }

  uint32_t drawn = 0;
  uint32_t item_index = 0;
  // Inline decode budget for DYNAMIC payloads only (skinned/cloth/ropa
  // buffers that change every frame). Static meshes and all textures route
  // to the decode workers on miss; the render thread never pays their
  // decode cost.
  int32_t mesh_budget = REXCVAR_GET(skate3_native_render_scene_mesh_decode_budget);
  if (settling) {
    // Post-takeover settle window: cap inline dynamic decodes too, so the
    // takeover frame never absorbs an unbounded decode burst.
    if (mesh_budget == 0 || mesh_budget > 16) mesh_budget = 16;
  }
  uint32_t mesh_decodes = 0;
  uint32_t tex_decodes = 0;
  // Item drawing body. environment.decal items draw in the same opaque pass:
  // they ARE the wall/ground sections they cover, with the art composited
  // in-shader (alpha-blending them as separate overlay geometry punched
  // holes in the world).
  // Ripple scroll clock for the water branch (seconds; wraps every hour to
  // keep float precision on the scrolled UVs).
  static const std::chrono::steady_clock::time_point water_t0 =
      std::chrono::steady_clock::now();
  const float water_time = float(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - water_t0).count() -
      std::floor(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - water_t0).count() /
          3600.0) *
          3600.0);

  // Words-keyed texture lookup with payload revalidation, shared by the
  // streamed-artwork diffuse override and the 2D pass. The event-ad system
  // rotates artwork IN PLACE; it streams the next poster into the same
  // guest buffer, so the fetch words (this cache's key) never change; without
  // the fingerprint recheck every frame keeps the first ad ever decoded at
  // that address and the wall posters diverge from the emulated frame.
  const auto find_words_texture = [&](uint64_t key) {
    auto it = g_r.textures_2d.find(key);
    if (it != g_r.textures_2d.end() && it->second.valid &&
        frame_number >= it->second.recheck_frame &&
        REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
      it->second.recheck_frame = frame_number + 16;
      const uint64_t fp = SamplePayloadFingerprint(base, it->second.payload_addr,
                                                   it->second.payload_size);
      if (fp != 0 && fp != it->second.payload_fp) {
        // In-place content rotation (event ads stream the next poster into
        // the same buffer; the words, and so the key, never change): heal
        // on the workers; keep serving the current decode meanwhile.
        EnqueueWordsMiss(key, it->second.fetch_words);
      }
    }
    return it;
  };
  // Resolve six raw fetch-constant words (a draw-time streamed-artwork
  // binding with no guest texture object) through the words-keyed cache.
  // Returns null while the decode is in flight on the workers (the item then
  // falls back to its channel diffuse, the placeholder poster) or when the
  // words fail to decode.
  const auto resolve_fetch_words = [&](const uint32_t words[6]) -> const GuestTexture* {
    const uint64_t fkey = FetchWordsKey(words);
    auto fit = find_words_texture(fkey);
    if (fit == g_r.textures_2d.end()) {
      EnqueueWordsMiss(fkey, words);
      return nullptr;
    }
    return fit->second.valid ? &fit->second : nullptr;
  };

  const auto draw_item = [&](const DrawItem& item) {
    // NO per-frame inline decodes here. Static content (world geometry,
    // props) loads/heals on the decode workers via the miss queue; a
    // texture decode averages ~10 ms and panning surfaces dozens of new
    // payloads in one frame; inline decode WAS the panning lag spike.
    // Dynamic payloads (skinned/ropa, CPU-rewritten every frame) are kept
    // fresh by the dyn decode jobs (guest-thread snapshot -> worker), one
    // frame behind the sim; only their FIRST sight decodes inline. The
    // cloth-quads particle path (gated off by default) still re-decodes
    // inline on change; it has no job route.
    const bool dynamic_payload = item.skinned || item.cloth_quads || item.ropa;
    auto it = g_r.meshes.find(item.mesh);
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint &&
        REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
      if (item.cloth_quads) {
        const uint64_t submission = command_processor->GetCurrentSubmission();
        g_r.retired.emplace_back(it->second.vb, submission);
        g_r.retired.emplace_back(it->second.ib, submission);
        g_r.meshes.erase(it);
        it = g_r.meshes.end();
      } else if (!dynamic_payload) {
        // Streaming heal: keep drawing the old decode this frame; the
        // workers decode the new payload and the commit swaps it in.
        EnqueueMeshMiss(item.mesh);
      }
    }
    if (it == g_r.meshes.end()) {
      if (!item.cloth_quads) {
        // ALL mesh misses decode on the workers, including first-sight
        // skinned entities (a streamed-in NPC appears 1-2 frames late
        // instead of hitching the frame; the dyn decode jobs usually land
        // the same content even sooner from the guest-thread snapshot).
        EnqueueMeshMiss(item.mesh);
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (mesh_budget > 0 && mesh_decodes >= uint32_t(mesh_budget)) {
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;  // decodes on a later frame
      }
      ++mesh_decodes;
      const auto decode_t0 = PerfClock::now();
      MeshBuffers buffers;
      const bool decode_ok = DecodeMesh(g_r.device, base, item, buffers);
      g_pw_mesh_decode.Add(perf_ns_since(decode_t0));
      if (!decode_ok) {
        g_rr_decode_fail.fetch_add(1, std::memory_order_relaxed);
        static std::unordered_set<uint32_t> logged;
        if (logged.size() < 32 && logged.insert(item.mesh).second) {
          REXLOG_WARN("native-scene: DecodeMesh FAILED mesh={:08X} vb={:08X} fmt={} stride={}",
                      item.mesh, item.vb_addr, item.pos_fmt, item.stride);
        }
        return;
      }
      buffers.fingerprint = item.fingerprint;
      it = g_r.meshes.emplace(item.mesh, buffers).first;
    }
    const MeshBuffers& buffers = it->second;

    // Double-sided sheet props draw with backface culling; without it the
    // front/back copies z-fight into lightmap flicker at range (banners/
    // flags). Opaque depth pass only; a mirrored instance (negative world
    // determinant) would flip winding, so those stay uncull(ed).
    // (hair items with a validated lighting capture draw in the blended
    // sub-pass under their own cull PSOs; never reset those here)
    const bool hair_pass = item.char_family >= 4 && item.char_family <= 5 &&
                           item.char_rows[14 * 4 + 1] > 0.0f;
    if (use_depth && !item.transparent && !item.water && !hair_pass &&
        g_r.pso_cullback != nullptr) {
      const float* w = item.world;
      const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                         w[1] * (w[4] * w[10] - w[6] * w[8]) +
                         w[2] * (w[4] * w[9] - w[5] * w[8]);
      ID3D12PipelineState* want =
          (buffers.two_sided_sheet && det3 >= 0.0f) ? g_r.pso_cullback : g_r.pso;
      if (want != scene_pso_bound) {
        list.D3DSetPipelineState(want);
        scene_pso_bound = want;
      }
    }

    // Resolve guest textures (white fallback). Cached decodes revalidate
    // against the live fetch words; streaming reuses texture objects. The
    // object addresses were readable on the game thread this frame; NO
    // VirtualQuery here; it takes the process VAD lock, which the guest
    // streaming threads hammer, and ~2 calls per item stalled the whole
    // renderer to 3 fps.
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
        // Content revalidation: a decode taken while the payload was still
        // streaming in is garbage (checkered/blacked-out surfaces) and the
        // fetch words never change when the content lands afterwards.
        // Re-decode when the sampled payload changed; keep the old decode
        // when the payload became unreadable (mip streamed out at range).
        bool payload_changed = false;
        if (tit->second.valid && frame_number >= tit->second.recheck_frame &&
            REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
          tit->second.recheck_frame = frame_number + 16;
          const uint64_t fp = SamplePayloadFingerprint(
              base, tit->second.payload_addr, tit->second.payload_size);
          payload_changed = fp != 0 && fp != tit->second.payload_fp;
        }
        // words_changed re-decodes are throttled by retry_after_frame on the
        // VALID entry (retry_failed already gates invalid ones): streaming
        // mip oscillation (words flapping A<->B for seconds, see the banner
        // churn logs) re-detected the mismatch EVERY frame while a heal was
        // already in flight or failing: thousands of queued decodes and log
        // spam that delayed the legitimate heal and stretched the visible
        // stale-art window.
        const bool words_changed =
            std::memcmp(live, tit->second.fetch_words, sizeof(live)) != 0 &&
            (!tit->second.valid || frame_number >= tit->second.retry_after_frame);
        if (retry_failed || payload_changed || words_changed) {
          // Re-decode churn diagnostic: repeated fetch-word or payload
          // changes on one object = streaming oscillation, visible as
          // texture flicker on the affected meshes.
          static std::atomic<uint32_t> s_redecode_logs{0};
          if (s_redecode_logs.fetch_add(1) < 256) {
            REXLOG_INFO(
                "native-scene: texture re-decode obj={:08X} reason={}{}{} "
                "old=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] new=[{:08X} {:08X} "
                "{:08X} {:08X} {:08X} {:08X}]",
                tex_ptr, words_changed ? "words" : "", payload_changed ? "payload" : "",
                retry_failed ? "retry" : "", tit->second.fetch_words[0],
                tit->second.fetch_words[1], tit->second.fetch_words[2],
                tit->second.fetch_words[3], tit->second.fetch_words[4],
                tit->second.fetch_words[5], live[0], live[1], live[2], live[3], live[4],
                live[5]);
          }
          // Heal on the workers: keep serving the current decode (no white
          // flash, no inline-decode hitch); the commit swaps the entry. A
          // failed retry keeps the entry's retry clock ticking via the
          // commit's retry_after_frame stamp.
          EnqueueTexMiss(tex_ptr);
          if (retry_failed) {
            tit->second.retry_after_frame = frame_number + 120;
          } else {
            // Valid entry heal in flight: next retry no sooner than +4
            // frames (a worker round trip is 1-3); the commit pushes this
            // further out (+30) when the fresh decode failed.
            tit->second.retry_after_frame = frame_number + 4;
          }
        }
      }
      if (tit == g_r.textures.end()) {
        // New texture: decode on the workers; white for the 1-3 frames that
        // takes (inline decode measured ~10 ms avg / ~70 ms max, the
        // panning lag spikes).
        EnqueueTexMiss(tex_ptr);
        ++tex_decodes;
        g_rr_tex_deferred.fetch_add(1, std::memory_order_relaxed);
        return &g_r.white;
      }
      return tit->second.valid ? &tit->second : &g_r.white;
    };
    // Streamed-artwork diffuse override (see DrawItem::diffuse_fetch): the
    // real art exists only as draw-time fetch words; resolve those through
    // the words-keyed cache (shared with the 2D pass; the art has no guest
    // object to key on).
    const GuestTexture* diffuse =
        item.diffuse_fetch[1] != 0 ? resolve_fetch_words(item.diffuse_fetch) : nullptr;
    if (diffuse == nullptr) {
      diffuse = resolve_texture(item.diffuse_tex);
    }
    const GuestTexture* lightmap =
        item.lightmap_tex != 0 && REXCVAR_GET(skate3_native_render_scene_lightmaps)
            ? resolve_texture(item.lightmap_tex)
            : nullptr;
    if (lightmap == &g_r.white) {
      lightmap = nullptr;
    }

    // constants = world + mvp (world * view_proj, row-vector) + tint + cam
    // + material tint + overlay params + misc. tint.a > 0 selects debug
    // solid colors; tint.r > 0 marks a bound lightmap. For transparent
    // items misc.yzw carries the fog ramp and mat_tint the fog color.
    float constants[52] = {};
    std::memcpy(constants, item.world, sizeof(item.world));
    if (item.unlit) {
      // sky.*: the dome mesh is CAMERA-RELATIVE (sky.fx defaultVS adds
      // g_vViewPos to every vertex). Anchoring it at the world origin put
      // the baked skyline panorama ~700 m off, visibly rotated/parallaxed
      // against the emulated frame. The game's sky viewpos tracks the camera
      // in x/z but pins Y at the level's fixed sky elevation (captured per
      // frame from the sky draw's VS bank; 165.0 in every capture); using
      // cam.y rendered the skyline ~160 m too LOW.
      constants[12] += scene.cam_pos[0];
      constants[13] += scene.sky_height;
      constants[14] += scene.cam_pos[2];
    }
    float* mvp = constants + 16;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += constants[r * 4 + k] * scene.view_proj[k * 4 + c];
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
      if (item.skinned && !item.bones.empty()) {
        // Ring exhausted: this item renders bind-pose at identity,
        // effectively invisible. Must never happen silently.
        g_rr_no_bones.fetch_add(1, std::memory_order_relaxed);
      }
      list.D3DSetGraphicsRootShaderResourceView(3, g_r.bone_ring->GetGPUVirtualAddress());
    }

    if (debug_mode >= 2) {
      // Stable per-object colors: hash the mesh address, not the (sort-order
      // dependent) item index.
      const uint32_t hash = (item.mesh >> 4) * 2654435761u;
      constants[32] = float((hash >> 0) & 0xFF) / 255.0f;
      constants[33] = float((hash >> 8) & 0xFF) / 255.0f;
      constants[34] = float((hash >> 16) & 0xFF) / 255.0f;
      constants[35] = 1.0f;
    } else {
      constants[32] = lightmap != nullptr ? 1.0f : 0.0f;
      // tint.g doubles as the "character: alpha = gloss, no alpha-test"
      // marker; ropa garments rendered rigid have no bones but must not be
      // alpha-clipped (their VS skinning branch is inert: zero weights).
      constants[33] = (bones_bound || item.ropa) ? 1.0f : 0.0f;
      constants[34] = item.unlit ? 1.0f : 0.0f;
      constants[35] = 0.0f;
    }
    constants[36] = scene.cam_pos[0];
    constants[37] = scene.cam_pos[1];
    constants[38] = scene.cam_pos[2];
    // cam_pos.w = character shading family: > 0 switches the PS to the
    // captured character-lighting branch (rows uploaded to b2 below);
    // char_rows[14*4+1] stays 0 when the capture failed validation, which
    // keeps the item on the legacy empirical shading.
    constants[39] = 0.0f;
    if (debug_mode == 0 && item.char_family != 0 &&
        item.char_rows[14 * 4 + 1] > 0.0f) {
      const uint32_t offset = (g_r.bone_ring_offset + 255u) & ~255u;
      if (offset + 256u <= RendererState::kBoneRegionSize) {
        std::memcpy(g_r.bone_ring_cpu + bone_region + offset, item.char_rows,
                    sizeof(item.char_rows));
        g_r.bone_ring_offset = offset + 256u;
        list.D3DSetGraphicsRootConstantBufferView(
            9, g_r.bone_ring->GetGPUVirtualAddress() + bone_region + offset);
        constants[39] = item.char_rows[14 * 4 + 1];
        g_char_drawn.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // cam_pos.w = -family selects the exact world-material branch. Gated on
    // the frame rows being captured (scene.shadow_valid carries the scene
    // exposure / material multiplier at b1); without them the tone chain
    // would multiply by zero and render black.
    if (debug_mode == 0 && item.env_family != 0 && scene.shadow_valid &&
        !item.water && !item.transparent) {
      constants[39] = -float(item.env_family);
    }
    // cam_pos.w = -(20 + variant) selects the exact dynamicobject branch
    // (rigid props). Gated on the frame's dynamicobject lighting rows having
    // been captured (dyn_* at b1); without them the tone chain multiplies by
    // zero and renders black; fall back to the legacy shading otherwise.
    if (debug_mode == 0 && item.dynobj != 0 && scene.dynobj_valid) {
      constants[39] = -float(20 + item.dynobj);
      g_dynobj_drawn.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(constants + 40, item.tint, 4 * sizeof(float));
    // t3 = macro grime/crack overlay, t4 = decal art for environment.decal
    // surfaces (in-shader composite). Independent slots: decal ground/wall
    // sections carry the same macrooverlay as their non-decal neighbors, and
    // binding only the art dropped the macro multiply there; alternating
    // plaza sections rendered ~1.4x too bright (the ground checkerboard).
    const GuestTexture* macro_tex =
        item.macro_tex != 0 && REXCVAR_GET(skate3_native_render_scene_macro)
            ? resolve_texture(item.macro_tex)
            : &g_r.white;
    if ((item.water || item.char_family >= 6) && item.water_normal != 0) {
      // Water rides its ripple normal map in the macro slot (water never
      // carries a macro overlay; overlay.z stays 0 below so the macro
      // composite path never runs). Vehicles do the same with their DXN
      // panel normal map; without it the hinged panels' vertex normals
      // face away from the sun and shade as a dark ambient-blue patch that
      // stops at the door seams (the exact PS with a FLAT map reproduces
      // that artifact; with the real map it matches the emulated car).
      macro_tex = resolve_texture(item.water_normal);
    }
    // Water / vehicle environment cube (t6, root param 8): decoded once per
    // guest object into the cube cache; the gray fallback cube otherwise.
    // Vehicle materials carry an `environment` channel that resolves through
    // the same chan+0x1C path as the ocean's. Cube decodes run on the
    // workers (a SINGLE inline cube decode measured up to ~100 ms, the
    // largest remaining traversal hitch when a vehicle / reflective area
    // streamed in); flat-gray reflections for the 1-3 frames in flight are
    // invisible.
    const GuestTexture* cube_tex = &g_r.white_cube;
    if ((item.water || item.char_family >= 6 ||
         (item.env_family >= 5 && item.env_family <= 6)) &&
        item.water_env != 0) {
      auto cit = g_r.cube_textures.find(item.water_env);
      if (cit == g_r.cube_textures.end()) {
        EnqueueCubeMiss(item.water_env);
      } else if (cit->second.valid) {
        cube_tex = &cit->second;
      }
    }
    const GuestTexture* decal_tex = item.decal && item.decal_art != 0 &&
                                            REXCVAR_GET(skate3_native_render_scene_decals)
                                        ? resolve_texture(item.decal_art)
                                        : &g_r.white;
    // Streamed-artwork decal override (see DrawItem::decal_fetch): ad frames
    // covered by an environment.decal section get the current event-ad art
    // bound over the decal channel at draw time.
    if (item.decal && item.decal_fetch[1] != 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      const GuestTexture* ad = resolve_fetch_words(item.decal_fetch);
      if (ad != nullptr) {
        decal_tex = ad;
      }
    }
    if (item.char_family >= 4 && item.char_family <= 5 &&
        item.hair_alpha_tex != 0) {
      // Hair strand coverage rides the (otherwise unused) decal slot; the
      // PS hair branch samples it at the raw second texcoord. The white
      // fallback keeps failed decodes opaque rather than invisible.
      decal_tex = resolve_texture(item.hair_alpha_tex);
    }
    // Exact env families without decal art bind the material's spec/ecc/
    // refmask map (or the animated.tree noise tint) in the free decal slot;
    // overlay.w == 3 tells the shader the masks are live.
    bool spec_bound = false;
    if (item.env_family != 0 && !item.decal && item.env_family != 10 &&
        item.spec_tex != 0) {
      const GuestTexture* spec = resolve_texture(item.spec_tex);
      if (spec != &g_r.white) {
        decal_tex = spec;
        spec_bound = true;
      }
    }
    const bool is_decal =
        item.char_family < 4 && item.decal && decal_tex != &g_r.white;
    constants[44] = item.macro_scale;
    constants[45] = item.macro_opacity;
    constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
    // overlay.w: 1 = single-placement decal (art clamps), 2 = tileable
    // decal (art wraps; clamping a many-period uv range stretched the
    // border texels into the giant cliff-face streaks), 3 = spec masks
    // bound (exact env families).
    constants[47] = is_decal ? (item.decal_tileable ? 2.0f : 1.0f)
                             : (spec_bound ? 3.0f : 0.0f);
    if (item.water) {
      // overlay.x = ripple scroll time, overlay.y = real environment cube
      // bound at t6, overlay.z = ripple normal map resolved (in the macro
      // slot; the shader synthesizes procedural ripples otherwise).
      // overlay.w = 1 marks water with NO diffuse channel (ocean.default):
      // the body term must be zero (ocean.fx diffTerm = 0); the white
      // fallback diffuse otherwise renders the whole sea as a bright plain.
      constants[44] = water_time;
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
      constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
      constants[47] = item.diffuse_tex == 0 ? 1.0f : 0.0f;
    } else if (item.char_family >= 6) {
      // Vehicles reuse the water convention: overlay.y > 0 = a real
      // environment cube bound at t6 (the PS vehicle branch's reflection
      // term). Vehicles never carry macro/decal channels, so the macro
      // defaults staged above are inert, but overlay.y must not inherit
      // macro_opacity's 1.0 default when no cube resolved.
      constants[45] = cube_tex != &g_r.white_cube ? 1.0f : 0.0f;
    }
    // misc.x: alpha-blended sub-pass item (1 = transparentenvironment
    // shading, 2 = water branch); fog rides in misc.yzw (ramp) and the
    // mat_tint row (color), unused by these items otherwise
    // (root-signature DWORD budget).
    constants[48] = item.water ? 2.0f : (item.transparent ? 1.0f : 0.0f);
    if (item.transparent || item.water) {
      constants[49] = scene.fog_ramp[0];
      constants[50] = scene.fog_ramp[1];
      constants[51] = scene.fog_ramp[2];
      std::memcpy(constants + 40, scene.fog_color, 4 * sizeof(float));
    }
    list.D3DSetGraphicsRoot32BitConstants(0, 52, constants, 0);

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
    D3D12_GPU_DESCRIPTOR_HANDLE macro_gpu = heap_start;
    macro_gpu.ptr += size_t(macro_tex->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 4, macro_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE decal_gpu = heap_start;
    decal_gpu.ptr += size_t(decal_tex->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 5, decal_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE cube_gpu = heap_start;
    cube_gpu.ptr += size_t(cube_tex->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 8, cube_gpu);
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
  };

  // Opaque items first; environment.transparent items are deferred to an
  // alpha-blended sub-pass (depth test on, z-write off) drawn back-to-front;
  // interleaved opaque rendering alpha-tested the mist sheets into solid
  // white cloud blobs.
  const auto items_t0 = PerfClock::now();
  const auto view_dist2 = [&](const DrawItem& item) {
    float c[3], w[3];
    for (int a = 0; a < 3; ++a) {
      c[a] = (item.bbox_min[a] + item.bbox_max[a]) * 0.5f;
    }
    for (int a = 0; a < 3; ++a) {
      w[a] = c[0] * item.world[0 * 4 + a] + c[1] * item.world[1 * 4 + a] +
             c[2] * item.world[2 * 4 + a] + item.world[3 * 4 + a];
    }
    float d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
      const float d = w[a] - scene.cam_pos[a];
      d2 += d * d;
    }
    return d2;
  };
  std::vector<const DrawItem*> transparent_items;
  // Opaque items draw front-to-back (bbox-center distance): early-z rejects
  // occluded pixels before the heavy material PS runs. The game's sort-list
  // order is by render state, not depth; depth-write LESS_EQUAL makes the
  // final image order-independent, so reordering is safe.
  std::vector<std::pair<float, const DrawItem*>> opaque_items;
  opaque_items.reserve(scene.items.size());
  for (const DrawItem& item : scene.items) {
    const uint32_t index = item_index++;
    if (debug_mode == 1) {
      break;
    }
    if (debug_mode == 3 && index >= 20) {
      break;
    }
    // Hair with a validated lighting capture joins the sorted alpha
    // sub-pass (strand coverage blend, depth test on / z-write off, the
    // game's own hair render state); without the capture it stays on the
    // legacy opaque path. Vehicle glass (fam 7) blends there too,
    // reflection-only windows at the captured alpha; vehicle bodies (fam 6)
    // stay opaque.
    const bool char_capture_ok = item.char_rows[14 * 4 + 1] > 0.0f;
    const bool hair_blend = item.char_family >= 4 && item.char_family <= 5 &&
                            char_capture_ok;
    const bool glass_blend = item.char_family == 7 && char_capture_ok;
    if ((item.transparent || item.water || hair_blend || glass_blend) &&
        debug_mode == 0) {
      if (REXCVAR_GET(skate3_native_render_scene_transparents)) {
        transparent_items.push_back(&item);
      }
      continue;
    }
    opaque_items.emplace_back(view_dist2(item), &item);
  }
  if (REXCVAR_GET(skate3_native_render_scene_sort_opaque) && debug_mode == 0) {
    std::stable_sort(opaque_items.begin(), opaque_items.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  for (const auto& [dist, item] : opaque_items) {
    draw_item(*item);
  }
  if (!transparent_items.empty() && g_r.pso_transparent != nullptr) {
    std::stable_sort(transparent_items.begin(), transparent_items.end(),
                     [&](const DrawItem* a, const DrawItem* b) {
                       return view_dist2(*a) > view_dist2(*b);
                     });
    list.D3DSetPipelineState(use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
    for (const DrawItem* item : transparent_items) {
      const bool hair = item->char_family >= 4 && item->char_family <= 5 &&
                        item->char_rows[14 * 4 + 1] > 0.0f;
      if (hair && use_depth && g_r.pso_hair_a != nullptr && g_r.pso_hair_b != nullptr) {
        // The game's two hair passes: cull BACK then cull FRONT with the
        // same shader: keeps far-side strands from compositing over
        // near-side ones (one uncull(ed) pass reads as crunchy noise).
        list.D3DSetPipelineState(g_r.pso_hair_a);
        draw_item(*item);
        list.D3DSetPipelineState(g_r.pso_hair_b);
        draw_item(*item);
        list.D3DSetPipelineState(g_r.pso_transparent);
        continue;
      }
      draw_item(*item);
    }
  }
  g_pw_items.Add(perf_ns_since(items_t0));

  // Guest-texture resolver shared by the spline pass (pre-resolve, in the
  // scene pass) and the HUD pass (post-resolve); both allocate strip/quad
  // vertices from the same per-frame ui_ring region.
  const auto resolve_2d_texture = [&](const uint32_t fetch[6]) -> const GuestTexture* {
    if ((fetch[0] & 0x3u) != 2 || fetch[1] == 0) {
      return &g_r.white;
    }
    uint64_t key = 1469598103934665603ull;
    for (int k = 0; k < 6; ++k) {
      key ^= fetch[k];
      key *= 1099511628211ull;
    }
    auto it = find_words_texture(key);
    if (it == g_r.textures_2d.end()) {
      // HUD/spline art decodes inline (small; async would flash UI elements
      // white on first sight). The big streamed posters go through
      // resolve_fetch_words -> the worker queue instead.
      const auto hud_t0 = PerfClock::now();
      GuestTexture gt;
      EnsureGuestTextureFromWords(context, base, fetch, gt);
      g_pw_tex_decode.Add(perf_ns_since(hud_t0));
      if (!gt.valid) {
        static std::unordered_set<uint64_t> logged;
        if (logged.size() < 32 && logged.insert(key).second) {
          REXLOG_INFO(
              "native-scene: 2D texture decode FAILED fetch=[{:08X} {:08X} {:08X} "
              "{:08X} {:08X} {:08X}]",
              fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
        }
      }
      context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
      it = g_r.textures_2d.emplace(key, gt).first;
    }
    return it->second.valid ? &it->second : &g_r.white;
  };
  const uint32_t ui_region =
      uint32_t(frame_number % RendererState::kUiRegions) * RendererState::kUiRegionSize;
  uint32_t ui_offset = 0;

  // In-world neon splines (waypoint arrows / marker beams): replayed inside
  // the scene pass, depth-tested against the world like the emulated frame,
  // in submission order (darken backdrop passes precede the additive glow).
  uint32_t drawn_spline = 0;
  if (REXCVAR_GET(skate3_native_render_scene_splines) &&
      g_r.pso_spline_default != nullptr && g_r.ui_ring_cpu != nullptr) {
    std::vector<SplineDraw> scene_spline;
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      scene_spline = g_scene_spline;
    }
    for (const SplineDraw& s : scene_spline) {
      const uint32_t bytes = uint32_t(s.verts.size());
      if (bytes == 0 || ui_offset + bytes > RendererState::kUiRegionSize) {
        continue;
      }
      std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, s.verts.data(), bytes);
      const uint32_t srv_slot = resolve_2d_texture(s.fetch)->srv_slot;
      list.D3DSetPipelineState(s.pass == 1 ? g_r.pso_spline_darken
                                           : g_r.pso_spline_default);
      // Root constants: i_intensity as staged (c149).
      list.D3DSetGraphicsRoot32BitConstants(0, 4, s.consts + 149 * 4, 0);
      D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu =
          g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
      srv_gpu.ptr += size_t(srv_slot) * g_r.srv_size;
      context.d3d12.set_graphics_root_descriptor_table(
          context.d3d12.command_processor_user_data, 1, srv_gpu);
      D3D12_VERTEX_BUFFER_VIEW vbv{
          g_r.ui_ring->GetGPUVirtualAddress() + ui_region + ui_offset, bytes, 28};
      list.D3DIASetVertexBuffers(0, 1, &vbv);
      list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      list.D3DDrawInstanced(s.count, 1, 0, 0);
      ui_offset += bytes;
      ++drawn_spline;
    }
  }

  // Selection-outline mask (see kOutlineShaderSource): re-render the frame's
  // selected items into the small R8 target while the scene pass state is
  // still bound. The edge composite runs after the resolve, on the
  // single-sample output.
  bool outline_ready = false;
  if (REXCVAR_GET(skate3_native_render_scene_selection_outline) &&
      g_r.pso_outline_mask != nullptr && g_r.pso_outline_edge != nullptr &&
      g_r.outline_mask != nullptr) {
    std::vector<const DrawItem*> sel;
    for (const DrawItem& item : scene.items) {
      // Skinned items are excluded: the mask VS runs the rigid path (world
      // matrix), which renders a skinned mesh at bind pose at the origin.
      if (item.selected && !item.skinned) {
        sel.push_back(&item);
      }
    }
    if (!sel.empty()) {
      D3D12_CPU_DESCRIPTOR_HANDLE mask_rtv =
          g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      mask_rtv.ptr += size_t(7) * g_r.rtv_size;
      const FLOAT mask_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      list.D3DClearRenderTargetView(mask_rtv, mask_clear, 0, nullptr);
      list.D3DOMSetRenderTargets(1, &mask_rtv, FALSE, nullptr);
      D3D12_VIEWPORT mask_vp{0.0f, 0.0f, float(g_r.outline_mask_width),
                             float(g_r.outline_mask_height), 0.0f, 1.0f};
      list.RSSetViewport(mask_vp);
      D3D12_RECT mask_sc{0, 0, LONG(g_r.outline_mask_width),
                         LONG(g_r.outline_mask_height)};
      list.RSSetScissorRect(mask_sc);
      list.D3DSetPipelineState(g_r.pso_outline_mask);
      for (const DrawItem* item : sel) {
        auto mit = g_r.meshes.find(item->mesh);
        if (mit == g_r.meshes.end() || mit->second.fingerprint != item->fingerprint) {
          continue;  // decoded by the main pass this frame; masks from the next
        }
        float constants[52] = {};
        std::memcpy(constants, item->world, sizeof(item->world));
        float* mvp = constants + 16;
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
              sum += constants[r * 4 + k] * scene.view_proj[k * 4 + c];
            }
            mvp[r * 4 + c] = sum;
          }
        }
        // tint = (1, 0, 0, 1): the scene PS's solid-color early-out
        // (tint.a > 0) writes 1.0 into the R8 mask; tint.g = 0 keeps the VS
        // skinning branch off.
        constants[32] = 1.0f;
        constants[35] = 1.0f;
        list.D3DSetGraphicsRoot32BitConstants(0, 52, constants, 0);
        list.D3DIASetVertexBuffers(0, 1, &mit->second.vb_view);
        list.D3DIASetIndexBuffer(&mit->second.ib_view);
        for (const DrawEntry& draw : item->draws) {
          if (draw.prim == 4) {
            list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          } else if (draw.prim == 6) {
            list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          } else {
            continue;
          }
          list.D3DDrawIndexedInstanced(draw.index_count, 1, draw.start_index,
                                       draw.base_vertex, 0);
          outline_ready = true;
        }
      }
      // Restore the pass state the resolve/2D paths rely on (fullscreen
      // viewport; the non-MSAA path keeps rendering into the scene target).
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      if (!msaa_on) {
        list.D3DOMSetRenderTargets(1, &scene_rtv, FALSE, use_depth ? &dsv : nullptr);
      }
      if (outline_ready) {
        context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                              g_r.outline_mask,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
  }

  if (msaa_on) {
    // Resolve: average the MSAA samples into the guest output with a
    // fullscreen pass, then restore steady-state resource states.
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          g_r.msaa_color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          context.d3d12.guest_output_resource,
                                          context.d3d12.guest_output_initial_state,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
    list.D3DOMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
    list.D3DSetPipelineState(g_r.resolve_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE msaa_srv =
        g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    msaa_srv.ptr += size_t(g_r.msaa_srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 1, msaa_srv);
    list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.D3DDrawInstanced(3, 1, 0, 0);
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          g_r.msaa_color,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  if (outline_ready) {
    // Selection-outline composite: additive stencil-edge-detect over the
    // resolved output (before the popup blur, like the game's postfx order).
    context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
    list.D3DOMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
    list.RSSetViewport(viewport);
    list.RSSetScissorRect(scissor);
    list.D3DSetPipelineState(g_r.pso_outline_edge);
    list.D3DSetGraphicsRoot32BitConstants(0, 4, scene.outline_color, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE mask_srv =
        g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    mask_srv.ptr += size_t(g_r.outline_mask_srv) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 1, mask_srv);
    list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.D3DDrawInstanced(3, 1, 0, 0);
    // Back to the mask's steady state for the next frame.
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          g_r.outline_mask,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  // Popup background blur: exact port of the game's blur_hBlur/vBlur +
  // postfx_basictex chain (see kBlurShaderSource): H blur of the finished
  // frame into a 1152x640 intermediate, V blur, then a fullscreen bilinear
  // stretch back over the output. Runs only on frames where the game issued
  // the blur draws (scene.ui_blur = captured kernel scale). The popup's own
  // 2D draws follow after and stay sharp.
  if (scene.ui_blur > 0.0f && g_r.pso_blur != nullptr && g_r.pso_blur_blit != nullptr &&
      g_r.pso_blur_down != nullptr && g_r.blur_tex[0] != nullptr) {
    // The guest output resource can change between frames; re-point the
    // dedicated SRV slot at it each blur frame.
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = context.d3d12.guest_output_format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
      slot.ptr += size_t(g_r.output_srv_slot) * g_r.srv_size;
      g_r.device->CreateShaderResourceView(context.d3d12.guest_output_resource, &srv, slot);
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE heap_start =
        g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    const auto srv_table = [&](uint32_t slot) {
      D3D12_GPU_DESCRIPTOR_HANDLE h = heap_start;
      h.ptr += size_t(slot) * g_r.srv_size;
      context.d3d12.set_graphics_root_descriptor_table(
          context.d3d12.command_processor_user_data, 1, h);
    };
    D3D12_VIEWPORT blur_vp{0.0f, 0.0f, float(RendererState::kBlurWidth),
                           float(RendererState::kBlurHeight), 0.0f, 1.0f};
    D3D12_RECT blur_sc{0, 0, LONG(RendererState::kBlurWidth),
                       LONG(RendererState::kBlurHeight)};
    D3D12_CPU_DESCRIPTOR_HANDLE blur_rtv0 = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    blur_rtv0.ptr += size_t(5) * g_r.rtv_size;
    D3D12_CPU_DESCRIPTOR_HANDLE blur_rtv1 = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    blur_rtv1.ptr += size_t(6) * g_r.rtv_size;
    const auto to_srv = [&](ID3D12Resource* r) {
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data, r,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    const auto to_rt = [&](ID3D12Resource* r) {
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data, r,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET);
    };
    const auto flush = [&] {
      context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
    };
    list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Downsample (prefiltered): output -> blur_tex[0], the game's 1152x640
    // blur space. The H/V passes then run 1:1 like the original chain.
    to_srv(context.d3d12.guest_output_resource);
    flush();
    list.D3DOMSetRenderTargets(1, &blur_rtv0, FALSE, nullptr);
    list.RSSetViewport(blur_vp);
    list.RSSetScissorRect(blur_sc);
    list.D3DSetPipelineState(g_r.pso_blur_down);
    const float d_consts[4] = {1.0f / float(context.guest_output_width),
                               1.0f / float(context.guest_output_height), 0.0f, 0.0f};
    list.D3DSetGraphicsRoot32BitConstants(0, 4, d_consts, 0);
    srv_table(g_r.output_srv_slot);
    list.D3DDrawInstanced(3, 1, 0, 0);
    // H: blur_tex[0] -> blur_tex[1].
    to_srv(g_r.blur_tex[0]);
    flush();
    list.D3DOMSetRenderTargets(1, &blur_rtv1, FALSE, nullptr);
    list.D3DSetPipelineState(g_r.pso_blur);
    const float h_consts[4] = {1.0f, 0.0f, scene.ui_blur, 0.0f};
    list.D3DSetGraphicsRoot32BitConstants(0, 4, h_consts, 0);
    srv_table(g_r.blur_srv[0]);
    list.D3DDrawInstanced(3, 1, 0, 0);
    // V: blur_tex[1] -> blur_tex[0].
    to_srv(g_r.blur_tex[1]);
    to_rt(g_r.blur_tex[0]);
    flush();
    list.D3DOMSetRenderTargets(1, &blur_rtv0, FALSE, nullptr);
    const float v_consts[4] = {0.0f, 1.0f, scene.ui_blur, 0.0f};
    list.D3DSetGraphicsRoot32BitConstants(0, 4, v_consts, 0);
    srv_table(g_r.blur_srv[1]);
    list.D3DDrawInstanced(3, 1, 0, 0);
    // Replace: blur_tex[0] stretched over the full output (basictex).
    to_srv(g_r.blur_tex[0]);
    to_rt(context.d3d12.guest_output_resource);
    flush();
    list.D3DOMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
    list.RSSetViewport(viewport);
    list.RSSetScissorRect(scissor);
    list.D3DSetPipelineState(g_r.pso_blur_blit);
    srv_table(g_r.blur_srv[0]);
    list.D3DDrawInstanced(3, 1, 0, 0);
    // Restore the intermediates' steady state for the next blur frame.
    to_rt(g_r.blur_tex[0]);
    to_rt(g_r.blur_tex[1]);
  }

  // 2D overlay (HUD/APT): replay the frame's captured 2D draws over the
  // resolved output, in submission order, with the game's own transform
  // constants and textures. In gameplay these draws compose the game's
  // full-screen HUD overlay texture at true screen coordinates; drawing
  // them here IS the composite the (suppressed) emulated pass used to do.
  uint32_t drawn_2d = 0;
  if (REXCVAR_GET(skate3_native_render_scene_2d) && g_r.pso_2d != nullptr &&
      g_r.ui_ring_cpu != nullptr) {
    std::vector<Draw2d> scene_2d;
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      scene_2d = g_scene_2d;
    }
    if (!scene_2d.empty()) {
      list.D3DSetPipelineState(g_r.pso_2d);
      // One shared draw routine for both the RTT passes and the screen pass.
      // ui_region/ui_offset continue after the spline pass's allocations.
      const auto emit_draw = [&](const Draw2d& d) {
        const uint32_t bytes = uint32_t(d.verts.size());
        if (bytes == 0 || d.stride != 28) {
          return;
        }
        if (ui_offset + bytes > RendererState::kUiRegionSize) {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, d.verts.data(), bytes);
        const uint32_t srv_slot = resolve_2d_texture(d.fetch)->srv_slot;
        float constants[40];
        std::memcpy(constants, d.consts, sizeof(d.consts));
        // 2D ortho draws have no translation row in the projection (c3 ==
        // (0,0,0,1)); perspective view-proj rows do. Half-pixel applies to
        // the former only.
        const bool ortho = d.consts[12] == 0.0f && d.consts[13] == 0.0f &&
                           d.consts[14] == 0.0f && d.consts[15] == 1.0f;
        constants[36] = ortho ? 1.0f : 0.0f;
        constants[37] = 0.0f;
        constants[38] = 0.0f;
        constants[39] = 0.0f;
        list.D3DSetGraphicsRoot32BitConstants(0, 40, constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu =
            g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
        srv_gpu.ptr += size_t(srv_slot) * g_r.srv_size;
        context.d3d12.set_graphics_root_descriptor_table(
            context.d3d12.command_processor_user_data, 1, srv_gpu);
        D3D12_VERTEX_BUFFER_VIEW vbv{
            g_r.ui_ring->GetGPUVirtualAddress() + ui_region + ui_offset, bytes, d.stride};
        list.D3DIASetVertexBuffers(0, 1, &vbv);
        list.D3DIASetPrimitiveTopology(d.prim == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                   : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.D3DDrawInstanced(d.count, 1, 0, 0);
        ui_offset += bytes;
        ++drawn_2d;
      };

      list.D3DOMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      for (const Draw2d& d : scene_2d) {
        emit_draw(d);
      }
    }
  }

  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        context.d3d12.guest_output_resource,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        context.d3d12.guest_output_initial_state);
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  g_pw_render.Add(perf_ns_since(render_t0));
  const uint64_t frames = g_frames_rendered.fetch_add(1) + 1;
  if (frames % 600 == 0) {
    // CPU-side perf snapshot for this 600-frame window. guest_fps is derived
    // from the guest frame interval; capture/build run on the guest render
    // thread (they extend guest frame time directly), render/items/shadow on
    // the command processor thread, decode inline on the render thread
    // (count = decodes this window; max = the worst single decode).
    const double guest_dt_ms = g_pw_guest_dt.AvgMs();
    REXLOG_INFO(
        "native-scene perf: guest_fps={:.0f} guest_dt_max={:.1f}ms "
        "capture={:.2f}/{:.2f}ms build={:.2f}/{:.2f}ms | render={:.2f}/{:.2f}ms "
        "items={:.2f}/{:.2f}ms shadow={:.2f}/{:.2f}ms "
        "decode[mesh n={} avg={:.2f} max={:.2f}ms tex n={} avg={:.2f} max={:.2f}ms] "
        "commit={:.2f}/{:.2f}ms itemcache[hit={} build={}] cam[chg={} rep={} maxstreak={}]",
        guest_dt_ms > 0.0 ? 1000.0 / guest_dt_ms : 0.0, g_pw_guest_dt.MaxMs(),
        g_pw_capture.AvgMs(), g_pw_capture.MaxMs(), g_pw_build.AvgMs(),
        g_pw_build.MaxMs(), g_pw_render.AvgMs(), g_pw_render.MaxMs(),
        g_pw_items.AvgMs(), g_pw_items.MaxMs(), g_pw_shadow.AvgMs(),
        g_pw_shadow.MaxMs(), g_pw_mesh_decode.count.load(std::memory_order_relaxed),
        g_pw_mesh_decode.AvgMs(), g_pw_mesh_decode.MaxMs(),
        g_pw_tex_decode.count.load(std::memory_order_relaxed), g_pw_tex_decode.AvgMs(),
        g_pw_tex_decode.MaxMs(), g_pw_commit.AvgMs(), g_pw_commit.MaxMs(),
        g_item_cache_hits.exchange(0, std::memory_order_relaxed),
        g_item_cache_builds.exchange(0, std::memory_order_relaxed),
        g_cam_changes.exchange(0, std::memory_order_relaxed),
        g_cam_repeats.exchange(0, std::memory_order_relaxed),
        g_cam_max_streak.exchange(0, std::memory_order_relaxed));
    for (PerfWindow* w : {&g_pw_guest_dt, &g_pw_capture, &g_pw_build, &g_pw_render,
                          &g_pw_items, &g_pw_shadow, &g_pw_mesh_decode,
                          &g_pw_tex_decode, &g_pw_commit}) {
      w->Reset();
    }
  }
  if (frames % 600 == 0) {
    REXLOG_INFO(
        "native-scene: frame {} items={} draws={} draws_2d={} drawn_2d={} "
        "splines[{}/{}] "
        "2d[other={} dropped={} textures={}] cached_meshes={} textures={} "
        "vs_uploads={} palettes={} palette_base_plus1={} ropa[rigid={} stale={} rescued={}] skinned={} skinned_skipped={} "
        "rigid[pending={} dropped={} worldprops={}] "
        "rej[dyn={} range={} chain={} geom={} draws={} bbox={}] "
        "rr[decode_fail={} no_bones={} mesh_deferred={} tex_deferred={}] "
        "shadow[valid={} ready={} draws={}] char[attempt={} valid={} drawn={} reused={} "
        "bones_rescued={}] dynobj[valid={} drawn={}]",
        frames, scene.items.size(), drawn, g_draws_2d.load(), drawn_2d,
        drawn_spline, g_draws_spline.load(),
        g_draws_2d_other.load(), g_draws_2d_dropped.load(), g_r.textures_2d.size(),
        g_r.meshes.size(), g_r.textures.size(),
        g_vs_uploads.load(), g_palette_snapshots.load(), g_palette_base_plus1.load(),
        g_ropa_rigid.load(), g_ropa_stale.load(), g_ropa_rescued.load(),
        g_skinned_items.load(),
        g_skinned_skipped.load(), g_rigid_pending.load(), g_rigid_dropped.load(),
        g_world_props.load(),
        g_rej_no_dynstate.load(), g_rej_dyn_range.load(),
        g_rej_chain.load(), g_rej_geom.load(), g_rej_draws.load(), g_rej_bbox.load(),
        g_rr_decode_fail.load(), g_rr_no_bones.load(), g_rr_mesh_deferred.load(),
        g_rr_tex_deferred.load(), scene.shadow_valid, shadow_ready, shadow_draws,
        g_char_attempts.load(), g_char_valid.load(), g_char_drawn.load(),
        g_char_rows_reused.load(), g_bones_rescued.load(), scene.dynobj_valid,
        g_dynobj_drawn.load());
  }
  return true;
}

}  // namespace

void Install() {
  // Registered even when the scene cvar starts off: RenderScene yields to the
  // emulated output while disabled, and the runtime toggle (F5) can flip the
  // cvar live at any point after boot.
  rex::graphics::SetNativeGuestOutputRenderer(&RenderScene, nullptr);
  REXLOG_INFO("native-scene: guest output renderer registered (scene {})",
              SceneEnabled() ? "on" : "off");
}

}  // namespace skate3::native_scene

#else  // !REX_HAS_D3D12

namespace skate3::native_scene {
void Install() {}
void FlushTextureCache() {}
void FlushMeshCache() {}
int CycleSyntheticPan() { return 0; }
void RecordCameraSignal(double) {}
void RecordBoneSignal(double) {}
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12
