#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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
                     "Max new mesh decodes per rendered frame (0 = unlimited, the "
                     "default: budgeting made the world stream in visibly slowly for no "
                     "measured perf win). Each decode converts vertices and creates GPU "
                     "upload resources on the render thread.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_texture_decode_budget, 0, "Skate 3",
                     "Max new texture decodes per rendered frame (0 = unlimited, the "
                     "default: budgeting made the world stream in visibly slowly for no "
                     "measured perf win; deferred textures render white until their "
                     "turn).")
    .range(0, 100000)
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
FrameScene g_scene;
uint64_t g_generation = 0;
// Debug-dialog cache flushes: consumed at the top of RenderScene so texture/
// mesh-affecting toggles (mip chains, 565 fixes, ...) take effect immediately
// instead of only for newly streamed content.
std::atomic<bool> g_flush_textures{false};
std::atomic<bool> g_flush_meshes{false};
std::atomic<uint8_t*> g_guest_base{nullptr};
std::atomic<uint64_t> g_frames_rendered{0};

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
// Outline color, refreshed from the guest postfx_edgedetectstencil draw's
// PS c0 (the park-editor blue in every capture) whenever that pass runs.
float g_outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
// PIXEL banks keep the CSM constants at c0..c8, pass-global (identical on
// every environment-family draw of the pass; character/hair/tree PSes
// allocate differently and are rejected by the sanity gate). Captured on the
// same camera-keyed main-pass draws as the fog rows.
float g_shadow_rows[36] = {};
bool g_shadow_have = false;
bool g_shadow_frame_done = false;
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
// Discriminate by skinning a few sample vertices with both candidate bases
// and projecting them with the pass's own viewproj (bank c0..c3,
// column-vector rows): the game drew this mesh with these constants, so
// only the correct base puts the samples inside the clip volume (validated
// offline across every skinned draw of an F10 capture: correct base 1.00,
// wrong base <= ~0.3). Only +1 is tested: a +3 shift (whole-bone
// misalignment) also projects fine and no such layout has been seen.
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
  const auto score = [&](uint32_t pb) -> int {
    int ok = 0;
    int n = 0;
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
      if (std::abs(clip[0]) <= 1.5f * aw && std::abs(clip[1]) <= 1.5f * aw) {
        ++ok;
      }
    }
    return n == 0 ? -1 : (ok * 16) / n;
  };
  const int s_std = score(palette_base);
  const int s_plus = score(palette_base + 1);
  if (s_plus > s_std) {
    g_palette_base_plus1.fetch_add(1, std::memory_order_relaxed);
    return palette_base + 1;
  }
  return palette_base;
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
bool GuestTryCopy(void* dst, const void* src, size_t size) {
  __try {
    std::memcpy(dst, src, size);
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
  item.normal_offset = 0;
  item.normal_fmt = 0;
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
    } else if (usage == 3 && item.normal_fmt == 0) {
      const uint8_t fmt = uint8_t(REX_LOAD_U32(e + 4) & 0x3F);
      if (fmt == 16) {  // k_10_11_11 packed normal
        item.normal_offset = REX_LOAD_U16(e + 2);
        item.normal_fmt = fmt;
      }
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
  if (!have_pos) {
    g_rej_geom.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  item.skinned = have_bw && have_bi;
  item.stride = REX_LOAD_U8(vdesc + (num_elements + 1) * 16);
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
  item.vb_addr = REX_LOAD_U32(vb + kBufferPhysAddr) & 0xFFFFFFFC;
  item.vb_bytes = REX_LOAD_U32(vb + kVbBytes);
  item.ib_addr = REX_LOAD_U32(ib + kBufferPhysAddr) & 0xFFFFFFFC;
  item.ib_count = REX_LOAD_U32(ib + kIbCount);
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
  item.ropa = false;
  item.decal = false;
  item.decal_tileable = false;
  item.transparent = false;
  item.water = false;
  item.water_normal = 0;
  item.water_env = 0;
  item.unlit = false;
  item.tint[0] = item.tint[1] = item.tint[2] = item.tint[3] = 0.0f;
  const uint32_t material = REX_LOAD_U32(mesh + kMeshMaterial);
  if (GuestReadableApprox(base, material)) {
    const uint32_t num_channels = REX_LOAD_U32(material);
    const uint32_t channels = REX_LOAD_U32(material + 8);
    if (num_channels <= 32 && GuestReadableApprox(base, channels)) {
      for (uint32_t i = 0; i < num_channels; ++i) {
        const uint32_t chan = channels + i * 0x20;
        const uint32_t name = REX_LOAD_U32(chan);
        if (!GuestReadableApprox(base, name)) continue;
        char text[20] = {};
        for (int k = 0; k < 19; ++k) {
          text[k] = char(REX_LOAD_U8(name + k));
          if (text[k] == '\0') break;
        }
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
        } else if (std::memcmp(text, "environment", 12) == 0) {
          // Environment CUBE map: the water reflection term.
          slot = &item.water_env;
        } else if (std::memcmp(text, "macroOverlayUVScale", 20) == 0 ||
                   std::memcmp(text, "macroOverlayOpacity", 20) == 0) {
          // Shader-constant channel: the float lives in the first guid word.
          const float f = std::bit_cast<float>(REX_LOAD_U32(chan + 0x10));
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
          const uint32_t s = REX_LOAD_U32(chan + 0x18);
          if (GuestReadableApprox(base, s)) {
            char mat_name[28] = {};
            for (int k = 0; k < 27; ++k) {
              mat_name[k] = char(REX_LOAD_U8(s + k));
              if (mat_name[k] == '\0') break;
            }
            item.hair = std::memcmp(mat_name, "character.hair", 15) == 0;
            item.unlit = std::memcmp(mat_name, "sky.", 4) == 0;
            item.ropa = std::memcmp(mat_name, "character.cloth_ropa", 21) == 0;
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
          }
          continue;
        }
        if (slot != nullptr && *slot == 0) {
          // Prefer the channel's live stream record (chan+0x1C -> word 0 =
          // the renderengine::Texture actually bound): runtime-composed
          // customization textures (CAS face/skin, shoes, deck, wheels)
          // are never registered under an asset GUID. Validate via the
          // fetch-constant type bits before trusting the pointer.
          const uint32_t stream = REX_LOAD_U32(chan + 0x1C);
          if (GuestReadableApprox(base, stream)) {
            const uint32_t tex = REX_LOAD_U32(stream);
            if (GuestReadableApprox(base, tex) &&
                (REX_LOAD_U32(tex + 7 * 4) & 3u) == 2u) {
              *slot = tex;
            }
          }
          if (*slot == 0) {
            const uint64_t guid =
                (uint64_t(REX_LOAD_U32(chan + 0x10)) << 32) | REX_LOAD_U32(chan + 0x14);
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
  std::memset(item.world, 0, sizeof(item.world));
  item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
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
  if (!BuildItemFromMesh(base, mesh, item)) {
    return false;
  }

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
      g_scene = FrameScene{};
    }
    {
      std::lock_guard<std::mutex> lock(g_2d_mutex);
      g_scene_2d.clear();
      g_scene_spline.clear();
    }
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
      // Sim inactive: the layout is exact (no refine needed).
      palette_base = main_pass ? 8u : 5u;
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
  if ((!g_fog_frame_done || !g_shadow_frame_done || !g_sky_frame_done) && func == 0 &&
      flags2d == 0 && SceneEnabled() &&
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
      if (!g_shadow_frame_done && ps_bank != 0 &&
          REXCVAR_GET(skate3_native_render_scene_shadows)) {
        float rows[36];
        for (int i = 0; i < 36; ++i) {
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
        const bool sane =
            mx2 > 0.01f && mx2 < 4.0f && std::fabs(mx2 - my2) < 0.05f * mx2 &&
            s1 > 0.0f && s1 < 1.0f && std::fabs(rows[5] - s1) < 1e-4f &&
            s2 > 0.0f && s2 < s1 && std::fabs(rows[9] - s2) < 1e-4f &&
            std::fabs(rows[16]) < 0.02f && std::fabs(rows[18]) < 0.02f &&
            std::fabs(rows[17]) > 0.005f && std::fabs(rows[17]) < 1.0f &&
            rows[32] >= 0.0f && rows[32] <= 1.0f && rows[33] >= 0.0f &&
            rows[33] <= 1.0f && rows[34] >= 0.0f && rows[34] <= 1.0f;
        if (sane) {
          std::memcpy(g_shadow_rows, rows, sizeof(rows));
          g_shadow_have = true;
          g_shadow_frame_done = true;
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
      // postfx edge-detect: PS c0 = the outline color as staged (the
      // park-editor blue (0.216, 0.647, 1.0) in every capture).
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
  g_frame_pending_by_buffers.erase(oldest);
}

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  if (!SceneEnabled()) {
    return;
  }
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
  }
  // Take this frame's selection re-draw captures and re-arm the post-sky
  // window (must happen on every exit path, like the dynitems swap).
  std::vector<SelectedDrawKey> frame_selected;
  frame_selected.swap(g_frame_selected);
  g_sky_seen_this_frame = false;
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
        // Deferred mesh whose draw never came: no valid palette/transform.
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
  if (scene.items.empty()) {
    return;
  }
  // Selected-object outline: flag items matching this frame's post-sky
  // re-draw captures. >= 2 identical draws = the stencil-marking pair; a
  // single occurrence is a legitimately late-drawn object, not a selection.
  {
    uint32_t outline_items = 0;
    for (const SelectedDrawKey& k : frame_selected) {
      if (k.count < 2) {
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

  std::lock_guard<std::mutex> lock(g_scene_mutex);
  scene.generation = ++g_generation;
  g_scene = std::move(scene);
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
          << "\",\"lightmap\":\"" << d.lightmap_tex << "\",\"macro\":\"" << d.macro_tex
          << "\",\"decal_art\":\"" << d.decal_art
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
  float4 sh_misc;   // x = depth bias [PS c5.x], y = enable, z = lit level L
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
  if (tint.g == 0.0 && overlay.w == 0.0) {
    // environment.transparent alpha-tests its SQUARED alpha at ref 16/255
    // (transparentenvironment.xml: ALPHAREF 16, PS outputs a = diffuse.a^2).
    clip(misc.x > 0.0 ? albedo.a * albedo.a - 0.0627 : albedo.a - 0.35);
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
  // otherwise fall back to derivative face shading.
  float3 lit;
  if (tint.b > 0.0) {
    lit = albedo.rgb;  // unlit (sky dome)
  } else if (tint.r > 0.0) {
    lit = albedo.rgb * lightmap.Sample(smp, i.uv2).rgb * 2.0;
  } else {
    // Smooth per-vertex normal when the mesh has one; face normal from
    // position derivatives otherwise.
    float3 n = dot(i.nrm, i.nrm) > 0.01 ? normalize(i.nrm)
                                        : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float l = abs(dot(n, normalize(float3(0.4, 0.8, 0.3)))) * 0.35 + 0.75;
    lit = albedo.rgb * l;
  }
  // Dynamic CSM shadow receive (world geometry + rigid props; characters
  // need the game's separate PCF/bias variant; skipping them avoids
  // self-shadow acne, and the ground shadow is 95% of the visible effect).
  // Exact receiver math from the baseenvironment PS disassembly: finest
  // cascade whose |ls| < 0.99 wins; shadow = saturate(infront + 1 -
  // coverage). The game min-clamps the LINEAR lighting to (s + shadowColor)
  // - our empirical shading is gamma, so the equivalent saturating curve
  // f = saturate((s + luma) / L), col *= sqrt(f) is used (a plain lerp
  // shows the whole Gaussian penumbra and looks conspicuously blurrier
  // than the emulated edge; L ~ lit ground lighting level, ~0.45 linear).
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
      // Per-channel: the game clamps the linear lighting to s + c8.rgb, so
      // full shadow takes on c8's cool blue cast ((0.05,0.09,0.13) in every
      // capture so far); a scalar multiply left the shadow warm-brown.
      float3 f = saturate((s + sh_color.rgb) / sh_misc.z);
      lit *= sqrt(f);
    }
  }
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
bool DecodeMesh(ID3D12Device* device, uint8_t* base, const DrawItem& item,
                MeshBuffers& out) {
  const uint32_t num_verts = item.vb_bytes / item.stride;
  if (num_verts == 0) return false;
  // This runs on the render thread; the guest payloads were valid on the
  // game thread this frame but streaming can decommit them in between.
  // Copy them out with the lock-free guarded copy (never VirtualQuery here:
  // the VAD lock stalls behind the guest streaming threads while panning).
  static thread_local std::vector<uint8_t> vb_scratch;
  static thread_local std::vector<uint8_t> ib_scratch;
  vb_scratch.resize(item.vb_bytes);
  if (!GuestTryCopy(vb_scratch.data(), base + item.vb_addr, item.vb_bytes)) {
    return false;
  }
  if (!item.cloth_quads) {
    ib_scratch.resize(size_t(item.ib_count) * 2);
    if (!GuestTryCopy(ib_scratch.data(), base + item.ib_addr, size_t(item.ib_count) * 2)) {
      return false;
    }
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

  const uint8_t* src_vb = vb_scratch.data();
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
    dst[v * 14 + 5] = std::fabs(dst[v * 14 + 5]);
    dst[v * 14 + 6] = std::fabs(dst[v * 14 + 6]);
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
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    if (item.normal_fmt == 16) {
      const uint32_t word = SwapU32(*reinterpret_cast<const uint32_t*>(
          src_vb + size_t(v) * item.stride + item.normal_offset));
      const int32_t ix = int32_t(word << 21) >> 21;
      const int32_t iy = int32_t((word >> 11) << 21) >> 21;
      const int32_t iz = int32_t((word >> 22) << 22) >> 22;
      nx = float(ix) / 1023.0f;
      ny = float(iy) / 1023.0f;
      nz = float(iz) / 511.0f;
    }
    dst[v * 14 + 9] = nx;
    dst[v * 14 + 10] = ny;
    dst[v * 14 + 11] = nz;
  }
  vb->Unmap(0, nullptr);
  // Blend indices outside the captured palette read garbage rows and mangle
  // the vertex. Indices are plain bone numbers, bone k = palette rows 3k.
  if (item.skinned && max_bi >= 0) {
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
    const uint16_t* src_ib = reinterpret_cast<const uint16_t*>(ib_scratch.data());
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
      two_sided = twins * 10 >= tris.size() * 6;
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

  if (g_r.srv_next >= 8192) {
    return false;
  }
  out.srv_slot = g_r.srv_next++;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping =
      ComposeSrvSwizzle(fetch_swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
  srv.Texture2D.MipLevels = mip_count;
  D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
  device->CreateShaderResourceView(out.texture, &srv, slot);
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = size;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = true;
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
  if (g_r.srv_next >= 8192) {
    return false;
  }
  out.srv_slot = g_r.srv_next++;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = host.srv_format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
  srv.Texture2D.MipLevels = mip_count;
  D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
  device->CreateShaderResourceView(out.texture, &srv, slot);
  // Payload sample for content revalidation (see GuestTexture).
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = srcs[0].size;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = true;
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
  if (g_r.srv_next >= 8192) {
    return false;
  }
  out.srv_slot = g_r.srv_next++;
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
    D3D12_ROOT_PARAMETER params[9] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    // NOTE the 64-DWORD root-signature budget: 52 constants + 6 descriptor
    // tables (1 each) + 1 root SRV (2) + 1 root CBV (2) = 62. Going past 64
    // makes CreateRootSignature fail (renderer falls back to emulated).
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
    desc.NumParameters = 9;
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
      cp.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
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
      bp.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
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
    // R16G16_FLOAT targets, 3 tiles of tile x tile each; RTV heap slots
    // 2/3/4.
    g_r.shadow_tile = uint32_t(REXCVAR_GET(skate3_native_render_scene_shadow_tile));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = g_r.shadow_tile * 3;
    desc.Height = g_r.shadow_tile;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
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
      REXLOG_INFO("native-scene: {} (presence context)",
                  in_menus ? "menus/pause - yielding to emulated output"
                           : "gameplay - rendering natively");
    }
    if (in_menus) {
      return false;
    }
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

  // Debug-dialog cache flushes: retire every cached decode (freed once the
  // GPU is done with the current submission) so hot-toggled decode settings
  // rebuild the world with the new rules this frame.
  if (g_flush_textures.exchange(false, std::memory_order_relaxed)) {
    const uint64_t submission = command_processor->GetCurrentSubmission();
    for (auto& [key, t] : g_r.textures) {
      if (t.texture) g_r.retired.emplace_back(t.texture, submission);
      if (t.upload) g_r.retired.emplace_back(t.upload, submission);
    }
    g_r.textures.clear();
    for (auto& [key, t] : g_r.textures_2d) {
      if (t.texture) g_r.retired.emplace_back(t.texture, submission);
      if (t.upload) g_r.retired.emplace_back(t.upload, submission);
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
    REXLOG_INFO("native-scene: mesh cache flushed (debug dialog)");
  }

  // Reset this frame's bone ring region (shared by the shadow casters and
  // the main pass: the shadow pass allocates first, the main pass appends).
  const uint64_t frame_number = g_frames_rendered.load(std::memory_order_relaxed);
  const uint32_t bone_region =
      uint32_t(frame_number % RendererState::kBoneRegions) *
      RendererState::kBoneRegionSize;
  g_r.bone_ring_offset = 0;

  // ---- Dynamic-shadow atlas pass ----
  // Renders the frame's dynamic casters (skinned characters + rigid
  // non-identity-world props: exactly the game's caster list) into the
  // three cascade tiles with the captured light rows, then applies the
  // game's coverage blur + depth dilation. Runs before the main pass so the
  // scene shader can sample the finished atlas.
  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
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
          if (mit == g_r.meshes.end() || mit->second.fingerprint != c.item->fingerprint) {
            continue;  // decoded by the main pass below; casts from next frame
          }
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
      cb[26] = 0.45f;   // lit ground lighting level L for the saturating curve
    }
    list.D3DSetGraphicsRootConstantBufferView(
        6, g_r.shadow_cb->GetGPUVirtualAddress() + cb_offset);
    D3D12_GPU_DESCRIPTOR_HANDLE atlas = g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
    atlas.ptr += size_t(shadow_ready ? g_r.shadow_srv_final : g_r.white.srv_slot) *
                 g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 7, atlas);
  }

  uint32_t drawn = 0;
  uint32_t item_index = 0;
  // Per-frame decode budgets: panning/streaming can surface dozens of new
  // meshes and textures in one frame, and each decode does CPU conversion +
  // CreateCommittedResource on the render thread; unbounded, that is a
  // visible hitch. Over-budget work is deferred; at native frame rates the
  // pop-in lasts a few tens of milliseconds.
  const int32_t mesh_budget = REXCVAR_GET(skate3_native_render_scene_mesh_decode_budget);
  const int32_t tex_budget = REXCVAR_GET(skate3_native_render_scene_texture_decode_budget);
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

  const auto draw_item = [&](const DrawItem& item) {
    auto it = g_r.meshes.find(item.mesh);
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint &&
        REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
      const uint64_t submission = command_processor->GetCurrentSubmission();
      g_r.retired.emplace_back(it->second.vb, submission);
      g_r.retired.emplace_back(it->second.ib, submission);
      g_r.meshes.erase(it);
      it = g_r.meshes.end();
    }
    if (it == g_r.meshes.end()) {
      if (mesh_budget > 0 && mesh_decodes >= uint32_t(mesh_budget)) {
        g_rr_mesh_deferred.fetch_add(1, std::memory_order_relaxed);
        return;  // decodes on a later frame
      }
      ++mesh_decodes;
      MeshBuffers buffers;
      if (!DecodeMesh(g_r.device, base, item, buffers)) {
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
    if (use_depth && !item.transparent && !item.water && g_r.pso_cullback != nullptr) {
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
        const bool words_changed =
            std::memcmp(live, tit->second.fetch_words, sizeof(live)) != 0;
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
          const uint64_t submission = command_processor->GetCurrentSubmission();
          if (tit->second.texture) g_r.retired.emplace_back(tit->second.texture, submission);
          if (tit->second.upload) g_r.retired.emplace_back(tit->second.upload, submission);
          g_r.textures.erase(tit);
          tit = g_r.textures.end();
        }
      }
      if (tit == g_r.textures.end()) {
        if (tex_budget > 0 && tex_decodes >= uint32_t(tex_budget)) {
          g_rr_tex_deferred.fetch_add(1, std::memory_order_relaxed);
          return &g_r.white;  // decodes on a later frame
        }
        ++tex_decodes;
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
          // Failed decodes render white: log each once (capped) so white
          // meshes are always attributable to a specific texture.
          static std::unordered_set<uint32_t> logged_failed;
          if (logged_failed.size() < 64 && logged_failed.insert(tex_ptr).second) {
            REXLOG_INFO(
                "native-scene: texture decode FAILED obj={:08X} fetch=[{:08X} {:08X} "
                "{:08X} {:08X} {:08X} {:08X}]",
                tex_ptr, gt.fetch_words[0], gt.fetch_words[1], gt.fetch_words[2],
                gt.fetch_words[3], gt.fetch_words[4], gt.fetch_words[5]);
          }
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
    constants[39] = 0.0f;
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
    if (item.water && item.water_normal != 0) {
      // Water rides its ripple normal map in the macro slot (water never
      // carries a macro overlay; overlay.z stays 0 below so the macro
      // composite path never runs).
      macro_tex = resolve_texture(item.water_normal);
    }
    // Water environment cube (t6, root param 8): decoded once per guest
    // object into the cube cache; the gray fallback cube otherwise.
    const GuestTexture* cube_tex = &g_r.white_cube;
    if (item.water && item.water_env != 0) {
      auto cit = g_r.cube_textures.find(item.water_env);
      if (cit == g_r.cube_textures.end()) {
        GuestTexture c{};
        if (EnsureGuestCubeTexture(context, base, item.water_env, c)) {
          cit = g_r.cube_textures.emplace(item.water_env, c).first;
        } else {
          if (c.upload) c.upload->Release();
          if (c.texture) c.texture->Release();
          c = GuestTexture{};
          c.valid = false;
          cit = g_r.cube_textures.emplace(item.water_env, c).first;  // negative-cache
        }
      }
      if (cit->second.valid) {
        cube_tex = &cit->second;
      }
    }
    const GuestTexture* decal_tex = item.decal && item.decal_art != 0 &&
                                            REXCVAR_GET(skate3_native_render_scene_decals)
                                        ? resolve_texture(item.decal_art)
                                        : &g_r.white;
    const bool is_decal = decal_tex != &g_r.white;
    constants[44] = item.macro_scale;
    constants[45] = item.macro_opacity;
    constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
    // overlay.w: 1 = single-placement decal (art clamps), 2 = tileable
    // decal (art wraps; clamping a many-period uv range stretched the
    // border texels into the giant cliff-face streaks).
    constants[47] = is_decal ? (item.decal_tileable ? 2.0f : 1.0f) : 0.0f;
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
  std::vector<const DrawItem*> transparent_items;
  for (const DrawItem& item : scene.items) {
    const uint32_t index = item_index++;
    if (debug_mode == 1) {
      break;
    }
    if (debug_mode == 3 && index >= 20) {
      break;
    }
    if ((item.transparent || item.water) && debug_mode == 0) {
      if (REXCVAR_GET(skate3_native_render_scene_transparents)) {
        transparent_items.push_back(&item);
      }
      continue;
    }
    draw_item(item);
  }
  if (!transparent_items.empty() && g_r.pso_transparent != nullptr) {
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
    std::stable_sort(transparent_items.begin(), transparent_items.end(),
                     [&](const DrawItem* a, const DrawItem* b) {
                       return view_dist2(*a) > view_dist2(*b);
                     });
    list.D3DSetPipelineState(use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
    for (const DrawItem* item : transparent_items) {
      draw_item(*item);
    }
  }

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
    auto it = g_r.textures_2d.find(key);
    if (it == g_r.textures_2d.end()) {
      GuestTexture gt;
      EnsureGuestTextureFromWords(context, base, fetch, gt);
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

  const uint64_t frames = g_frames_rendered.fetch_add(1) + 1;
  if (frames % 600 == 0) {
    REXLOG_INFO(
        "native-scene: frame {} items={} draws={} draws_2d={} drawn_2d={} "
        "splines[{}/{}] "
        "2d[other={} dropped={} textures={}] cached_meshes={} textures={} "
        "vs_uploads={} palettes={} palette_base_plus1={} ropa[rigid={} stale={}] skinned={} skinned_skipped={} "
        "rigid[pending={} dropped={} worldprops={}] "
        "rej[dyn={} range={} chain={} geom={} draws={} bbox={}] "
        "rr[decode_fail={} no_bones={} mesh_deferred={} tex_deferred={}] "
        "shadow[valid={} ready={} draws={}]",
        frames, scene.items.size(), drawn, g_draws_2d.load(), drawn_2d,
        drawn_spline, g_draws_spline.load(),
        g_draws_2d_other.load(), g_draws_2d_dropped.load(), g_r.textures_2d.size(),
        g_r.meshes.size(), g_r.textures.size(),
        g_vs_uploads.load(), g_palette_snapshots.load(), g_palette_base_plus1.load(),
        g_ropa_rigid.load(), g_ropa_stale.load(), g_skinned_items.load(),
        g_skinned_skipped.load(), g_rigid_pending.load(), g_rigid_dropped.load(),
        g_world_props.load(),
        g_rej_no_dynstate.load(), g_rej_dyn_range.load(),
        g_rej_chain.load(), g_rej_geom.load(), g_rej_draws.load(), g_rej_bbox.load(),
        g_rr_decode_fail.load(), g_rr_no_bones.load(), g_rr_mesh_deferred.load(),
        g_rr_tex_deferred.load(), scene.shadow_valid, shadow_ready, shadow_draws);
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
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12
