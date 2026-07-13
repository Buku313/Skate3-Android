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
#include <deque>
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

#include "native/skate3_native_diag.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_v3_shadow.h"
#include "native/skate3_native_v3_shadow_mat.h"

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
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_photo_yield, true, "Skate 3",
                    "Yield to the emulated output while a photo-mission's photo editor "
                    "(the FE PhotoSelect screen) is up. The editor's depth of field / "
                    "saturation / brightness / contrast / lens vignette are the game's "
                    "own postfx chain, which native rendering suppresses; natively "
                    "the photo showed the raw scene and the effect controls did "
                    "nothing. The scene is frozen there, so emulated-path performance "
                    "is fine. Detected by polling the FrontEndManager NIS push-state "
                    "stack (plus the PhotoReplayController heartbeat as a backup).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_readback, true, "Skate 3",
    "While a photo flow is active (the photo-mission editor, or a few "
    "seconds after any TakePhoto), arm the SDK's forced small-resolve CPU "
    "readback (native_render_force_resolve_readback_max_length) and lift "
    "emulated-draw suppression. The game takes photos by CPU-reading a "
    "resolved 1152x640 PostFX screenshot target from guest memory "
    "(ScreenshotBackEnd::GrabScreenshot -> JPEG); photo missions keep the "
    "gameplay presence context so no readback path ever ran and the grabbed "
    "memory stayed all-zero; the final photo display and the saved photo "
    "were BLACK. Costs one synchronous readback per small resolve, photo-flow "
    "frames only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_photo_native, true, "Skate 3",
    "Render the photo-mission photo editor NATIVELY, applying the game's "
    "own postfx chain (depth of field / saturation / brightness / contrast "
    "/ lens vignette) as exact ucode ports (photo_fx.hlsl: visualfx -> DOF "
    "downsample -> tap9dofMotionBlur -> tap9dof -> uber -> fisheye) driven "
    "by the LIVE constants the game stages for its own (suppressed) postfx "
    "draws each frame. Takes precedence over "
    "skate3_native_render_scene_photo_yield; if the pass captures are not "
    "yet fresh (first frames of the editor), the scene renders without the "
    "effects until they land. Known deltas: the motion-accumulation feed "
    "(see photo_native_accum), the bloom pyramid contribution (disabled in "
    "every editor capture), grade LUT served as identity, deck AO.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_photo_native_accum, 0, "Skate 3",
    "Source for the visualfx pass's quarter-res motion-accumulation input "
    "(the game feeds a jitter-accumulated buffer natively unmodeled): 0 = "
    "black, 1 = downsampled scene (pre-grade), 2 = downsampled final frame. "
    "Compare against the emulated editor (F11 pair) and keep the match.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_pause_native, true, "Skate 3",
                    "Keep the NATIVE renderer active through the in-game pause menu "
                    "instead of yielding to the emulated output. Pause is told apart "
                    "from loading screens / the boot frontend by the world still "
                    "submitting perspective scenes while the presence context reads 0 "
                    "(loads and the frontend stop publishing within ~300 ms, which "
                    "falls back to the yield path and its cache clears). Native pause "
                    "also skips the pause-entry cache clears, so unpausing costs "
                    "nothing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_loading_native, true, "Skate 3",
                    "Render post-startup loading screens natively, black backdrop "
                    "plus the game's own captured 2D loading UI, instead of "
                    "yielding to the emulated output. The loading-screen "
                    "housekeeping (cache clears, registration prewarm, takeover "
                    "arming) is unchanged; only the presented pixels switch source. "
                    "The boot flow before the first gameplay stays emulated.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_boot_native, true, "Skate 3",
                    "Extend native rendering to the game startup flow (intro videos, "
                    "boot frontend, the first load); i.e. drop the first-gameplay "
                    "prerequisite from the native menu/loading modes, and render the "
                    "pre-takeover boot frames as native 2D-over-black instead of "
                    "yielding. With this and the pause/loading modes on, the emulated "
                    "GPU output is never presented.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_fmv_native, true, "Skate 3",
                    "Render FMVs natively: the movie player CPU-decodes VP6 into "
                    "three YUV plane textures (VideoRenderer_RwTexture members, "
                    "published per frame by the Render hook), and the captured "
                    "movie quad (the AptMovieIntegration stride-24 draw; through "
                    "the plain 2D shader it rendered as an opaque black cover, its "
                    "c8 is black) is substituted with the ps_yuv2d combine inside "
                    "the 2D replay, order-faithful and at the quad's own geometry "
                    "(windowed movies place exactly). When off (or the planes fail "
                    "to publish), FMVs fall back to the emulated yield "
                    "(skate3_native_render_scene_fmv_yield).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_fmv_yield, true, "Skate 3",
                    "Yield to the emulated output while an FMV is playing (intro "
                    "logos, any rw::movie playback). The video frame is CPU-decoded "
                    "into a texture and reaches the screen through the game's postfx "
                    "chain + swap without any capturable 2D draw (F11-proven on the "
                    "boot intro: 15 draws, all postfx passes + fade fills, none "
                    "sampling the video), so the native path has nothing to replay; "
                    "the emulated frame is complete and correct there, same class as "
                    "the photo editor. Detected via the MovieDecoder::Decode "
                    "heartbeat; native rendering resumes within 0.5 s of the last "
                    "decoded frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_cas_yield, false, "Skate 3",
                    "Yield to the emulated output while the create-a-skater editor "
                    "(the CAS 'Edit Skater' screen) is up. OFF by default since the "
                    "editor renders natively: the char-lighting capture understands "
                    "the editor CAC compiles' shifted constant layout (see "
                    "CaptureCharLighting's editor fam-2 retry), the texture-space "
                    "composite passes (cac*_unwrapPS, pitch <= 512) execute under "
                    "the pitch-selective suppression, and the ropa/palette VS layout "
                    "is unchanged. Known native deltas: the deck's shift-recolor "
                    "masks and the editor's own DOF chain are not modeled. Turn ON "
                    "to get the exact emulated editor instead (photo-editor class; "
                    "detected via FE push-state id 15 + the _nis shader heartbeat).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_menu_rtt_passes, true, "Skate 3",
    "While a menu/pause/loading context (presence context 0) renders "
    "natively, drop native_render_suppress_mode to 0 (suppress "
    "framebuffer-sized passes ONLY) so the game's sub-framebuffer "
    "render-to-texture passes execute; the team-menu/Import-Skater skater "
    "portrait boxes are one-shot RTT passes at a surface pitch inside the "
    "mode-2 suppressed band (> 512, != 1024): under mode 2 they never ran "
    "and the boxes stayed empty. The visible frame stays fully native "
    "(framebuffer passes remain suppressed); this is the same "
    "execute-the-composition-passes-emulated class as lightmap pages and "
    "CAS outfit composition. Restored to the configured mode on the first "
    "gameplay frame. Menu-only cost: the midsize postfx-chain passes also "
    "execute there (mode 0 was the long-lived default before mode 2).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_menu_unsuppress, false, "Skate 3",
    "ESCAPE HATCH, normally unnecessary: while a menu/pause/loading context "
    "(presence context 0) renders natively, temporarily clear "
    "native_render_suppress_emulated_draws so ALL emulated passes execute. "
    "The original motivation, the team-menu skater portrait boxes are "
    "one-shot render-to-texture passes that suppression left forever empty "
    ", is covered without this by the SDK's pitch-selective suppression "
    "(native_render_suppress_mode 2: surfaces <= 512 px wide, incl. the "
    "portrait cards, always execute). Turn on only if small offscreen "
    "composites are missing in menus despite that; costs the full emulated "
    "pipeline's GPU time during menus.")
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
// Reflective-glass (env families 5/6) isolation controls for the F12
// dialog: live A/B of each stage of the cube-reflection term against the
// emulated look (F5).
REXCVAR_DEFINE_INT32(skate3_native_render_scene_refl_mode, 0, "Skate 3",
                     "Reflective glass debug: 0 normal, 1 cube term off, 2 cube at "
                     "the absolute LOD in refl_lod, 3 flat normal (no normal-map "
                     "perturb), 4 visualize the cube sample only, 5 body only (no "
                     "spec, no cube), 6 normal-map LOD bias from the slider.")
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_lod, 0.0, "Skate 3",
                      "Reflective glass debug: mode 2 = absolute cube mip level; "
                      "other modes = EXTRA LOD bias on top of the automatic "
                      "640p-parity bias.")
    .range(-4.0, 12.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Constant tangent-space normal tilt on the reflective glass, live-tunable
// (F12). The defaults are DERIVED, not tuned: the material's 16x16 detail
// texture is a constant BC1 block whose endpoints expand on HARDWARE by bit
// replication: 5-bit red 16 -> (16<<3)|(16>>2) = 132/255, 6-bit green 32
// -> (32<<2)|(32>>4) = 130/255; so the shader's 2*d - 1 fold is exactly
// (0.035294, 0.019608). An earlier fold used our CPU decoder's
// integer-division expansion (131/129 -> 0.028/0.012), leaving a ~0.8 deg
// constant normal tilt = the residual reflection-position offset that was
// dialed out by hand to (0.036, 0.019), matching the hardware value to
// the slider step and confirming the derivation.
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_bias_x, 0.035294, "Skate 3",
                      "Reflective glass: constant tangent-X (horizontal) normal "
                      "tilt added to the normal-map sample (= the detail "
                      "constant's hardware-BC1 fold).")
    .range(-0.2, 0.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_refl_bias_y, 0.019608, "Skate 3",
                      "Reflective glass: constant binormal-Y (vertical) normal "
                      "tilt added to the normal-map sample (= the detail "
                      "constant's hardware-BC1 fold).")
    .range(-0.2, 0.2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_refl_bias_auto, true, "Skate 3",
                    "Derive the reflective-glass normal tilt from each material's "
                    "own detail texture (hardware-exact BC1 decode of its constant "
                    "color) instead of the refl_bias_x/y values. The sliders "
                    "remain the fallback/override with auto off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
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
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_entity_fade, true, "Skate 3",
                    "Honor the game's per-entity spawn/distance fade: LivingWorld "
                    "pres entities (NPCs, traffic vehicles) publish an opacity that "
                    "every character-family PS writes as output alpha (peds c21.x, "
                    "defaultcharacter c13.x, cacstamp c22.x, vehicle body c20.x). "
                    "The game submits their draws at alpha 0 through the whole "
                    "spawn settle (the physics drop) and ramps alpha up afterwards "
                    "/ by distance. Off = the old behavior: entities render fully "
                    "opaque from their first draw (mid-air spawn pop, early "
                    "distant pop-in).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_dynamic_items, true, "Skate 3",
                    "Publish dynamic entities (characters, props, cloth)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_fade, true, "Skate 3",
    "Serve LivingWorld NPC/vehicle fade alpha from the entity itself "
    "(entity+528 via the LW entity store, mapped per MeshContext) instead "
    "of the per-draw captured constant row. The captured row is a per-draw "
    "inference: capture races serve alpha=1 (opaque mid-air spawns, no "
    "fade-in) or a clone's foreign row (one-frame invisibility blinks). "
    "Off = the pre-store captured-row behavior.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_gap_fill, true, "Skate 3",
    "Republish a LivingWorld NPC for up to 2 frames when its MeshContext "
    "drops out of the submit records while the entity is still alive (the "
    "1-3 frame publish GAPs: an in-view NPC vanishing for a frame reads as "
    "a blink/teleport). Off = gaps render as-is.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_native_render_scene_lw_identity, true, "Skate 3",
    "Key LivingWorld entities' pose-smoothing rings by their MeshContext "
    "(the game's own per-instance identity) instead of (mesh, occurrence) "
    "pairing. Same-mesh clone reshuffles in the sort lists can then never "
    "mispair a ring (the NPC/prop teleport-slide class). Off = the "
    "positional re-pair heuristics alone.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_inline, false, "Skate 3",
                    "Decode CPU-cloth (ROPA) garment VBs inline on the render thread "
                    "instead of the worker jobs. Measured 4.3ms avg per garment "
                    "decode (committed-resource allocation dominates) AND it does not "
                    "address the jelly; the mismatch is the cloth shape having no "
                    "place on the interpolation play clock, not decode latency. Kept "
                    "for experiments only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_blend, true, "Skate 3",
                    "Lerp CPU-cloth shape generations onto the motion-smoothing play "
                    "clock at draw time (pose<->shape pairing via the interp ring); "
                    "the stepped shape against the interpolated body was the tee "
                    "jelly/clip-through, worse at LOWER fps. OFF = newest decode "
                    "(old behavior).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_native_render_scene_2d_async_px, 1, "Skate 3",
    "Pixel-count threshold above which 2D/HUD texture decodes route to the "
    "words-miss decode workers instead of running inline on the render "
    "thread. Inline 2D decodes stall the frame AND the guest (swap blocks; "
    "guest_dt_max 100-256 ms in menu windows), and the cost "
    "is NOT proportional to texel count: 8888 APT tiles pay a per-PIXEL "
    "tiled-offset computation plus two CreateCommittedResource calls, so "
    "even 64x64 tiles measured 3-19 ms. Default 1 = "
    "everything async: a first sighting skips the quad for the 1-3 frames "
    "the worker needs (imperceptible pop-in at render rate), a content "
    "change keeps serving the stale decode until the commit swaps it. "
    "0 = all inline (old "
    "behavior); larger values gate by pixel count (the original big-art "
    "threshold was 131072).")
    .range(0, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_2d_sharp, 0.0, "Skate 3",
                      "Sharp-magnification amount for the 2D/HUD overlay (0 = plain "
                      "bilinear, up to 2). Much of the APT (Flash) HUD samples "
                      "cached-bitmap tiles whose texel count equals their 720p display "
                      "size (score digits, gauge ring, compass; measured density 1.0 "
                      "in the 2D draw stream) while text batches sample 512x512 glyph "
                      "atlases at 8-10 texels/pixel; at 2-3x output scales the tiles "
                      "blur under bilinear while atlas text stays crisp; the same on "
                      "console/emulated, the content is simply 720p. Catmull-Rom + "
                      "clamped unsharp mask where the fetch is magnified (>1.25x), "
                      "ramped to full by 2x; minified/1:1 fetches untouched. DEFAULT "
                      "0 (off): sharpened ramps read worse than the soft "
                      "bilinear; the honest fix is higher-res source pixels (APT "
                      "cache-tile rasterization scale), not edge-contrast synthesis.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ropa_boxcar, true, "Skate 3",
                    "Blend CPU-cloth shape generations through the SAME 8-tap boxcar "
                    "kernel the body bones/garment world are filtered with (see "
                    "smooth_camera_filter_ms) instead of a plain 2-generation lerp. "
                    "The plain lerp reconstructs the 60Hz limb signal SHARPLY while "
                    "the boxcar rounds the body ~a window; the cloth led the body "
                    "through every direction change by an excursion that scales with "
                    "the guest period (the residual tee jelly after the blend fix). "
                    "OFF = plain bracketing lerp (for A/B).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ropa_bias, 0, "Skate 3",
                     "Shift the ROPA pose<->shape pairing by N ring poses (+ = fresher "
                     "shape, - = older), a live trim for any residual constant drape "
                     "lag/lead while skating. 0 = the paired generation.")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_ropa_delay, 0, "Skate 3",
                     "Delay CPU-cloth VB snapshots by N guest frames before the worker "
                     "decode, phase-aligning the cloth SHAPE with the motion-smoothing "
                     "play clock the body renders on (~2 periods behind now). Without "
                     "it the drape is ~2 frames AHEAD of the rendered body; it hangs "
                     "where the body WILL be and leads it through direction changes "
                     "(the tee jelly / clip-through-torso). DEFAULT 0: in practice "
                     "the garment LAGGED, and delay made it worse; the "
                     "phase model was backwards. Kept for experiments.")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_native_render_scene_trace_mesh, "", "Skate 3",
                      "Hex guest mesh address to trace end-to-end through the "
                      "texture pipeline ('tex-trace:' log lines): per-frame "
                      "served objects + content fingerprints (on change), "
                      "every slot resolve decision (direct/sticky/hold/near-"
                      "black/white), every payload/words poll verdict, and "
                      "every worker commit touching the mesh's objects. "
                      "Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_detail_hold, 240, "Skate 3",
                     "Frames an item slot keeps serving its previous HIGHER-"
                     "resolution texture after the game's material-detail "
                     "system rebinds a strictly smaller one (streaming "
                     "pressure flaps a nearby mesh's DT material to its UN "
                     "variant for ~0.5 s and back, the visible 'different "
                     "texture set' flash; the detailed decode is still "
                     "cached host-side, so the flap can be invisible). A "
                     "downgrade that persists past the hold is adopted (a "
                     "real demote as you leave the area). 0 = serve the "
                     "guest binding verbatim (the console's own detail pop).")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_retain_offscreen, true, "Skate 3",
                    "Keep recently seen static items in the scene while the game "
                    "view-culls them: the re-timed (smoothed) render camera trails "
                    "the guest pose by up to the filter window, so items leaving "
                    "the guest frustum were visibly torn down right at the screen "
                    "edges during pans/traversal. Items the guest frustum can see "
                    "but stopped submitting (LOD switch, despawn) drop immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_tex_revalidate, true, "Skate 3",
                    "Re-fingerprint cached texture payloads every 16 frames and "
                    "re-decode on change (heals late-composed lightmap pages)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_stretch_guard, true, "Skate 3",
                    "Draw-time stretch veto: skin cached sample verts of each "
                    "skinned mesh's GPU-resident decode with the final palette "
                    "every frame; wider than bind size = the 1-frame mangled-"
                    "ribbon flash: skip the item's draws (blink) and dump the "
                    "palette to logs/stretch_*.txt for diagnosis")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_lm_dump, false, "Skate 3",
                    "Diagnostic: dump the decoded mip 0 of every generated-mip "
                    "(no-chain composed page) texture to native_texture_dumps/ "
                    "as raw RGBA for offline diffing against the gsnap decode")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_backface_cull, true, "Skate 3",
                    "Backface-cull world env materials like the game does: the "
                    "material XMLs set CULLMODE=FRONT on every environment "
                    "family (banners calibrated game-kept faces = our D3D12 "
                    "BACK faces -> CULL_FRONT). CULL_NONE stacked hidden faces "
                    "into the frame: double glass panes + interior wall faces "
                    "behind the translucent canopy glass = the too-bright "
                    "slope / too-dark awning deltas. Trees/alphatest (fams "
                    "7/9/10) and mirrored instances stay uncull(ed).")
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
REXCVAR_DEFINE_DOUBLE(skate3_native_render_scene_bonesig_auto, 0.0, "Skate 3",
                      "Auto-arm bone-signal recordings of this many seconds "
                      "(entity-pose diagnosis, same output as the F12 button): "
                      "first window ~30 s after the native scene comes up, "
                      "re-armed every 90 s, 3 windows max. 0 = off.")
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
// Defined in native/skate3_native_lw.cpp (the LW entity store module).
REXCVAR_DECLARE(bool, skate3_native_render_scene_lw_palette);
// SDK-level emulated-draw suppression (rexglue native_guest_renderer.cpp):
// command processors skip emulated draw/resolve execution while the native
// output is active. Temporarily overridden to false during menu contexts by
// YieldForMenus (see skate3_native_render_scene_menu_unsuppress).
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);
// SDK-level suppression PASS FILTER (d3d12 command_processor.cpp): 0 =
// suppress framebuffer-sized passes only (pitch >= 1280), 2 = suppress all
// except lightmap pages (1024) and small composites (<= 512). YieldForMenus
// drops it to 0 during menu contexts so the skater-portrait RTT passes
// (pitch > 512, in the mode-2 suppressed band) execute; see
// skate3_native_render_scene_menu_rtt_passes.
REXCVAR_DECLARE(int32_t, native_render_suppress_mode);
// SDK-level forced resolve readback window (rexglue command_processor.cpp /
// both backends' IssueCopy): when > 0, resolves up to that byte length are
// synchronously read back to CPU-visible guest memory regardless of
// readback_resolve and gameplay state. Armed by UpdatePhotoGrabWindow while
// a photo flow is active; the game CPU-reads the resolved screenshot
// target to build the photo JPEG.
REXCVAR_DECLARE(int32_t, native_render_force_resolve_readback_max_length);
// SDK-level async pipeline compilation (rexglue command_processor.cpp): the
// d3d12 backend SKIPS draws whose pipeline is still compiling. Forced
// synchronous during menu contexts by YieldForMenus so one-shot portrait
// renders can't lose still-compiling pieces (first-run armless skaters).
REXCVAR_DECLARE(bool, async_shader_compilation);

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
// Steady-clock stamp of the last scene publish (BuildFrameScene only
// publishes when a perspective view submitted this frame). YieldForMenus
// uses its freshness to tell the in-game pause menu (the world keeps
// resubmitting behind the menu) apart from loading screens and the boot
// frontend (publishes stop), for skate3_native_render_scene_pause_native.
std::atomic<int64_t> g_last_publish_ns{-1};
// Debug-dialog cache flushes: consumed at the top of RenderScene so texture/
// mesh-affecting toggles (mip chains, 565 fixes, ...) take effect immediately
// instead of only for newly streamed content.
std::atomic<bool> g_flush_textures{false};
std::atomic<bool> g_flush_meshes{false};
// Off-screen retention clear request (see g_retained_items further down):
// set on the menus/loading flip (render thread) and the F5 re-enable,
// consumed at the top of BuildFrameScene's retention block (guest thread).
std::atomic<bool> g_retained_clear{false};

// Camera/bone signal recorders + synthetic-pan probe + offline recording:
// state and file writers live in native/skate3_native_diagnostics.cpp
// (native/skate3_native_diag.h); capture call sites below read per-frame
// locals and stay here.
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
  // Environment-cube entry (mesh == 0, tex != 0): the one object-keyed
  // texture path left (see EnqueueCubeMiss).
  uint32_t tex = 0;
  // Fetch-words entry (mesh == 0, tex == 0, wkey != 0): a content-store
  // miss decoded from the captured stable words snapshot (see
  // EnqueueWordsMiss): every 2D/3D texture miss and heal.
  uint64_t wkey = 0;
  uint32_t words[6] = {};
  // Environment-cube entry (tex != 0 && cube): a cube-cache miss: a single
  // cube decode measured up to ~100 ms, the largest remaining traversal
  // hitch when a vehicle / reflective area streamed in.
  bool cube = false;
  // Draw-path miss (vs speculative prewarm registration): the result is
  // visible RIGHT NOW, so the commit takes it this frame regardless of the
  // gameplay per-frame commit cap.
  bool miss = false;
  // 2D/HUD overlay miss (large-art async routing, see
  // skate3_native_render_scene_2d_async_px): the commit skips the
  // payload-stability verify for these; APT re-rasterizes animating UI
  // art every guest frame, so "payload moved between decode and commit" is
  // the NORMAL state mid-animation and the verify would reject every
  // commit, freezing the element (spinners, ramping fades). The per-frame
  // content probe in the 2D resolve is the heal path for torn reads, the
  // same exposure the old inline decode had.
  bool ui = false;
};
std::mutex g_prewarm_mutex;
std::condition_variable g_prewarm_cv;  // wakes the decode workers
std::vector<PrewarmEntry> g_prewarm_queue;
// Draw-path misses (EnqueueMeshMiss/EnqueueWordsMiss/EnqueueCubeMiss):
// content that is visible RIGHT NOW (white / skipped),
// served FIFO with strict priority over the speculative registration
// backlog in g_prewarm_queue. Guarded by g_prewarm_mutex.
std::vector<PrewarmEntry> g_miss_queue;
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
// registers many times per load, but a permanent per-address set silently
// DISABLED the prewarm for re-streamed content (the streamer reuses the
// same mesh/texture object addresses all session): after a few minutes
// every re-registered mesh was "seen", its re-decode was skipped, and every
// streamed texture surfaced as a draw-path first-sight miss instead:
// 388 white-flash textures in one 45 s traversal = the
// residual medium-distance pop-in. Now a mesh -> last-queued-frame map:
// clone bursts still dedupe inside the window, re-streams past it re-queue
// (registration is the game's own "content changed here" signal, see
// InvalidateCachedItem above). Cleared when the game enters menus/loading.
std::unordered_map<uint32_t, uint64_t> g_prewarm_seen;
// Texture-object dedupe for the workers (they cannot read the render
// thread's g_r caches): address -> fetch-words key at last staging; a
// words change at a reused address re-stages, an unchanged one skips.
std::unordered_map<uint32_t, uint64_t> g_prewarm_tex_seen;
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
// Sky sun rows from the same draw's PIXEL bank (see FrameScene::sky_sun):
// light dir (c0.xyz), sun angular scale (c4.x), pre-tone multiplier (c4.y),
// exposure (c3.x). Persist like g_sky_height once captured.
float g_sky_sun[6] = {};
bool g_sky_sun_have = false;
// UI background blur (see FrameScene::ui_blur and kBlurShaderSource): while
// a frontend popup is up the game appends blur_hBlur/vBlur + basictex passes
// after the postfx uber. g_ui_blur holds the captured kernel scale (PS c0.x,
// 8 in every capture); g_ui_blur_seen latches per frame on the blur_hBlurPS
// draw and is cleared at publish; the pass chain only exists while the
// popup is actually up, so this can never stick on.
float g_ui_blur = 8.0f;
bool g_ui_blur_seen = false;
// Blur modulate color (the blur passes' PS c1): both blur ucodes end in
// `mul oC0, r0, c1`: the game's menu fade. Gameplay popups stage (1,1,1)
// (why the exact port originally had no multiply); the pause menu stages
// ~(0.35,0.33,0.32), squared across the H+V passes = the darkened pause
// backdrop. Read live on the blur_hBlurPS draw, accepted only when the
// bank's c0 kernel row reads exactly (8,8); a stale mid-stage bank fails
// that gate (the staleness class that pulsed the blur radius when c0.x was
// fed live, see g_ui_blur above).
float g_ui_blur_color[3] = {1.0f, 1.0f, 1.0f};
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
struct CachedBones {
  std::vector<float> bones;
  // g_guest_frame at refresh: rescues/heals must be FRESH; a close-pass
  // refusal resurrecting a seconds-old palette rendered the vehicle 10-20 m
  // BEHIND its live position (the momentary ghost-back).
  uint64_t frame = 0;
};
std::unordered_map<uint32_t, CachedBones> g_bones_cache;
std::atomic<uint64_t> g_bones_rescued{0};
// ctx -> last published palette for skinned character-family items (the
// per-INSTANCE sibling of g_bones_cache): clones share meshes, so the
// mesh-keyed rescue is gated to pub_count==1 and a refused clone capture
// next to a published twin got NOTHING: a one-frame invisibility blink.
// The MeshContext is the game's own per-instance identity,
// so this cache rescues each instance
// with ITS OWN last palette regardless of how many clones are alive.
std::unordered_map<uint32_t, CachedBones> g_bones_cache_ctx;
std::atomic<uint64_t> g_lw_ctx_rescued{0};
// LW entity store consumption counters (stats line lw[...]).
std::atomic<uint64_t> g_lw_stamped{0};
std::atomic<uint64_t> g_lw_fade0{0};
std::atomic<uint64_t> g_lw_gap_filled{0};
// Authoritative caster-palette substitutions + per-ctx lighting-rows serves
// (edge-of-view vehicles; see the stamp pass).
std::atomic<uint64_t> g_lw_pal_sub{0};
std::atomic<uint64_t> g_lw_rows_served{0};
// ctx -> last VALIDATED lighting/paint rows, entity-checked (a recycled
// instance must not inherit the previous occupant's paint). The mesh-keyed
// g_char_rows_cache fallback is gated to single-instance meshes; cloned
// traffic never qualified, so a caster-only stretch (main view culls the
// vehicle at the screen edge before it leaves the screen) dropped to
// legacy flat shading: the "vehicle loses its texture/color at the edge"
// sighting.
struct CharRowsCtx {
  std::array<float, 60> rows;
  uint32_t entity = 0;
};
std::unordered_map<uint32_t, CharRowsCtx> g_char_rows_cache_ctx;
// ctx -> last PUBLISHED item of a live LW entity (skinned character
// families, non-ropa): when a ctx drops out of the submit records for a
// frame or two while its entity is still alive in the store (observed as
// "dyn publish GAP" incidents on fam-3/5 ped meshes, 1-3
// frames each: an in-view NPC vanishing for a frame reads as a blink or,
// moving, a small teleport), the whole item republishes. Console behavior:
// a live entity never skips a frame. Self-limiting: entries expire after 2
// unpublished frames and refresh only from LIVE publishes (never from a
// fill, dbg_src 9), so a real despawn shows at most 2 filled frames, at
// the entity's own served alpha.
struct LwRetained {
  DrawItem item;
  uint64_t frame = 0;
};
std::unordered_map<uint32_t, LwRetained> g_lw_last_items;
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
// mesh -> payload identity + resolved MODE of the ropa decode currently
// RESIDENT on the GPU (written by PrewarmCommit on the render thread when a
// dyn decode job lands; read by BuildFrameScene on the guest thread, hence
// the mutex). The dyn decode workers run 1-2 frames behind the capture, and
// the draw path deliberately tolerates a fingerprint mismatch on dynamic
// payloads (cloth one frame behind the sim). At a sim MODE FLIP that
// tolerance was the residual ribbon: the flip frame publishes the NEW
// mode's interpretation (skinned palette) while the GPU still draws the OLD
// payload's decode (sim-deformed root-local verts) until the worker's
// re-decode commits: skinned-over-sim content for 1-2 frames, the brief
// hard-to-catch stretched-strip flash. The frame-end coherence guard cannot
// see this (ropa[mismatch] stayed 0 in testing): the
// SNAPSHOT it checks is coherent with the new mode; the RESIDENT decode is
// not. BuildFrameScene's flip-hold pass keeps publishing the previous
// resolved state until the resident decode matches the published mode.
struct RopaResidentDecode {
  uint64_t fp = 0;
  bool skinned = false;
};
std::mutex g_ropa_resident_mutex;
std::unordered_map<uint32_t, RopaResidentDecode> g_ropa_resident;
// mesh -> sample verts of the most recent DECODE of a skinned mesh (bind-
// space position + raw blend attribs, ~32 evenly spaced), written by
// DecodeMesh on whichever thread decodes (workers / render), read by the
// guest thread's draw-time STRETCH VETO in BuildFrameScene. The veto skins
// these samples with the FINAL post-interpolation palette, i.e. it judges
// what the GPU will ACTUALLY draw. Every upstream gate (capture acceptance,
// publish coherence) judges the LIVE guest VB instead, so a decode-content
// vs palette pairing mismatch, or junk introduced by the interpolation
// substitutions, passes all of them and still flashes the 1-frame
// map-length ribbon (observed with every ropa[] counter clean).
// `fp` = the decode's payload fingerprint so the veto can log whether the
// drawn content even matches the frame's payload (content lag vs junk
// palette: the decisive diagnostic split).
struct SkinProbeSample {
  float p[3];
  uint32_t bw;
  uint32_t bi;
};
struct SkinProbe {
  std::vector<SkinProbeSample> s;
  uint64_t fp = 0;
};
std::mutex g_skin_probe_mutex;
std::unordered_map<uint32_t, SkinProbe> g_skin_probe;
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
// Large-art async decode routing (see skate3_native_render_scene_2d_async_px):
// quads skipped while their first decode is in flight on the workers, and
// stale decodes served while a content-change re-decode is in flight.
std::atomic<uint64_t> g_2d_async_skip{0};
std::atomic<uint64_t> g_2d_async_stale{0};

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
  // Capture-time stride, preserved across the publish normalization (which
  // rewrites stride to the renderer's 28). The FMV substitution keys on it:
  // the movie quad is the stride-24 textured layout inside the
  // AptMovieIntegration bracket (bit 1).
  uint32_t src_stride = 0;
  uint32_t addr;    // guest inline-ring write pointer
  uint32_t flags;   // bracket bits at capture (layout disambiguation)
  // Texture fetch constants, shadow slots 0-2 (6 dwords each). Slot 0 is
  // the draw's own texture (all the replay paths read only [0..5]); slots
  // 1-2 exist for the VIDEO quads, whose console YUV shader binds the U and
  // V planes there; the replay's YUV-triple detection reads them.
  uint32_t fetch[18];
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

// Offline-analysis recording structs/state: native/skate3_native_diag.h.
std::atomic<uint64_t> g_vs_uploads{0};
std::atomic<uint64_t> g_palette_snapshots{0};
// Palettes captured at base+1 (the cloth/morph VS layout with an extra
// parameter row before the palette, see RefinePaletteBase).
std::atomic<uint64_t> g_palette_base_plus1{0};
// Reflective-glass (fam 5/6) normal-map pair telemetry: pair = draws with
// the masks+normal t4/t5 descriptor pair bound (overlay.w == 4), flat =
// spec-bound reflective draws still on the flat-normal path, gate = the
// last no-pair reason bitmask (1 no spec masks, 2 no normal channel, 4
// masks SRV recipe missing, 8 normal unresolved/white, 16 normal invalid,
// 32 normal recipe missing, 64 pair slots exhausted).
std::atomic<uint64_t> g_refl_pair{0};
std::atomic<uint64_t> g_refl_flat{0};
std::atomic<uint32_t> g_refl_gate{0};

// ---- F7 scene-composition ring (see RequestSceneRingDump in the header) ----
REXCVAR_DEFINE_BOOL(skate3_native_render_scene_ring, true, "Skate 3",
                    "Record a rolling per-frame scene-composition ring (~900 "
                    "frames); F7 dumps it to logs/scene_ring_<ts>.csv for "
                    "diffing 1-2 frame artifacts no capture can catch")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
struct SceneRingItem {
  uint32_t ctx;
  uint32_t mesh;
  uint32_t diffuse;
  uint32_t lightmap;
  uint32_t vb_obj;
  // Full material texture set: a one-frame spec/normal/macro/decal-art
  // resolution glitch changes shading (e.g. an unmasked sky reflection on a
  // reflective bank face) with the diffuse/lightmap identical; the first
  // ring revision could not see those.
  uint32_t spec;
  uint32_t macro;
  uint32_t decal_art;
  uint32_t wnormal;
  uint32_t indices;      // summed index counts of the item's draw entries
  uint16_t drawn;        // draw calls actually issued (0 = skipped at draw)
  uint8_t env_family;
  uint8_t char_family;
  uint8_t flags;  // 1 transparent 2 water 4 skinned 8 retained 16 pending
                  // 32 caster_bank 64 decal
  uint8_t route;  // 0 skipped pre-pass, 1 opaque pass, 2 blended sub-pass
  // SERVED-content fingerprints, stamped in draw_item after the resolves:
  // the object pointers above cannot see an in-place content swap (a heal
  // commit changes what a texture shows with every pointer identical);
  // a fp that changes A->B->A across the flash frame names the texture,
  // the slot, and both contents. 0 = white fallback / not resolved.
  uint64_t fp_diffuse;
  uint64_t fp_lightmap;
  uint64_t fp_macro;
  uint64_t fp_decal;
};
struct SceneRingFrame {
  uint64_t frame = 0;
  float cam[3] = {};
  bool v3_walk = false;
  // Per-frame captured globals: a one-frame glitch in any of these shifts
  // shading on every consumer with the composition identical.
  float fog[6] = {};        // ramp xyz + color rgb
  float family_rows[4] = {};
  float sky_height = 0.0f;
  bool shadow_valid = false;
  std::vector<SceneRingItem> items;
};
std::deque<SceneRingFrame> g_scene_ring;  // render thread only
constexpr size_t kSceneRingFrames = 2400;
std::atomic<bool> g_scene_ring_dump{false};

// Render thread, frame end: write the whole ring as CSV. One F line per
// frame, then one I line per item (hex object addresses to match every
// other diagnostic).
void MaybeDumpSceneRing() {
  if (!g_scene_ring_dump.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  char path[128];
  std::snprintf(path, sizeof(path), "logs/scene_ring_%lld.csv",
                static_cast<long long>(std::time(nullptr)));
  std::ofstream f(path);
  if (!f) {
    REXLOG_WARN("native-scene ring: cannot open {}", path);
    return;
  }
  f << "kind,frame,ctx,mesh,diffuse,lightmap,vb,spec,macro,decal_art,"
       "wnormal,indices,drawn,env_fam,char_fam,flags,route,fp_diffuse,"
       "fp_lightmap,fp_macro,fp_decal\n";
  char line[256];
  for (const SceneRingFrame& fr : g_scene_ring) {
    std::snprintf(line, sizeof(line),
                  "F,%llu,cam,%.2f,%.2f,%.2f,items,%zu,fog,%.5f,%.4f,"
                  "%.3f,%.4f,%.4f,%.4f,fam,%.4f,%.4f,%.4f,%.4f,sky,%.1f,"
                  "shadow,%d\n",
                  static_cast<unsigned long long>(fr.frame), double(fr.cam[0]),
                  double(fr.cam[1]), double(fr.cam[2]), fr.v3_walk ? 1 : 0,
                  fr.items.size(), double(fr.fog[0]), double(fr.fog[1]),
                  double(fr.fog[2]), double(fr.fog[3]), double(fr.fog[4]),
                  double(fr.fog[5]), double(fr.family_rows[0]),
                  double(fr.family_rows[1]), double(fr.family_rows[2]),
                  double(fr.family_rows[3]), double(fr.sky_height),
                  fr.shadow_valid ? 1 : 0);
    f << line;
    for (const SceneRingItem& it : fr.items) {
      std::snprintf(line, sizeof(line),
                    "I,%llu,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,"
                    "%u,%u,%u,%u,%u,%u,%016llX,%016llX,%016llX,%016llX\n",
                    static_cast<unsigned long long>(fr.frame), it.ctx, it.mesh,
                    it.diffuse, it.lightmap, it.vb_obj, it.spec, it.macro,
                    it.decal_art, it.wnormal, it.indices, unsigned(it.drawn),
                    unsigned(it.env_family), unsigned(it.char_family),
                    unsigned(it.flags), unsigned(it.route),
                    static_cast<unsigned long long>(it.fp_diffuse),
                    static_cast<unsigned long long>(it.fp_lightmap),
                    static_cast<unsigned long long>(it.fp_macro),
                    static_cast<unsigned long long>(it.fp_decal));
      f << line;
    }
  }
  REXLOG_INFO("native-scene ring: wrote {} frames -> {}", g_scene_ring.size(),
              path);
}
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
// ropa garments whose resolved mode CHANGED between frames (skinned <->
// rigid: the cloth sim toggling with distance/activity). The flip frames
// are where mode/payload races live; see the dyn-job coherence guard.
std::atomic<uint64_t> g_ropa_flip{0};
// SKINNED-mode ropa payload snapshots whose frame-end VB no longer skins at
// bind size (the cloth sim rewrote it with sim-deformed vertices after the
// draw-time capture): the re-decode is skipped so the GPU keeps last
// frame's coherent verts instead of rendering the mangled ribbon.
std::atomic<uint64_t> g_ropa_mismatch{0};
// ropa captures accepted through the RELAXED near-camera criterion (all
// samples in FRONT of the projection inside a loose 6x guard band + sane
// spread) after the strict half-in-clip score refused them. Up close the
// garment fills/overflows the screen and most of the 6 sample verts clip;
// the strict gate refused the CORRECT palette every frame, and with no
// cached resolved state to rescue from the torso stayed invisible until the
// NPC was far enough away again (log signature: `ropa mesh=... score=2
// flag=(1.000,...)` repeating while stale climbed with rescued flat).
std::atomic<uint64_t> g_ropa_relaxed{0};
// mesh -> newest enqueued cloth-shape generation (DynDecodeJob seq). GUEST
// render thread only: written by the dyn-job enqueue, read by the interp
// ring's pose ingestion (the pose <-> shape pairing).
std::unordered_map<uint32_t, uint64_t> g_ropa_last_seq;
// Shape blends served / skipped (generation missing from the ring) per
// stats window.
std::atomic<uint64_t> g_ropa_blend_drawn{0};
std::atomic<uint64_t> g_ropa_blend_miss{0};
// Frames a ropa garment was HELD on its previous resolved state (or dropped
// when no previous state existed) because the GPU-resident decode still
// pairs with the other mode; see g_ropa_resident.
std::atomic<uint64_t> g_ropa_hold{0};
// Ropa garments PUBLISHED with a caster-cascade (ortho) bank's palette:
// the ~40 ms-stale rows behind the garment-offset-from-body symptom. The
// same-mode graft in the dyn_slot merge should keep this at ~0 whenever a
// perspective capture exists in the same frame.
std::atomic<uint64_t> g_ropa_caster{0};
// Published skinned items whose palette FAILED the dense 32-sample
// publish-time coherence gate (see PublishedPaletteSane): the 1-frame
// junk-palette ribbon flash that passes every 6-sample capture gate. The
// item re-publishes its cached state instead (or drops for one frame).
std::atomic<uint64_t> g_pub_incoherent{0};
// Draw-time STRETCH VETO trips (see g_skin_probe): the final palette skins
// the resident decode's sample verts wider than bind size; the item's
// draws are cleared for the frame (blink, not ribbon) and the first events
// dump full palette + samples to logs/stretch_*.txt for offline diagnosis.
std::atomic<uint64_t> g_stretch_veto{0};
// skinned/ropa meshes that published after a 1-3 frame publish GAP (was on
// screen, vanished, came back): the "flickered invisible" telemetry.
std::atomic<uint64_t> g_dyn_gap{0};
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
PerfWindow g_pw_pal_tail;  // frame tail: palette snapshot-ring rotation
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

// Provenance of the last completed guest draw: (ib_obj << 32 | vb_obj) for
// indexed 3D draws, 0 for everything else. The submit-exit capture may only
// consume the constant bank when the mesh's OWN draw was the last one to
// flush it; `drew_inside` alone proved only that SOME draw ran during the
// submit call. When this mesh's draws were deferred but another entity's
// inline draws ran inside the call, the bank holds that entity's palette,
// and the sample-projection acceptance gate cannot reliably refuse it: a
// vehicle right next to the skater, skinned by the skater's foreign
// palette, still projects on-screen at a plausible spread, and rendered
// glued to his walking bones (the player-becomes-the-vehicle bug).
std::atomic<uint64_t> g_last_draw_ibvb{0};
// drew_inside captures whose bank provably belonged to another mesh's draw
// (now deferred to the post-draw fixup instead of consumed).
std::atomic<uint64_t> g_capture_foreign_bank{0};

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
// structural_guess: the caller PROVED the guess base's layout structurally
// (the ropa flag row read exactly (1,...) from the mesh's own bank; every
// capture site guarantees own-draw provenance now, so the foreign-bank
// hypothesis the strict gate defends against is off the table). When the
// strict half-in-clip score still refuses every home, accept the guess if
// all samples skin IN FRONT of the projection inside a loose 6x guard band
// at a sane spread: up close the garment fills/overflows the screen and
// most sample verts clip the tight 1.5x band; the strict gate refused the
// CORRECT palette every frame and the torso stayed invisible until the NPC
// walked far enough away. A row-shifted palette still fails this (skins
// hundreds of meters off-view or behind the camera).
uint32_t RefinePaletteBase(uint8_t* base, uint32_t bank, uint32_t palette_base,
                           const DrawItem& item, bool structural_guess = false) {
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
  // Sample verts decoded ONCE (native/skate3_native_guest_read.h); only the
  // candidate palette base varies between score() calls.
  SkinSampleVert sverts[kSamples];
  if (!ReadSkinSamplesGuest(base, item, kSamples, sverts)) {
    // Unsupported position format: every candidate is unscorable (-1); keep
    // the caller's guess, as the old per-candidate decode did.
    return palette_base;
  }
  // score: fraction (0..16) of samples that skin in front of and inside the
  // bank's own clip volume; *out_spread = the skinned samples' bbox diagonal
  // (world units) for the bind-size sanity test below. *out_front_all (when
  // asked): every sample is in FRONT of the projection within a loose 6x
  // guard band, the relaxed near-camera criterion (see structural_guess).
  const auto score = [&](uint32_t pb, float* out_spread,
                         bool* out_front_all = nullptr) -> int {
    int ok = 0;
    int loose = 0;
    int n = 0;
    bool rows_sane = true;
    float qmin[3] = {1e9f, 1e9f, 1e9f};
    float qmax[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t s = 0; s < kSamples; ++s) {
      float q[3];
      if (SkinPointBankRows(base, bank, pb, sverts[s], q, &rows_sane) == 0) {
        continue;
      }
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
      if (clip[3] > 0.0f && std::abs(clip[0]) <= 6.0f * aw &&
          std::abs(clip[1]) <= 6.0f * aw) {
        ++loose;
      }
    }
    if (n == 0) {
      return -1;
    }
    if (!rows_sane) {
      return -2;  // provably not a palette (vs -1 = nothing to judge)
    }
    if (out_front_all) {
      *out_front_all = n >= 2 && loose == n;
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
  const float bind_diag = BindDiag(item);
  const float max_spread = std::max(3.0f * bind_diag, bind_diag + 1.0f);
  // Bounded from BELOW too: a palette of non-pose rows can skin every
  // sample to nearly one point that happens to project on-screen (the
  // vehicle-flick garbage). Samples span the whole VB, so legit skinning
  // keeps a large fraction of the bind size; small items (hats: samples
  // can cluster on one bone) skip the floor.
  const float min_spread = bind_diag > 1.0f ? 0.2f * bind_diag : 0.0f;
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
    return ok >= 8 && spread <= max_spread && spread >= min_spread;
  };
  int s_std = -1;
  int s_plus = -1;
  const bool std_pass = gate(palette_base, &s_std);
  if (s_std == -1) {
    // Unsupported position format / no weighted samples: nothing to judge;
    // keep the caller's guess rather than refusing every capture. (-2 =
    // provably-insane rows falls through: try the other homes, else refuse.)
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
  // Fallback homes need their layout's STRUCTURAL signature, not just a
  // passing projection score. The projection gate alone is fooled by
  // ONE-BONE-LATE palettes: a vehicle filling the screen at close range
  // clips half its samples (the correct base FAILS the gate), while a
  // base+3-register home shifts the vehicle BODY; most of its verts hang
  // off the LAST real bone, onto the next leftover rows in the bank,
  // i.e. whatever skinned entity staged before. Standing next to a truck
  // that is the PLAYER: the shifted palette projects beautifully and was
  // accepted, gluing the truck body to the walking skater (the
  // player-becomes-the-vehicle bug; proven in capture:
  // real palette c4..c18, published capture bone k = real bone k+1, body
  // bone 4 = the player's stale c19 rows). Signatures, from the verified
  // layouts:
  //   +1 homes (cloth/morph/ropa): a PARAMETER row directly in front of
  //     the palette: (0,0,0,w) (ropa flag) or (1,0,0,0) (NPC morph).
  //   main-pass homes 7/8: the CAMERA POSITION at c4 (the same key
  //     BankPaletteBase pins the main-pass layout with).
  const auto param_like = [&](uint32_t reg) -> bool {
    float f[4];
    for (int i = 0; i < 4; ++i) {
      f[i] = LoadGuestF32(base, bank + (reg * 4 + uint32_t(i)) * 4);
      if (!(f[i] > -1e7f && f[i] < 1e7f)) {
        return false;
      }
    }
    if (f[0] * f[0] + f[1] * f[1] + f[2] * f[2] <= 0.0025f) {
      return true;  // (0,0,0,w): the ropa flag row
    }
    // (1,0,0,0): the NPC cloth/morph variant's parameter row. A bone row0
    // can also be (1,0,0,tx) for an unrotated bone; its w is the world
    // translation x, so require |w| small too.
    return std::fabs(f[0] - 1.0f) <= 1e-3f && std::fabs(f[1]) <= 1e-3f &&
           std::fabs(f[2]) <= 1e-3f && std::fabs(f[3]) <= 1.5f;
  };
  const auto cam_at_c4 = [&]() -> bool {
    if (g_fog_cam[0] == 0.0f && g_fog_cam[1] == 0.0f && g_fog_cam[2] == 0.0f) {
      return false;
    }
    const float dx = LoadGuestF32(base, bank + 16 * 4) - g_fog_cam[0];
    const float dy = LoadGuestF32(base, bank + 17 * 4) - g_fog_cam[1];
    const float dz = LoadGuestF32(base, bank + 18 * 4) - g_fog_cam[2];
    return dx * dx + dy * dy + dz * dz < 25.0f;
  };
  const auto home_ok = [&](uint32_t pb) -> bool {
    switch (pb) {
      case 4:
        // Never fall back to the pre-pass home on a structurally MAIN-pass
        // bank (camera at c4): palette@4 is then the ONE-BONE-SHIFTED
        // palette: bone 0 = the camera + parameter rows, every other bone
        // = its neighbor's affine. A close vehicle whose real palette@7
        // fails the strict projection gate (screen-filling: samples clip
        // out of the band) fell through here, and the shifted palette
        // skins adjacent bones plausibly enough to PASS, the
        // camera-position-as-bone-0 vehicle mangle (proven in capture:
        // main bank c4 = frame camera, c6.w = -0.5 =
        // the published bone0_t, real palette at c7).
        return !cam_at_c4();
      case 5:
        return param_like(4);
      case 7:
        return cam_at_c4();
      case 8:
        return cam_at_c4() && param_like(7);
      default:
        return false;
    }
  };
  if (home_ok(palette_base + 1) && gate(palette_base + 1, &s_plus)) {
    g_palette_base_plus1.fetch_add(1, std::memory_order_relaxed);
    return palette_base + 1;
  }
  for (const uint32_t pb : {4u, 7u, 5u, 8u}) {
    if (pb == palette_base || pb == palette_base + 1) {
      continue;
    }
    if (home_ok(pb) && gate(pb, nullptr)) {
      return pb;
    }
  }
  // Camera-pinned MAIN-pass home: a close vehicle fills the screen and
  // clips most samples out of the strict band, so gate(7/8) fails even on
  // the REAL palette; with the pre-pass fallback (correctly) blocked
  // above, the capture then refused every frame and the vehicle
  // flickered/vanished during close passes. The camera key at c4 proves
  // the layout structurally; accept on the loose front-of-projection
  // criterion (same relaxation as the ropa structural path below).
  if (cam_at_c4()) {
    const uint32_t pb_main = param_like(7) ? 8u : 7u;
    float spread = 0.0f;
    bool front_all = false;
    const int sc = score(pb_main, &spread, &front_all);
    if (sc >= 0 && front_all && spread <= max_spread && spread >= min_spread) {
      static std::atomic<uint64_t> s_main_relaxed{0};
      const uint64_t n = s_main_relaxed.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 1023u) == 0) {
        REXLOG_INFO(
            "native-scene: main-pass palette relaxed-accept mesh={:08X} "
            "base={} score={} spread={:.2f} bind={:.2f} (n={})",
            item.mesh, pb_main, sc, spread, bind_diag, n);
      }
      return pb_main;
    }
  }
  // Structurally-proven guess (ropa flag row, own-draw bank): near-camera
  // relaxed acceptance; see the function comment. Tried LAST so a strictly
  // passing home always wins first.
  if (structural_guess) {
    float spread = 0.0f;
    bool front_all = false;
    if (score(palette_base, &spread, &front_all) >= 0 && front_all &&
        spread <= max_spread) {
      g_ropa_relaxed.fetch_add(1, std::memory_order_relaxed);
      return palette_base;
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

// Orthographic viewproj at c0..c3 (bottom row exactly (0,0,0,1)): the CSM
// caster-cascade banks, verified in capture (a
// truck's three caster draws vs its perspective main-pass draw). See
// DrawItem::caster_bank.
bool BankIsOrtho(uint8_t* base, uint32_t bank) {
  const float x = LoadGuestF32(base, bank + (3 * 4 + 0) * 4);
  const float y = LoadGuestF32(base, bank + (3 * 4 + 1) * 4);
  const float z = LoadGuestF32(base, bank + (3 * 4 + 2) * 4);
  const float w = LoadGuestF32(base, bank + (3 * 4 + 3) * 4);
  return std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f && std::fabs(z) < 1e-6f &&
         std::fabs(w - 1.0f) < 1e-3f;
}

// AUX perspective viewproj (bank c0..c3, column-vector rows): a perspective
// pass whose projection is NOT screen-shaped; the skater-portrait
// render-to-texture passes (team menu boxes / Import Skater card) render a
// tall narrow card (aspect ~0.4-0.5) while every screen view (main, editor,
// water reflection) is >= 4:3 (16:9 default, wider ultrawide). For a
// combined viewproj the projection scales survive as row norms: the view's
// rotation rows are unit, so ||c0.xyz|| = m00 and ||c1.xyz|| = m11, and
// aspect(w/h) = m11/m00 (verified in capture:
// 1.5/0.843 = 1.78 exactly). Captures from aux passes must never enter the
// palette/world stores: the portrait stage sits ~85 m from the player and a
// portrait-pass capture merged onto the player's meshes produced the mixed
// palettes the stretch veto hid (bones 0-1 at the
// player, rows 2+ at the stage). Ortho banks (CSM caster cascades, square
// aspect) are handled by BankIsOrtho and are NOT aux.
bool BankIsAuxPerspective(uint8_t* base, uint32_t bank) {
  if (BankIsOrtho(base, bank)) {
    return false;
  }
  float n0 = 0.0f;
  float n1 = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float a = LoadGuestF32(base, bank + (0 * 4 + i) * 4);
    const float b = LoadGuestF32(base, bank + (1 * 4 + i) * 4);
    if (!(a > -1e6f && a < 1e6f && b > -1e6f && b < 1e6f)) {
      return false;  // implausible bank: let the existing gates decide
    }
    n0 += a * a;
    n1 += b * b;
  }
  if (!(n0 > 1e-12f && n1 > 1e-12f)) {
    return false;
  }
  // aspect < 1.2 (n1/n0 < 1.44): narrower than any screen view.
  return n1 < n0 * 1.44f;
}

bool GuestReadableApprox(uint8_t* base, uint32_t addr) {
  // The hook layer only walks pointers the game is actively rendering from;
  // they are mapped. Reject null/small.
  (void)base;
  return addr >= 0x10000;
}

// Guarded bulk copy: skate3::native_scene::GuestTryCopy, moved to
// native/skate3_native_guest_read.cpp so every guest reader shares the one
// correctly-built SEH guard. The compiler-trap documentation
// (volatile fn-ptr + noinline: plain __try{memcpy}__except compiles to an
// UNPROTECTED `jmp memcpy`) moved with it.

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
        } else if (std::memcmp(text, "detail", 7) == 0) {
          // Exact match ("detailNormalUVScale" is a different channel).
          // Constant detail texture folded into the fam 5/6 normal
          // composition (see DrawItem::detail_tex).
          slot = &item.detail_tex;
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
            // character.*_ropa = the Ropa cloth-sim VS variant (flag-row
            // switched skinned/rigid, see CaptureSkinnedState). The suffix
            // composes with the base family name: cloth_ropa (player tees),
            // default_cloth_ropa (NPC jackets), hair_ropa and
            // default_hair_ropa all exist in the attrib table; so detect it
            // generically and STRIP it, letting the family idioms below see
            // the base name. Matching only the player's character.cloth_ropa
            // left NPC ropa garments on the generic skinned path, where
            // every palette home is refused: the layout's guess register is
            // this VS's FLAG row (scores 0), and the true +1 home fails the
            // (0,0,0,w)/(1,0,0,0) parameter-row signature because this
            // variant's row is (1, junk, junk, junk), the persistent
            // invisible-torso NPC (observed: a
            // character.default_cloth_ropa mesh refused all three captures every
            // frame while the draw-time banks pass the gate at c5/c8).
            item.ropa = false;
            {
              const size_t len = strnlen(mat_name, sizeof(mat_name));
              if (len >= 5 && len < sizeof(mat_name) &&
                  std::memcmp(mat_name, "character.", 10) == 0 &&
                  std::memcmp(mat_name + len - 5, "_ropa", 5) == 0) {
                item.ropa = true;
                mat_name[len - 5] = '\0';
              }
            }
            item.hair = std::memcmp(mat_name, "character.hair", 15) == 0;
            item.unlit = std::memcmp(mat_name, "sky.", 4) == 0;
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
            } else if (is("environment.reflective_trans", 29)) {
              // transparentenvironmentreflective: the sloped glass canopy /
              // awning panels. Blended in the sorted alpha sub-pass with the
              // fam-5 shading minus kd/macro, premultiplied body, out a^2
              // (model verified 0.0-error against the ucode).
              item.env_family = 13;
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

void RequestSceneRingDump() {
  g_scene_ring_dump.store(true, std::memory_order_release);
}
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

// RecordBoneSignal / RecordCameraSignal: native/skate3_native_diagnostics.cpp.

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
    // The off-screen retention map is equally stale after the gap.
    g_retained_clear.store(true, std::memory_order_relaxed);
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
  const uint64_t now = g_frames_rendered.load(std::memory_order_relaxed);
  const auto [it, fresh] = g_prewarm_seen.try_emplace(mesh, now);
  // 300-frame (~2 s) dedup window: activation bursts re-register the same
  // model once per clone instance: queue it once; a re-registration past
  // the window is a re-stream (content changed at this address) and must
  // re-decode BEFORE its first draw or it pops in as a visible miss.
  if (!fresh) {
    if (now - it->second < 300) {
      return;
    }
    it->second = now;
  }
  if (g_prewarm_queue.size() < 65536) {
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

// ---- Photo-editor postfx capture (FrameScene::PhotoFx / photo_fx.hlsl) ----
// While a photo flow is active (g_photo_flow_frame, armed by
// UpdatePhotoGrabWindow), the SetPending_AluConstants hook snapshots each
// postfx pass's final PS/VS constant rows plus the device fetch-constant
// shadow (device+0x480, the source SetPending_FetchConstants emits to the
// ring) at the moment the game flushes them, the only reliable point:
// draw-entry bank reads return stale values for these postfx draws (the
// same staleness class the blur capture works around with its c0 gate).
enum PfxPass {
  kPfxVisualFx = 0,
  kPfxDofDown,
  kPfxDofMB,
  kPfxDof,
  kPfxUber,
  kPfxFisheye,
  kPfxPassCount
};
struct PfxCapture {
  float ps[32][4];
  float vs[8][4];
  uint32_t fetch[8][6];
  int64_t ps_ns;  // PerfClock stamp of the last PS-row capture (0 = never)
  bool vs_seen;
};
PfxCapture g_pfx_cap[kPfxPassCount] = {};
std::atomic<bool> g_photo_flow_frame{false};
// Defined with the yield helpers below (anonymous namespace); used by the
// BuildFrameScene publish gate (the chain must only run while the editor
// itself is up).
namespace {
const char* PhotoEditorSignal(uint8_t* base);
}  // namespace

// Debug-path classification, cached per shader object (guest render thread
// only, like the blur classifier). -1 = not a photo postfx shader.
int ClassifyPfxShader(uint8_t* base, uint32_t obj) {
  if (obj == 0 || !GuestReadableApprox(base, obj)) {
    return -1;
  }
  static std::unordered_map<uint32_t, int> cache;
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char text[112] = {};
  for (int k = 0; k < 111; ++k) {
    text[k] = char(REX_LOAD_U8(obj + 0x54 + k));
    if (text[k] == '\0') break;
  }
  int pass = -1;
  if (std::strstr(text, "postfx_visualfxPS") != nullptr) {
    pass = kPfxVisualFx;
  } else if (std::strstr(text, "bloom_dof_motionblur_dof_dowsample") != nullptr) {
    // sic: the game's own shader name carries the 'dowsample' typo.
    pass = kPfxDofDown;
  } else if (std::strstr(text, "bloom_dof_tap9dofMotionBlur") != nullptr) {
    pass = kPfxDofMB;
  } else if (std::strstr(text, "bloom_dof_tap9dofPS") != nullptr) {
    pass = kPfxDof;
  } else if (std::strstr(text, "postfx_uberPS") != nullptr) {
    pass = kPfxUber;
  } else if (std::strstr(text, "postfx_basictex_fisheye") != nullptr) {
    pass = kPfxFisheye;
  }
  if (cache.size() < 4096) {
    cache.emplace(obj, pass);
  }
  return pass;
}

void CapturePfxConstants(uint8_t* base, uint32_t bank_ptr, uint32_t device,
                         bool pixel) {
  // Shader labels can be swapped in the OnSetShader hook; classify both.
  int pass = ClassifyPfxShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
  if (pass < 0) {
    pass = ClassifyPfxShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
  }
  if (pass < 0) {
    return;
  }
  PfxCapture& cap = g_pfx_cap[pass];
  // First-hit diagnostics: which passes ever capture, and on which side.
  static uint8_t s_seen[kPfxPassCount][2] = {};
  if (!s_seen[pass][pixel ? 0 : 1]) {
    s_seen[pass][pixel ? 0 : 1] = 1;
    REXLOG_INFO("native-scene: pfx capture first hit pass={} {} bank={:08X}", pass,
                pixel ? "PS" : "VS", bank_ptr);
  }
  if (pixel) {
    for (int r = 0; r < 32; ++r) {
      for (int i = 0; i < 4; ++i) {
        cap.ps[r][i] = LoadGuestF32(base, bank_ptr + uint32_t(r * 4 + i) * 4);
      }
    }
    if (device != 0 && GuestReadableApprox(base, device + 0x480)) {
      for (int s = 0; s < 8; ++s) {
        for (int w = 0; w < 6; ++w) {
          cap.fetch[s][w] = REX_LOAD_U32(device + 0x480 + uint32_t(s * 6 + w) * 4);
        }
      }
    }
    cap.ps_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    PerfClock::now().time_since_epoch())
                    .count();
  } else {
    for (int r = 0; r < 8; ++r) {
      for (int i = 0; i < 4; ++i) {
        cap.vs[r][i] = LoadGuestF32(base, bank_ptr + uint32_t(r * 4 + i) * 4);
      }
    }
    cap.vs_seen = true;
  }
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
    if (g_photo_flow_frame.load(std::memory_order_relaxed)) {
      CapturePfxConstants(base, ptr, device, /*pixel=*/true);
    }
    return;
  }
  if (bank != 0x4000) {
    return;
  }
  g_vs_uploads.fetch_add(1, std::memory_order_relaxed);
  g_vs_bank.store(ptr, std::memory_order_relaxed);
  if (g_photo_flow_frame.load(std::memory_order_relaxed)) {
    CapturePfxConstants(base, ptr, device, /*pixel=*/false);
  }
}

// Last PhotoReplayController::Update heartbeat, nanoseconds on PerfClock
// (steady). -1 until the first heartbeat. Written from the guest thread,
// read by RenderScene on the render thread.
static std::atomic<int64_t> g_photo_replay_last_ns{-1};

void OnPhotoReplayUpdate() {
  g_photo_replay_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}

// Last FrontEndState_Replay2::TakePhoto heartbeat: a photo was just taken
// (replay editor or photo mission). The frames that follow render and
// CPU-grab the 1152x640 screenshot target, so the forced-readback window
// (UpdatePhotoGrabWindow) stays armed for a few seconds after it. Same
// thread contract as the photo-replay heartbeat above.
static std::atomic<int64_t> g_take_photo_last_ns{-1};

void OnTakePhoto() {
  g_take_photo_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}

// Last rw::movie::MovieDecoder::Decode heartbeat (FMV playback: intro
// logos, any full-motion video). Same contract as the photo-replay
// heartbeat above.
static std::atomic<int64_t> g_movie_decode_last_ns{-1};
// Last frame the 2D replay actually SUBSTITUTED a movie quad (ps_yuv2d).
// Consulted by YieldForMovie: while the native substitution is serving the
// video, the emulated yield must never engage; the emulated framebuffer
// is STALE under draw suppression, and presenting it flashed the previous
// video's last frame at every video boundary.
static std::atomic<int64_t> g_movie_native_last_ns{-1};

void OnMovieDecode() {
  g_movie_decode_last_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}

// YUV plane fetch words of the FMV frames handed to
// VideoRenderer_RwTexture::Render (Y full res, U/V half res, CPU-filled via
// Texture::Lock). Multiple videos play at once (the camera-angle select
// page runs TWO preview movies), so this is a small table keyed by the Y
// plane's fetch-words hash; the 2D replay matches each captured draw's own
// slot-0 fetch against it; a video quad samples its Y plane there (the
// camera-page previews rendered as slow GREYSCALE through the plain 2D
// shader: exactly that luma). Movie threads publish under the mutex;
// RenderScene copies per frame.
struct MoviePlanes {
  // Y plane identity = the fetch constant's ADDRESS dword (words[0][1]).
  // NOT a full-words hash: the words staged into the device shadow at draw
  // time carry per-draw sampling flags that differ from the texture
  // object's stored constant (observed: both camera-page videos
  // published, exact-words match still 0 hits): the address dword is the
  // physical identity both sides agree on.
  uint32_t y_addr = 0;
  uint32_t words[3][6];  // Y, U, V fetch constants
  int64_t ns = -1;
};
constexpr int kMaxMovies = 4;
std::mutex g_movie_mutex;
MoviePlanes g_movies[kMaxMovies];

void OnMovieFrame(uint8_t* base, uint32_t renderer) {
  // Plane texture members of VideoRenderer_RwTexture, from the recompiled
  // Render body (Lock/FillTextureData/Unlock trios): [this+12] = Y,
  // [this+124] = U, [this+68] = V; the fill sources are the renderable's
  // y/u/v buffers at +8/+12/+16 (TransferYUVBuffer order).
  static constexpr uint32_t kPlaneOfs[3] = {12, 124, 68};
  MoviePlanes mf;
  for (int p = 0; p < 3; ++p) {
    uint32_t obj = 0;
    if (!GuestTryLoadU32(base, renderer + kPlaneOfs[p], &obj) || obj < 0x10000) {
      return;
    }
    // Stable fetch-words read at tex+0x1C (the D3D12 section's
    // ReadStableTexWords, inlined to keep this compiled guest-side).
    uint32_t raw[6], raw2[6];
    if (!GuestTryCopy(raw, base + obj + 0x1C, sizeof(raw)) ||
        !GuestTryCopy(raw2, base + obj + 0x1C, sizeof(raw2)) ||
        std::memcmp(raw, raw2, sizeof(raw)) != 0) {
      return;
    }
    for (int i = 0; i < 6; ++i) {
      mf.words[p][i] = BSwap32(raw[i]);
    }
    if ((mf.words[p][0] & 3u) != 2 || mf.words[p][1] == 0) {
      return;  // not a live texture fetch constant
    }
  }
  mf.y_addr = mf.words[0][1];
  mf.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PerfClock::now().time_since_epoch())
              .count();
  bool new_key = false;
  {
    std::lock_guard<std::mutex> lock(g_movie_mutex);
    int slot = 0;
    for (int m = 0; m < kMaxMovies; ++m) {
      if (g_movies[m].y_addr == mf.y_addr) {
        slot = m;
        break;
      }
      if (g_movies[m].ns < g_movies[slot].ns) {
        slot = m;  // evict the stalest entry for a new video
      }
    }
    new_key = g_movies[slot].y_addr != mf.y_addr;
    g_movies[slot] = mf;
  }
  OnMovieDecode();  // shared FMV heartbeat (the emulated-yield fallback)
  static std::atomic<uint32_t> s_logged{0};
  if (new_key && s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO(
        "native-scene: FMV planes published (y=[{:08X} {:08X}] u=[{:08X} "
        "{:08X}] v=[{:08X} {:08X}])",
        mf.words[0][0], mf.words[0][1], mf.words[1][0], mf.words[1][1],
        mf.words[2][0], mf.words[2][1]);
  }
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


// Heartbeat of the create-a-skater editor's OWN pixel shaders: the editor's
// in-view skater draws use "_nis" compiles (cacstamp_skin_nisPS,
// cac_cloth_nisPS, cac_face_nisPS, cac_hair_nisPS, defaultcharacter_nisPS,
// cacstamp_shift_nisPS) that exist nowhere else in the game; a fresh
// sighting means the CAS editor is on screen NOW. Backs the FE-stack id-15
// detection in YieldForCasEditor: the startup new-game flow reaches the
// editor through a different FE screen (observed: the indicator stayed
// NATIVE there). Deliberately NOT matched: the "_unwrap" texture-space
// composite variants; outfit composition also runs mid-gameplay after an
// outfit change and must never yield a gameplay frame.
std::atomic<int64_t> g_cas_ps_last_ns{-1};

// Guest render thread only: per-object cached debug-path classifier
// (same pattern as ClassifySplineShader).
bool IsCasEditorPs(uint8_t* base, uint32_t obj) {
  if (obj < 0x10000) {
    return false;
  }
  static std::unordered_map<uint32_t, bool> cache;
  auto it = cache.find(obj);
  if (it != cache.end()) {
    return it->second;
  }
  char text[97] = {};
  if (!GuestTryCopy(text, base + obj + 0x54, 96)) {
    return false;  // not cached: unreadable now may be readable later
  }
  text[96] = '\0';
  const char* leaf = std::strrchr(text, '\\');
  leaf = leaf ? leaf + 1 : text;
  const bool cas =
      std::strstr(leaf, "_nis") != nullptr &&
      (std::strncmp(leaf, "cacstamp_", 9) == 0 || std::strncmp(leaf, "cac_", 4) == 0 ||
       std::strncmp(leaf, "defaultcharacter_", 17) == 0);
  if (cache.size() > 4096) {
    cache.clear();
  }
  cache.emplace(obj, cas);
  return cas;
}

void OnSetShader(bool pixel, uint32_t obj) {
  (pixel ? g_cur_ps_obj : g_cur_vs_obj).store(obj, std::memory_order_relaxed);
  // Classify BOTH labels: the hook's pixel/vertex flags are SWAPPED
  // relative to the real shader types (see ClassifySplineShader, which
  // checks both trackers for the same reason); gating on `pixel` alone
  // left the heartbeat dead (the *_nisPS debug paths arrive on the other
  // label), which blackholed the startup-flow editor.
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base != nullptr && IsCasEditorPs(base, obj)) {
    g_cas_ps_last_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           std::memory_order_relaxed);
  }
}

// The CAS editor is ON SCREEN now: its own "_nis" pixel shaders were set
// within the last 0.5 s (see IsCasEditorPs). Consumed by the portrait-pass
// publish gate and the takeover gates; the STARTUP-flow editor's scene is
// small (below warmup_min_items) and can look like a portrait pass, so the
// gates need to know the editor is up.
bool CasEditorHeartbeatFresh() {
  const int64_t last_ns = g_cas_ps_last_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  return now_ns - last_ns < 500'000'000;
}

// Full CAS-editor detection: FE push-state screen id 15 (present in BOTH
// flows, pause: [0,56,63,15], startup new-game: [0,67,15], verified in
// capture) OR the _nis shader heartbeat as backup. Cheap
// (a handful of guarded u32 reads); stateless per frame.
bool CasEditorActive(uint8_t* base) {
  if (CasEditorHeartbeatFresh()) {
    return true;
  }
  constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
  uint32_t mgr = 0, beg = 0, end = 0;
  if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
      GuestTryLoadU32(base, mgr + 0x210, &beg) &&
      GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
      end - beg <= 20 * 16) {
    const uint32_t n = (end - beg) / 20;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t f0 = 0;
      if (GuestTryLoadU32(base, beg + i * 20, &f0) && f0 == 15) {
        return true;
      }
    }
  }
  return false;
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
//     (same literal as livingworld), exposure c13.z, alpha c20.x (the
//     per-entity spawn/distance fade), phong spec color c16
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
  // `plus` = register-row shift for the create-a-skater EDITOR compiles of
  // the fam-2 shaders (cacstamp_/cac_*_nisPS + the ropa variants): the whole
  // cacstamp layout sits ONE ROW HIGHER there: light c10, key c16, exposure
  // c14.z, ambMult c20.w, SH c25..c33 scaled by c22.y, alpha c23.x, tint c24
  // (ucode-proven from cac_cloth_nisPS, offline-validated on every fam-2
  // draw in capture: skin tint (0.80,0.68,0.64),
  // key (1.0,0.95,0.9), expo 1.5, unit light at c10). The editor's VS/ropa
  // palette layout is UNCHANGED (flag c7, palette c8, rigid c191 - draw-41
  // VS disasm), and the editor hair compile keeps the gameplay fam-4 rows,
  // so lighting rows are the only editor delta.
  const auto rows_valid = [&](uint8_t f, uint32_t plus, float* light, float* expo,
                              float* key) {
    uint32_t light_r = 9, key_r = 15, expo_r = 13, expo_c = 2;
    switch (f) {
      case 1: light_r = 0; key_r = 6; expo_r = 4; expo_c = 2; break;
      case 2: case 3: case 6: case 7: break;  // defaults above
      case 4: light_r = 4; key_r = 16; expo_r = 8; expo_c = 2; break;
      case 5: light_r = 4; key_r = 15; expo_r = 8; expo_c = 2; break;
    }
    light_r += plus;
    key_r += plus;
    expo_r += plus;
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
  uint32_t plus = 0;
  if (!rows_valid(fam, 0, light, &expo, key)) {
    // NPC skin (character_skin_defaultPS) shares the "character.skin"
    // attribulator name with the CAC player skin (cacstamp_skin) but uses
    // the DEFAULTCHARACTER register layout; the two banks are mutually
    // exclusive on the light-row position (the other layout's slot holds
    // non-unit data), so a failed fam-2 read retries as fam 1, then as the
    // EDITOR fam-2 layout (+1 row, see `plus` above); the create-a-skater
    // screen's _nis compiles, where both standard maps read junk.
    if (fam == 2 && rows_valid(1, 0, light, &expo, key)) {
      fam = 1;
    } else if (fam == 2 && rows_valid(2, 1, light, &expo, key)) {
      plus = 1;
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
    const uint32_t sh_base = (fam == 1 ? 14u : 24u) + plus;
    const float s = row((fam == 1 ? 12u : 21u) + plus, 1);
    const float s2 = s * s;
    const float amb_mult = row((fam == 1 ? 10u : 19u) + plus, 3);
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
      // CAC diffuse/skin tint (c23; editor c24), multiplies the squared
      // diffuse.
      bool tint_ok = true;
      float tint[3];
      for (int c = 0; c < 3; ++c) {
        tint[c] = row(23u + plus, uint32_t(c));
        tint_ok = tint_ok && tint[c] >= 0.0f && tint[c] < 16.0f;
      }
      if (tint_ok) {
        d[12 * 4 + 0] = tint[0];
        d[12 * 4 + 1] = tint[1];
        d[12 * 4 + 2] = tint[2];
        d[12 * 4 + 3] = 1.0f;
      }
    }
    d[14 * 4 + 0] = std::clamp(row((fam == 1 ? 13u : 22u) + plus, 0), 0.0f, 1.0f);
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
    d[14 * 4 + 0] = std::clamp(row(21u, 0), 0.0f, 1.0f);
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
      // Entity opacity: vehicle_defaultPS ends `max oC0.w, c20.x, c20.x`,
      // the same per-entity constant the glass variant multiplies by its
      // material alpha. The LivingWorld spawn/distance fade rides here;
      // 1.0 once the vehicle is fully faded in.
      d[14 * 4 + 0] = std::clamp(row(20u, 0), 0.0f, 1.0f);
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

// The game's per-entity spawn/streaming fade. LivingWorld presentation
// entities (NPCs, traffic vehicles, spawned props) publish an opacity:
// 0 through the whole spawn settle (the physics drop after
// CensusMan::SpawnPedestrian/SpawnVehicle), ramping up afterwards and by
// distance (cLivingWorldPresEntity::EvaluateOpacityDistance), which every
// character-family PS writes as its output alpha (the "alpha out" row of
// CaptureCharLighting: peds c21.x, defaultcharacter c13.x, cacstamp c22.x,
// vehicle body c20.x, glass c20.x * c18.w, hair strand * scale). Returns
// that alpha for items whose validated capture carries it, 1.0 otherwise
// (legacy-shaded items keep the old always-opaque behavior).
float CharFadeAlpha(const DrawItem& item) {
  // LW-mapped items: the entity's own
  // opacity is authoritative; it is the exact value the game serves this
  // ctx's shader as output alpha, independent of whether the per-draw row
  // capture validated (or captured a clone's foreign row). Families whose
  // shader COMPOSES the entity fade with another factor (hair strand-scale,
  // vehicle-glass tint alpha) keep their captured value bounded by it.
  if (item.lw_alpha >= 0.0f) {
    const float a = std::clamp(item.lw_alpha, 0.0f, 1.0f);
    switch (item.char_family) {
      case 1:
      case 2:
      case 3:
      case 6:
        return a;
      case 4:
      case 5:
        return item.char_rows[14 * 4 + 1] > 0.0f
                   ? std::min(a, std::clamp(item.char_rows[13 * 4 + 3], 0.0f, 1.0f))
                   : a;
      case 7:
        return item.char_rows[14 * 4 + 1] > 0.0f
                   ? std::min(a, std::clamp(item.char_rows[14 * 4 + 0], 0.0f, 1.0f))
                   : a;
      default:
        break;
    }
  }
  if (item.char_rows[14 * 4 + 1] <= 0.0f) {
    return 1.0f;
  }
  switch (item.char_family) {
    case 1:
    case 2:
    case 3:
    case 6:
    case 7:
      return std::clamp(item.char_rows[14 * 4 + 0], 0.0f, 1.0f);
    case 4:
    case 5:
      // Hair alpha = strand coverage * scale; the scale constant carries the
      // entity fade, so a near-zero scale means the whole garment is
      // invisible this frame.
      return std::clamp(item.char_rows[13 * 4 + 3], 0.0f, 1.0f);
    default:
      return 1.0f;
  }
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
// *out_front_all (when asked): every sample lands IN FRONT of the
// projection within a loose 6x guard band, the relaxed near-camera
// criterion (a sim-active garment right at the camera clips most of its
// samples out of the tight 1.5x band, and the strict >=8 gate refused the
// CORRECT rigid matrix; same failure mode as the skinned branch).
int ScoreRigidAffine(uint8_t* base, uint32_t bank, uint32_t m, const DrawItem& item,
                     bool* out_front_all = nullptr) {
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
  SkinSampleVert sverts[kSamples];
  if (!ReadSkinSamplesGuest(base, item, kSamples, sverts)) {
    return -1;
  }
  int ok = 0;
  int loose = 0;
  int n = 0;
  for (uint32_t s = 0; s < kSamples; ++s) {
    const float* p = sverts[s].p;
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
    if (clip[3] > 0.0f && std::abs(clip[0]) <= 6.0f * aw &&
        std::abs(clip[1]) <= 6.0f * aw) {
      ++loose;
    }
  }
  if (out_front_all) {
    *out_front_all = n >= 2 && loose == n;
  }
  return n == 0 ? -1 : (ok * 16) / n;
}

// Frame-end coherence check for SKINNED-mode ropa payloads (dyn decode
// jobs). The item's mode and palette were captured at draw time, but the
// cloth VB is snapshotted at BuildFrameScene, and the game's cloth sim
// runs concurrently: when it ACTIVATES between the draws and the snapshot
// (skating NPCs toggle with distance, "when he got near"), the copied
// buffer holds sim-deformed root-local vertices, i.e. the ropa VS's OTHER
// branch's input. Skinning those with the palette is the map-length-ribbon
// interpretation (the offline-validated 0/31). Skin the acceptance gate's
// sample vertices from the JUST-COPIED payload and require the same
// bind-size spread the capture gate does; on failure the caller keeps last
// frame's decode (coherent) and retries next frame, when the capture will
// have flipped the mode to match the payload.
bool RopaPayloadCoherent(const DrawItem& item, const std::vector<uint8_t>& vb) {
  if (item.stride == 0 || item.bones.size() < 12 || item.bw_offset == 0 ||
      item.bi_offset == 0) {
    return true;
  }
  const float bind_diag = BindDiag(item);
  const float max_spread = std::max(3.0f * bind_diag, bind_diag + 1.0f);
  constexpr uint32_t kSamples = 6;
  SkinSampleVert sverts[kSamples];
  const int got = ReadSkinSamplesRaw(vb.data(), vb.size(), item, kSamples, sverts);
  if (got < 0) {
    return true;  // unsupported position format: nothing to judge
  }
  if (got != int(kSamples)) {
    // Short copy: garbage in the decoded prefix still fails (the original
    // checked each sample before its successor's bounds check); otherwise
    // there is nothing to judge.
    for (int s = 0; s < got; ++s) {
      if (!sverts[s].pos_finite) {
        return false;
      }
    }
    return true;
  }
  float spread = 0.0f;
  const int r = SkinnedSpreadHostRows(sverts, kSamples, item.bones.data(),
                                      item.bones.size(), /*min_n=*/2,
                                      /*garbage_fails=*/true, &spread);
  if (r < 0) {
    return false;  // NaN/garbage positions mid-sim-write
  }
  if (r == 0) {
    return true;  // nothing to judge
  }
  return spread <= max_spread;
}

// Dense publish-time coherence gate: skin ~32 samples of the LIVE guest VB
// with the item's PUBLISHED palette and require bind-pose spread. The
// capture acceptance gates sample only 6 verts, so a palette whose junk
// rows sit on UNSAMPLED bones (e.g. a staging bank partially overwritten
// by the next entity's rows between this mesh's draw and our capture)
// passes them and stretches the unsampled islands into the map-length-
// ribbon flash, with zero refusals, zero mismatches, zero gaps in the
// telemetry (ropa[stale=0 mismatch=0 hold=0
// caster=0] yet a momentary stretch on screen). 32 evenly-spaced samples
// cover the islands the 6-vert gates miss. Returns the measured spread via
// out_spread for the diagnosis log.
bool PublishedPaletteSane(uint8_t* base, const DrawItem& item,
                          float* out_spread) {
  *out_spread = 0.0f;
  if (item.stride == 0 || item.bones.size() < 12 || item.bw_offset == 0 ||
      item.bi_offset == 0 || item.vb_addr == 0) {
    return true;
  }
  const float bind_diag = BindDiag(item);
  // 6x, NOT the capture gates' 3x: this judges already-ACCEPTED palettes,
  // where the observed junk measures 183-405 while legit articulation on
  // small meshes (a 0.5 m accessory mid-stride) reached 3.1-3.3x and
  // tripped a 3x bound repeatedly (log 1285 mesh=436E7210, spread 1.6 vs
  // 1.56 - real world-space palette, needlessly healed/frozen).
  const float max_spread = std::max(6.0f * bind_diag, bind_diag + 2.0f);
  constexpr uint32_t kSamples = 32;
  SkinSampleVert sverts[kSamples];
  if (!ReadSkinSamplesGuest(base, item, kSamples, sverts)) {
    return true;  // unsupported position format: nothing to judge
  }
  float spread = 0.0f;
  if (SkinnedSpreadHostRows(sverts, kSamples, item.bones.data(), item.bones.size(),
                            /*min_n=*/4, /*garbage_fails=*/false, &spread) != 1) {
    return true;  // nothing to judge
  }
  *out_spread = spread;
  return spread <= max_spread;
}

// Returns false when the bank could not be consumed for this item (ropa
// rigid matrix implausible or off-clip = stale bank); the caller must
// leave/mark the item pending so a later matching draw re-captures it.
bool CaptureSkinnedState(uint8_t* base, uint32_t bank, uint32_t palette_base,
                         DrawItem& item) {
  // Refuse captures staged by AUX perspective passes (skater-portrait RTTs):
  // the portrait pass draws the SAME meshes as the on-screen player at the
  // off-map portrait stage, and accepting its palette poisons the mesh/ctx
  // stores (see BankIsAuxPerspective). The item stays pending; a later
  // main-view draw of these buffers resolves it, and portrait-only frames
  // publish nothing (BuildFrameScene's aux-view gate).
  if (BankIsAuxPerspective(base, bank)) {
    static std::atomic<uint64_t> s_aux_refused{0};
    const uint64_t n = s_aux_refused.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 || (n & 4095u) == 0) {
      REXLOG_INFO(
          "native-scene: skinned capture refused - aux perspective pass "
          "(portrait RTT) mesh={:08X} (n={})",
          item.mesh, n);
    }
    return false;
  }
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
      // ropa state rescue). The game writes exactly 1.0 into the flag row's
      // x for sim-inactive (observed on every live capture, incl. the
      // (1,junk,junk,junk) NPC variant); a bit-exact 1.0 is the structural
      // proof that unlocks the relaxed near-camera acceptance; any other
      // positive x (a bone row of some other layout) keeps the strict gate.
      palette_base = RefinePaletteBase(base, bank, main_pass ? 8u : 5u, item,
                                       /*structural_guess=*/flag_x == 1.0f);
      if (palette_base == 0) {
        g_ropa_stale.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    } else {
      // The garment's rigid world is the accepted draw-time bank matrix
      // (c188/c191): the game stages it tick-exact with the deformed VB
      // content and with the body palettes packed at EndJobs. (A guest-side
      // entity L2W read at StartJobs predates the tick's locomotion update;
      // serving that as the draw world rendered the whole shirt one guest
      // tick behind the body, a constant velocity-proportional drape lag.)
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
      bool front_all = false;
      const int rigid_score =
          plausible ? ScoreRigidAffine(base, bank, m, item, &front_all) : -1;
      // Relaxed near-camera acceptance mirrors the skinned branch: a
      // sim-active garment right at the camera clips most samples out of
      // the strict band, but a stale/foreign matrix throws them behind the
      // projection or far outside even the loose band.
      if (plausible && (rigid_score >= 8 || front_all)) {
        if (rigid_score < 8) {
          g_ropa_relaxed.fetch_add(1, std::memory_order_relaxed);
        }
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
        if (item.hair) {
          CaptureHairTint(base, item);
        }
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
  // Items drawn inside an AUX perspective pass (skater-portrait RTTs) never
  // enter the frame at all: they share (ib,vb) buffers with the on-screen
  // player, so letting them sit PENDING lets them steal the player's own
  // post-draw fixups (FIFO oldest-pending) and publish a ghost at the
  // player's pose. drew_inside guarantees the bank is the pass's own, so
  // its c0..c3 viewproj identifies the pass (see BankIsAuxPerspective).
  if (drew_inside && BankIsAuxPerspective(base, bank)) {
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
  item.ctx = ctx;  // identity key for the palette serve / entity store
  // The bank only provably holds THIS mesh's constants when the last draw
  // that flushed it bound this mesh's buffers (see g_last_draw_ibvb);
  // `drew_inside` alone also accepted banks left by another entity's inline
  // draws while this mesh's own draws were deferred (the walking-vehicle /
  // origin-vending-machine captures). Deferring those to the post-draw
  // (ib,vb) fixup pairs the state with the mesh's own real draw.
  const bool own_draw_last =
      drew_inside && g_last_draw_ibvb.load(std::memory_order_relaxed) ==
                         ((uint64_t(item.ib_obj) << 32) | item.vb_obj);
  if (drew_inside && !own_draw_last) {
    g_capture_foreign_bank.fetch_add(1, std::memory_order_relaxed);
  }
  // Rigid transform: deferred (multi-pass) rigid props draw later; the
  // bank belongs to some earlier mesh, and a leftover identity matrix at c4
  // VALIDATES as a plausible world (verified from recorded draw streams:
  // 4 of 6 vending-machine clones captured exact identity and rendered
  // invisibly at the origin). Defer those to the post-draw fixup, like
  // skinned palettes.
  if (!item.skinned) {
    // Aux perspective passes (portrait RTTs) stage worlds at the off-map
    // portrait stage; defer like a foreign bank (see BankIsAuxPerspective).
    if (!own_draw_last || BankIsAuxPerspective(base, bank) ||
        !BankRigidWorld(base, bank, item.world)) {
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
    // to the post-draw fixup. Same when the last draw inside the submit
    // call was not this mesh's own (deferred mesh, foreign inline draws):
    // a stale/foreign bank can still hold plausible bone rows.
    item.pending = world_path || !own_draw_last || (passes > 1 && passes < 16) ||
                   palette_base == 0;
    if (!item.pending) {
      if (CaptureSkinnedState(base, bank, palette_base, item)) {
        g_palette_snapshots.fetch_add(1, std::memory_order_relaxed);
        item.dbg_src = 1;
        item.caster_bank = BankIsOrtho(base, bank);
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
  if (g_frame_dynitems[index].pending ||
      (g_frame_dynitems[index].skinned && g_frame_dynitems[index].caster_bank &&
       !g_frame_dynitems[index].ropa)) {
    // Pending items wait for their first fixup; caster-bank captures stay
    // registered so the mesh's later main-pass draw REFRESHES the palette
    // (see DrawItem::caster_bank, stale wheel spin in the shadow banks).
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
  // Last-draw provenance for the submit-exit capture (see g_last_draw_ibvb):
  // only an indexed 3D draw leaves a bank the palette/world capture may
  // trust, keyed by the buffers it bound.
  g_last_draw_ibvb.store(
      (func == 0 && flags2d == 0)
          ? ((uint64_t(g_cur_ib.load(std::memory_order_relaxed)) << 32) |
             g_cur_vb.load(std::memory_order_relaxed))
          : 0,
      std::memory_order_relaxed);
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
      // Same draw, PIXEL bank (sky_defaultPS layout, verified in
      // capture): c0.xyz = g_vLightDir (unit vector
      // toward the sun), c4.x = sun angular scale (m_params[0].x, 0.75),
      // c4.y = sky pre-tone multiplier (m_params[0].y, 0.35), c3.x = scene
      // exposure (g_envattributes[2].x, 2.5). Sanity-gated: a stale bank
      // here would put the sun glow in a wrong spot or blow out the tone.
      const uint32_t sky_ps = g_ps_bank.load(std::memory_order_relaxed);
      if (sky_ps != 0) {
        float dir[3];
        for (int k = 0; k < 3; ++k) {
          dir[k] = LoadGuestF32(base, sky_ps + uint32_t(k) * 4);
        }
        const float n2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
        const float scale = LoadGuestF32(base, sky_ps + (4 * 4 + 0) * 4);
        const float mult = LoadGuestF32(base, sky_ps + (4 * 4 + 1) * 4);
        const float expo = LoadGuestF32(base, sky_ps + (3 * 4 + 0) * 4);
        if (n2 > 0.8f && n2 < 1.2f && scale > 1e-3f && scale < 100.0f &&
            mult > 1e-3f && mult < 100.0f && expo > 0.01f && expo < 100.0f) {
          g_sky_sun[0] = dir[0];
          g_sky_sun[1] = dir[1];
          g_sky_sun[2] = dir[2];
          g_sky_sun[3] = scale;
          g_sky_sun[4] = mult;
          g_sky_sun[5] = expo;
          g_sky_sun_have = true;
        }
      }
    }
    if (dx * dx + dy * dy + dz * dz < 25.0f) {
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
      // Fog rows (VS c5 ramp / c6 color): POSITIVE family gate like the
      // receiver rows below, for the same reason: the camera-keyed c4 check
      // alone lets ANY main-pass-layout draw win the first-draw race, and
      // the dam spillway's water shaders keep the camera at c4 with a
      // water-teal where fog c6 lives, values that PASSED the range gate
      // and tinted every distance-fogged surface (the bank mist band, the
      // far dirt hill) saturated blue for exactly the one frame that
      // capture served (the approach-flicker blue flash; F7 scene-ring
      // proved composition/textures identical across the artifact frame).
      // Same failure class as the flowingwater tone hijack documented at
      // env_receiver_ps.
      if (!g_fog_frame_done && env_receiver_ps()) {
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        // Range gate (kept as a second line of defense): ramp scale is a
        // tiny per-meter slope, the exponent is a small power, the fog color
        // is a dim linear-space rgb and the transmittance scale small.
        const bool sane = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                          rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                          rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                          rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (sane) {
          std::memcpy(g_fog_rows, rows, sizeof(rows));
          g_fog_have = true;
          g_fog_frame_done = true;
        }
      } else if (!g_fog_frame_done) {
        // Confirmation probe for the blue-flash fix: a NON-env draw whose
        // rows would have passed the old value-only gate with a fog color
        // far from the current one is exactly the frame that used to flash
        // - each hit here is one prevented flash, naming the hijacker.
        float rows[8];
        for (int i = 0; i < 8; ++i) {
          rows[i] = LoadGuestF32(base, bank + (20 + i) * 4);
        }
        const bool would = rows[0] >= 0.0f && rows[0] < 0.1f && std::fabs(rows[1]) < 16.0f &&
                           rows[2] > 0.0f && rows[2] <= 8.0f && rows[4] >= 0.0f &&
                           rows[4] <= 4.0f && rows[5] >= 0.0f && rows[5] <= 4.0f &&
                           rows[6] >= 0.0f && rows[6] <= 4.0f && std::fabs(rows[7]) <= 1.0f;
        if (would) {
          const float dr = rows[4] - g_fog_rows[4];
          const float dg = rows[5] - g_fog_rows[5];
          const float db = rows[6] - g_fog_rows[6];
          if (dr * dr + dg * dg + db * db > 0.01f) {
            // Rolling cap (the flat 32 burned at the load screen).
            static std::atomic<uint32_t> s_fog_rejects{0};
            static std::atomic<int64_t> s_fog_win{0};
            const int64_t now_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            int64_t win = s_fog_win.load(std::memory_order_relaxed);
            if (now_s - win >= 5 &&
                s_fog_win.compare_exchange_strong(win, now_s)) {
              s_fog_rejects.store(0, std::memory_order_relaxed);
            }
            if (s_fog_rejects.fetch_add(1, std::memory_order_relaxed) < 8) {
              REXLOG_INFO(
                  "native-scene: fog capture REJECTED by family gate "
                  "(prevented flash): vs_obj={:08X} ps_obj={:08X} "
                  "color=({:.3f},{:.3f},{:.3f}) vs current ({:.3f},{:.3f},{:.3f})",
                  g_cur_vs_obj.load(std::memory_order_relaxed),
                  g_cur_ps_obj.load(std::memory_order_relaxed), rows[4], rows[5],
                  rows[6], g_fog_rows[4], g_fog_rows[5], g_fog_rows[6]);
            }
          }
        }
      }
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
  // Photo-editor postfx capture FALLBACK at the draw hook: the SetPending
  // hook is the preferred site, but if the postfx constants never flush
  // through it (or classification misses there), capture here from the
  // last-known bank pointers; a draw capture proved the
  // visualfx PS rows read correctly from g_ps_bank at this point.
  if (flags2d == 0 && SceneEnabled() &&
      g_photo_flow_frame.load(std::memory_order_relaxed)) {
    int pfx_pass = ClassifyPfxShader(base, g_cur_ps_obj.load(std::memory_order_relaxed));
    if (pfx_pass < 0) {
      pfx_pass = ClassifyPfxShader(base, g_cur_vs_obj.load(std::memory_order_relaxed));
    }
    if (pfx_pass >= 0) {
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      const uint32_t vs_bank = g_vs_bank.load(std::memory_order_relaxed);
      const uint32_t dev = g_device.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        CapturePfxConstants(base, ps_bank, dev, /*pixel=*/true);
      }
      if (vs_bank != 0) {
        CapturePfxConstants(base, vs_bank, dev, /*pixel=*/false);
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
      // Fade modulate (PS c1, see g_ui_blur_color): genuinely dynamic (the
      // pause fade ramps in over a few frames), so it must be read live,
      // gated on the c0 kernel row reading exactly its known (8,8) staging,
      // which a stale mid-update bank fails. NaN also fails the comparisons.
      const uint32_t ps_bank = g_ps_bank.load(std::memory_order_relaxed);
      if (ps_bank != 0) {
        const float k0 = LoadGuestF32(base, ps_bank + 0);
        const float k1 = LoadGuestF32(base, ps_bank + 4);
        float c1[3];
        for (int a = 0; a < 3; ++a) {
          c1[a] = LoadGuestF32(base, ps_bank + 16 + a * 4);
        }
        if (k0 == 8.0f && k1 == 8.0f && c1[0] >= 0.0f && c1[0] <= 1.0f &&
            c1[1] >= 0.0f && c1[1] <= 1.0f && c1[2] >= 0.0f && c1[2] <= 1.0f) {
          std::memcpy(g_ui_blur_color, c1, sizeof(g_ui_blur_color));
        }
      }
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
  // (An "unbracketed capture" for out-of-bracket boot draws lived here
  // briefly, removed: every real boot/menu UI draw is bracketed, including
  // the intro video quad, and it demonstrably ingested blur/postfx pass
  // quads during takeover-hold windows.)
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
        // Slots 0-2 (18 dwords): slot 0 is the draw's texture; slots 1-2
        // matter only for video quads, whose YUV shader binds the U and V
        // planes there (the self-contained YUV-triple detection at replay).
        for (int i = 0; i < 18; ++i) {
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
  // This draw's constants belong to the OLDEST still-PENDING item with
  // these buffers (clones share mesh assets; the deferred list draws in
  // submit order, so FIFO one-shot pairing keeps clones' palettes apart).
  // With none pending, the oldest CASTER-sourced item instead gets a
  // REFRESH from this later draw: palettes captured from the ortho
  // caster-cascade banks carry stale fine animation (vehicle wheel spin,
  // ~40 ms behind in bursts); publishing them made the car's pose stream
  // jump, tripping the smoothing ring's discontinuity reset (the traffic
  // judder). The refresh entry is only retired by a perspective-bank
  // (z/main-pass) capture.
  auto oldest = range.second;
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second >= g_frame_dynitems.size() ||
        !g_frame_dynitems[it->second].pending) {
      continue;
    }
    if (oldest == range.second || it->second < oldest->second) {
      oldest = it;
    }
  }
  bool caster_refresh = false;
  if (oldest == range.second) {
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second >= g_frame_dynitems.size()) {
        continue;
      }
      const DrawItem& c = g_frame_dynitems[it->second];
      if (!c.caster_bank || !c.skinned || c.ropa) {
        continue;
      }
      if (oldest == range.second || it->second < oldest->second) {
        oldest = it;
      }
    }
    if (oldest == range.second) {
      return;
    }
    caster_refresh = true;
  }
  DrawItem& d = g_frame_dynitems[oldest->second];
  if (caster_refresh) {
    // Refresh of a caster-sourced capture from a later (ideally main-pass)
    // draw. Re-capture into a PROBE and require the fresher palette to be
    // THIS entity's pose: same-mesh clones share these buffers, and a far
    // twin's later draw otherwise refreshes the near car onto the twin's
    // position (bone-signal recorded 151 m palette teleports; the near
    // car renders across the map, i.e. invisible where it stands).
    const uint32_t palette_base = BankPaletteBase(base, bank);
    if (palette_base == 0) {
      return;
    }
    DrawItem probe = d;
    if (!CaptureSkinnedState(base, bank, palette_base, probe)) {
      return;
    }
    if (probe.bones.size() < 12 || probe.bones.size() != d.bones.size()) {
      return;
    }
    const float dx = probe.bones[3] - d.bones[3];
    const float dy = probe.bones[7] - d.bones[7];
    const float dz = probe.bones[11] - d.bones[11];
    if (dx * dx + dy * dy + dz * dz > 2.25f) {
      return;  // > 1.5 m: a twin's draw, not this entity's
    }
    probe.caster_bank = BankIsOrtho(base, bank);
    probe.pending = false;
    probe.dbg_src = 2;
    d = std::move(probe);
    if (d.char_family != 0 && g_frame_char_refresh.size() < 256) {
      g_frame_char_refresh.emplace(key, oldest->second);
    }
    if (!d.caster_bank) {
      g_frame_pending_by_buffers.erase(oldest);
    }
    return;
  }
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
    d.caster_bank = BankIsOrtho(base, bank);
  } else {
    // Deferred rigid prop: the world matrix is wherever this draw's layout
    // keeps it (pre-pass c4..c7, main-pass c8..c11). Not plausible -> wait
    // for a later draw with these buffers. Aux perspective passes (portrait
    // RTTs) stage off-map portrait-stage worlds; skip those draws too.
    if (BankIsAuxPerspective(base, bank) || !BankRigidWorld(base, bank, d.world)) {
      return;
    }
    d.caster_bank = false;
  }
  d.pending = false;
  d.dbg_src = 2;
  if (d.char_family != 0 && g_frame_char_refresh.size() < 256) {
    g_frame_char_refresh.emplace(key, oldest->second);
  }
  if (!d.caster_bank) {
    g_frame_pending_by_buffers.erase(oldest);
  }
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

// Synthetic-pan probe state/helpers: native/skate3_native_diag.h (the
// engage/step orchestration stays in BuildFrameScene below; it reads the
// live camera and scene locals).

// ---- Off-screen retention (edge-of-view teardown fix) ----------------------
// The game culls its submission stream to ITS camera pose; the native frame
// renders with the re-timed (smoothed) pose, which TRAILS the guest pose by
// up to the boxcar window (~25-50 ms). During a pan the guest stops
// submitting items that left its (leading) frustum while the (trailing)
// rendered view still contains them; world geometry visibly tore down right
// at the screen edges (the emulated frame can never show this: it IS the
// guest frame). Recently seen statics stay retained here and are re-appended
// while the RAW guest frustum can NOT see them (i.e. the cull was
// view-driven). An item the guest frustum CAN see but did not submit was
// really removed (LOD switch, despawn, streaming) and drops immediately;
// retaining those would z-fight the replacement LOD. Statics only: dynamic
// poses go stale the moment they stop being captured. Guest render thread
// only (BuildFrameScene); flips request a clear via the atomic.
struct RetainedItem {
  DrawItem item;
  uint64_t last_seen = 0;  // g_guest_frame of the last live submission
};
std::unordered_map<uint64_t, RetainedItem> g_retained_items;
// Dynamic sibling for traffic VEHICLES (char families 6/7): a vehicle
// passing CLOSE leaves the guest frustum while the trailing rendered pose
// still shows it; its captures stop and it tore down in view (and the
// frames just before that alternated perspective / stale caster-bank
// captures; see the caster ingest guard in InterpolateDynamicItems).
// Entries hold the last live NON-caster capture; matched by mesh +
// bone-derived position (clones). Guest render thread only. Characters/
// NPCs are deliberately excluded: their ropa garments cannot be retained
// coherently (a body without its shirt is worse than the teardown).
struct DynRetained {
  DrawItem item;
  uint64_t last_seen = 0;
  float pos[3] = {};  // plausible-bone average (world)
  float half = 2.0f;  // bbox-diagonal half-extent for the frustum test
};
std::vector<DynRetained> g_dyn_retained;

// True when all 8 world-space corners fall outside one clip plane of `vp`
// (row-vector view*proj). `margin` scales the tested frustum: < 1 shrinks
// it (bounds poking just inside an edge still count as outside; the
// game's own cull volumes are tighter than a mesh bbox), > 1 widens it
// (only clearly-outside counts).
bool CornersOutsideFrustum(const float (&corners)[8][3], const float vp[16],
                           float margin) {
  int outside[6] = {};
  for (int c = 0; c < 8; ++c) {
    const float* p = corners[c];
    float clip[4];
    for (int k = 0; k < 4; ++k) {
      clip[k] = p[0] * vp[0 * 4 + k] + p[1] * vp[1 * 4 + k] + p[2] * vp[2 * 4 + k] +
                vp[3 * 4 + k];
    }
    // D3D clip volume: -w <= x <= w, -w <= y <= w, 0 <= z <= w. The game's
    // negative projection x-scale only swaps left/right; the tests are
    // symmetric. Corners behind the camera land in z < 0.
    const float m = margin * clip[3];
    if (clip[0] < -m) ++outside[0];
    if (clip[0] > m) ++outside[1];
    if (clip[1] < -m) ++outside[2];
    if (clip[1] > m) ++outside[3];
    if (clip[2] < 0.0f) ++outside[4];
    if (clip[2] > clip[3]) ++outside[5];
  }
  for (int k = 0; k < 6; ++k) {
    if (outside[k] == 8) {
      return true;
    }
  }
  return false;
}

// The item's world-space bbox against the frustum (statics: bbox is
// mesh-local, world transforms it; fmt-57 absolute geometry carries an
// identity world with world-space bounds).
bool ItemOutsideFrustum(const DrawItem& it, const float vp[16], float margin) {
  float corners[8][3];
  for (int c = 0; c < 8; ++c) {
    const float l[3] = {c & 1 ? it.bbox_max[0] : it.bbox_min[0],
                        c & 2 ? it.bbox_max[1] : it.bbox_min[1],
                        c & 4 ? it.bbox_max[2] : it.bbox_min[2]};
    const float* w = it.world;
    for (int k = 0; k < 3; ++k) {
      corners[c][k] = l[0] * w[0 * 4 + k] + l[1] * w[1 * 4 + k] +
                      l[2] * w[2 * 4 + k] + w[3 * 4 + k];
    }
  }
  return CornersOutsideFrustum(corners, vp, margin);
}

// Axis-aligned cube of half-extent `half` around `center` against the
// frustum, the skinned-item variant (a palette's world position lives in
// its bone translations, not the identity item world).
bool BoxOutsideFrustum(const float center[3], float half, const float vp[16],
                       float margin) {
  float corners[8][3];
  for (int c = 0; c < 8; ++c) {
    corners[c][0] = center[0] + (c & 1 ? half : -half);
    corners[c][1] = center[1] + (c & 2 ? half : -half);
    corners[c][2] = center[2] + (c & 4 ? half : -half);
  }
  return CornersOutsideFrustum(corners, vp, margin);
}

// SynPanAngleDeg / SynPanView: native/skate3_native_diagnostics.cpp.

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
      CamSigPush(now, 0.0, YawFromViewRows(s.view), 0);
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
void InterpolateDynamicItems(uint8_t* base, FrameScene& scene, double now) {
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
    // ROPA: the newest cloth-shape generation (dyn job seq) that existed
    // when this pose was captured, the shape that belongs WITH this pose
    // (constant enqueue offset; the draw lerps the bracketing generations).
    uint64_t shape_seq = 0;
  };
  struct DynHist {
    DynPose ring[kRing];
    int count = 0;
    int newest = 0;
    uint64_t seen = 0;
    // EMA of this entity's own pose-change period. Characters/board update
    // at the 60 Hz sim value cadence (~16.7 ms); traffic vehicles update
    // SLOWER (their own sim rate); those entities need their evaluation
    // point delayed by one own-period or the playback clock runs past
    // their newest sample and they render raw stepped poses (see play_e).
    double period = 0.0;
    // Per-bone skin-weighted vertex centroids in BIND space (w, x, y, z),
    // lazily computed from the mesh's own vertex buffer the first time a
    // bone of this entity trips the spin-collapse guard. The centroid IS
    // the wheel's geometric center (verified in capture:
    // wheel-mesh bone centroids match the motion-solved spin pivot to a
    // millimeter), so the collapse guard can pin it to the boxcar path
    // exactly, no runtime estimation (every estimator variant tested was
    // noisier than the artifact it fixed).
    uint32_t cen_vb = 0;
    uint32_t cen_bytes = 0;
    std::vector<std::array<float, 4>> cen;
    // Last RENDERED pose on this ring (post-interpolation / raw fallback):
    // the flick detector's reference. Vehicles only (fam 6/7).
    std::vector<float> last_final;
    uint64_t last_final_frame = 0;
    uint8_t last_final_src = 0;
    bool last_final_caster = false;
    // Timestamp of the last PERSPECTIVE-sourced (non-caster, non-retained)
    // sample ingested: decides whether a caster pose tracks (caster-only
    // stream) or holds (perspective stream fresh; interleaving the two
    // sawtooths the wheel phase).
    double last_persp_t = 0.0;
  };
  static std::unordered_map<uint64_t, DynHist> s_hist;
  static uint64_t s_frame = 0;
  ++s_frame;
  // Bone-signal recorder window state (see BoneSigAppend). Written on a
  // detached thread once the window closes.
  const bool bs_rec = BoneSigTick(now);
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
    // Skinned pose-to-pose translation distance^2 at a NON-SPINNING
    // reference. Fast wheel bones sweep multi-meter circles between ~8 ms
    // samples (the model->world affine's translation carries the axle-
    // pivot compensation; measured 2.9 m single-sample swings on traffic
    // at speed), and bone 0 IS a wheel on the vehicle meshes, so keying
    // the teleport gate on it reset the ring every few samples and
    // traffic spent most of its time on raw stepped poses ("the whole
    // vehicle lags then jumps to catch up"; the board's centimeter wheel
    // offsets never trip it). A genuine teleport/mispair moves EVERY
    // bone, spin moves only the wheels: with the entity's vertex-weighted
    // bone set known (the centroid table), take the MINIMUM jump over
    // real bones; until it exists, bone 0.
    const auto skinned_dist2 = [&](const std::vector<float>& a,
                                   const std::vector<float>& b,
                                   const DynHist& hh) -> float {
      const auto bone_d2 = [&](size_t bone) -> float {
        const size_t bi = bone * 12;
        const float dx = a[bi + 3] - b[bi + 3];
        const float dy = a[bi + 7] - b[bi + 7];
        const float dz = a[bi + 11] - b[bi + 11];
        return dx * dx + dy * dy + dz * dz;
      };
      const size_t nbones = a.size() / 12;
      float best = 1e30f;
      const size_t ncen = std::min(hh.cen.size(), nbones);
      for (size_t bone = 0; bone < ncen; ++bone) {
        if (hh.cen[bone][0] > 0.5f) {
          best = std::min(best, bone_d2(bone));
        }
      }
      return best < 1e30f ? best : bone_d2(0);
    };
    // Distance^2 from this item's new pose to a history's newest pose;
    // 1e30 when the history is empty, stale, or a different palette size.
    const auto hist_dist2 = [&](const DynHist& hh) -> float {
      if (hh.count == 0) {
        return 1e30f;
      }
      const DynPose& lp = hh.ring[hh.newest];
      if (now - lp.t > 0.1) {
        return 1e30f;
      }
      if (skinned) {
        if (lp.b.size() != item.bones.size() || lp.b.size() < 12) {
          return 1e30f;
        }
        return skinned_dist2(item.bones, lp.b, hh);
      }
      const float dx = item.world[12] - lp.w[12];
      const float dy = item.world[13] - lp.w[13];
      const float dz = item.world[14] - lp.w[14];
      return dx * dx + dy * dy + dz * dz;
    };
    // One-time bind-space vertex-centroid table for this entity (see the
    // pivot-boxcar comment at the collapse guard). Also computed EAGERLY on
    // an entity's first pose so the teleport gate's non-spinning reference
    // (skinned_dist2) exists before the first interpolated frame; a car
    // first seen at full speed otherwise reset its ring off the wheel
    // swings forever and never reached the collapse path that used to
    // build this table.
    const auto ensure_cen = [&](DynHist& hh, size_t nbones) {
          if ((hh.cen_vb != item.vb_addr || hh.cen_bytes != item.vb_bytes) &&
              item.stride != 0 && item.bw_offset != 0 && item.bi_offset != 0) {
            // One-time bind-space centroid pass over this entity's vertex
            // buffer (guest thread; raw guest reads are safe here, same
            // as RefinePaletteBase). u8x4 attributes are big-endian per
            // 32-bit word: component k is byte (24 - 8k) of the host-order
            // load.
            hh.cen_vb = item.vb_addr;
            hh.cen_bytes = item.vb_bytes;
            hh.cen.assign(nbones, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            const uint32_t vcount =
                std::min<uint32_t>(item.vb_bytes / item.stride, 200000);
            for (uint32_t vtx = 0; vtx < vcount; ++vtx) {
              SkinSampleVert sv;
              if (!ReadSkinVertGuest(base, item, vtx, &sv)) {
                break;  // unsupported position format
              }
              for (int k = 0; k < 4; ++k) {
                const uint32_t wgt = sv.w[k];
                if (wgt == 0) {
                  continue;
                }
                const uint32_t bone = sv.bone[k];
                if (bone >= nbones) {
                  continue;
                }
                auto& cb = hh.cen[bone];
                const float wf = float(wgt) * (1.0f / 255.0f);
                cb[0] += wf;
                cb[1] += wf * sv.p[0];
                cb[2] += wf * sv.p[1];
                cb[3] += wf * sv.p[2];
              }
            }
            for (auto& cb : hh.cen) {
              if (cb[0] > 0.5f) {
                cb[1] /= cb[0];
                cb[2] /= cb[0];
                cb[3] /= cb[0];
              }
            }
          }
    };
    // LW-mapped items key their ring by the game's own per-instance
    // identity (the MeshContext); sort-
    // list clone reshuffles cannot mispair an identity key, so the whole
    // positional claim search below is skipped for them. Mesh stays in the
    // key so a model/LOD swap on the same instance starts a fresh ring
    // (pose sizes differ). Legacy (mesh, occurrence) pairing continues to
    // serve everything without a store entry (player, CAC, non-LW).
    bool lw_keyed = item.lw_entity != 0 && item.ctx != 0 &&
                    REXCVAR_GET(skate3_native_render_scene_lw_identity);
    uint64_t key = lw_keyed ? ((1ull << 63) | (uint64_t(item.ctx) << 32) |
                               uint64_t(item.mesh))
                            : ((uint64_t(item.mesh) << 8) | (k & 0xFF));
    DynHist* hp = &s_hist[key];
    if (lw_keyed && hp->seen == s_frame) {
      // One ctx published twice in a frame (should not happen; dyn_slot
      // dedups per ctx): fall back to the legacy pairing for this copy
      // rather than double-ingesting the identity ring.
      lw_keyed = false;
      key = (uint64_t(item.mesh) << 8) | (k & 0xFF);
      hp = &s_hist[key];
    }
    const float own_d2 = hp->seen == s_frame ? 1e30f : hist_dist2(*hp);
    if (!lw_keyed && own_d2 > 1e-4f) {
      // The k-th slot mispairs: the game's sort lists RESHUFFLE same-mesh
      // clones as they and the camera move. For static props the
      // reset-on-jump guard below was enough (a mispair rendered raw for a
      // few frames), but driving traffic reshuffles CONSTANTLY; the rings
      // never accumulated 3 poses and every moving car rendered raw
      // stepped poses (the vehicle judder/catch-up). Re-pair by POSITION
      // instead: claim the unclaimed history of this mesh whose newest
      // pose is nearest, within the same 1.5 m one-tick jump gate.
      // The search runs whenever the slot is not an (almost) EXACT
      // continuation, not only past the 1.5 m gate: clone placements
      // CLOSER than 1.5 m (the paired newspaper holders, 0.78 m apart)
      // otherwise inherit each other's history on every reshuffle, and the
      // interpolator renders them sliding between the two placements ("the
      // props jitter in and out of position"). Seeding `best` with the own
      // slot's distance keeps a claim strictly-nearer-only, so a genuinely
      // moving entity still prefers its own ring.
      // RIGID claims get a far tighter cap than the vehicles' 1.5 m: when a
      // close clone's OWN ring goes stale (its submission flapped for a few
      // frames), strictly-nearer alone still claims the TWIN's fresh ring
      // (own = 1e30) and slides once, the residual single jitter seen
      // after the strictly-nearer fix. No rigid prop moves 0.3 m in one sim
      // tick (18 m/s at 60 Hz), while clone placements sit >= 0.78 m apart;
      // a rigid item with no history inside 0.3 m starts a fresh ring and
      // renders raw at its correct placement instead.
      const float claim_cap = skinned ? 2.25f : 0.09f;
      float best = std::min(own_d2, claim_cap);
      DynHist* alt = nullptr;
      uint64_t alt_key = key;
      uint32_t fresh_k = 256;  // first index past the dense key range
      for (uint32_t k2 = 0; k2 < 256; ++k2) {
        const uint64_t key2 = (uint64_t(item.mesh) << 8) | k2;
        const auto it2 = s_hist.find(key2);
        if (it2 == s_hist.end()) {
          fresh_k = k2;  // occurrence keys are dense per mesh
          break;
        }
        if (k2 == (k & 0xFF) || it2->second.seen == s_frame) {
          continue;
        }
        const float d2 = hist_dist2(it2->second);
        if (d2 < best) {
          best = d2;
          alt = &it2->second;
          alt_key = key2;
        }
      }
      if (alt != nullptr) {
        hp = alt;
        key = alt_key;
      } else if (hp->seen == s_frame && fresh_k < 256) {
        // The k-th slot already belongs to another clone this frame and no
        // history matches: start a fresh ring instead of corrupting the
        // claimed one with interleaved poses.
        key = (uint64_t(item.mesh) << 8) | fresh_k;
        hp = &s_hist[key];
      }
    }
    DynHist& h = *hp;
    h.seen = s_frame;
    if (skinned) {
      ensure_cen(h, item.bones.size() / 12);
    }
    // Flick FIREWALL ("vehicle suddenly points sideways/upright"): compare
    // the pose about to RENDER against last frame's rendered pose on this
    // ring. No vehicle chassis legally rotates ~45 deg in one frame at any
    // sim rate, while every bad-palette path (mis-located ortho rows,
    // foreign bank, mispaired clone, bad rescue) rotates the whole chassis
    // - minimum over the centroid-weighted bones, so spinning wheels can
    // never trip it. Returns true = suppress this frame's draws (the
    // reference pose is KEPT so a one-off bad palette never renders); four
    // consecutive suppressions accept the new pose stream (a real teleport
    // or respawn appears ~30 ms late instead of never).
    const auto flick_check = [&](const char* path) -> bool {
      if (!skinned || (item.char_family != 6 && item.char_family != 7)) {
        return false;
      }
      if (h.last_final_frame + 1 == s_frame &&
          h.last_final.size() == item.bones.size()) {
        const size_t nb = item.bones.size() / 12;
        const size_t ncen = std::min(h.cen.size(), nb);
        float min_ang = 1e9f;
        float move_at_min = 0.0f;
        int checked = 0;
        for (size_t b = 0; b < nb; ++b) {
          if (ncen != 0 && (b >= ncen || h.cen[b][0] <= 0.5f)) {
            continue;  // not vertex-weighted (junk palette row)
          }
          const size_t bi = b * 12;
          double tr = 0.0, na = 0.0, nb2 = 0.0;
          for (int r = 0; r < 3; ++r) {
            for (int c2 = 0; c2 < 3; ++c2) {
              const double av = item.bones[bi + r * 4 + c2];
              const double bv = h.last_final[bi + r * 4 + c2];
              tr += av * bv;
              na += av * av;
              nb2 += bv * bv;
            }
          }
          // Orthonormal 3x3 rows sum to 3; anything far off is not a
          // rotation and cannot be angle-compared.
          if (na < 1.5 || na > 6.0 || nb2 < 1.5 || nb2 > 6.0) {
            continue;
          }
          const double cth = std::clamp((tr - 1.0) * 0.5, -1.0, 1.0);
          const float ang = float(std::acos(cth) * 57.2957795);
          if (ang < min_ang) {
            min_ang = ang;
            const float dx = item.bones[bi + 3] - h.last_final[bi + 3];
            const float dy = item.bones[bi + 7] - h.last_final[bi + 7];
            const float dz = item.bones[bi + 11] - h.last_final[bi + 11];
            move_at_min = std::sqrt(dx * dx + dy * dy + dz * dz);
          }
          ++checked;
        }
        if (checked > 0 && min_ang > 40.0f) {
          // Detector only, suppression is retired: the sighting logs
          // showed every trigger was a clone RING-SWAP (the sort lists
          // reshuffle same-mesh clones; poses 20-75 m apart are different
          // vehicles' VALID poses), so hiding them blinked legit traffic,
          // while the real artifact (per-bone garbage) slides under the
          // min-over-sane-bones angle and is handled by the bone repair
          // above.
          static std::atomic<uint64_t> s_flicks{0};
          const uint64_t n = s_flicks.fetch_add(1, std::memory_order_relaxed);
          if (n < 24 || (n & 127u) == 0) {
            const float* c0 = item.bones.data();
            const float* p0 = h.last_final.data();
            REXLOG_INFO(
                "native-scene FLICK: mesh={:08X} fam={} path={} ang={:.0f} "
                "move={:.2f} cur[src={} pend={} caster={} retained={}] "
                "prev[src={} caster={}] ring[count={} age_ms={:.0f}] "
                "cur_r0=({:.3f},{:.3f},{:.3f},{:.2f}) "
                "prev_r0=({:.3f},{:.3f},{:.3f},{:.2f}) (n={})",
                item.mesh, item.char_family, path, min_ang, move_at_min,
                item.dbg_src, item.pending ? 1 : 0, item.caster_bank ? 1 : 0,
                item.retained ? 1 : 0, h.last_final_src,
                h.last_final_caster ? 1 : 0, h.count,
                h.count > 0 ? (now - h.ring[h.newest].t) * 1e3 : -1.0, c0[0],
                c0[1], c0[2], c0[3], p0[0], p0[1], p0[2], p0[3], n);
          }
        }
      }
      h.last_final = item.bones;
      h.last_final_frame = s_frame;
      h.last_final_src = item.dbg_src;
      h.last_final_caster = item.caster_bank;
      return false;
    };
    const DynPose& latest = h.ring[h.newest];
    // Per-bone palette repair (vehicles): the sighting captures carried
    // garbage on SOME weighted bones, world positions in the rotation
    // rows, bone 0 gliding along the vehicle's own path, while every
    // sampled-vert gate upstream judged only the bones its samples happen
    // to reference. The survivors rendered as mangled vehicles with zero
    // refusals in the telemetry. Repair the insane bones from the ring's
    // newest pose (the body keeps its live motion; a repaired wheel
    // freezes for the frames its rows are junk); with no sane source for
    // a weighted bone, hide the item for the frame instead.
    if (skinned && (item.char_family == 6 || item.char_family == 7) &&
        !h.cen.empty()) {
      const size_t nbones = item.bones.size() / 12;
      const size_t ncen = std::min(h.cen.size(), nbones);
      const bool ring_ok = h.count > 0 && latest.b.size() == item.bones.size();
      const auto rows_sane = [](const float* bones, size_t bi) {
        for (int r = 0; r < 3; ++r) {
          const float* row = bones + bi + size_t(r) * 4;
          const float n2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
          if (n2 < 0.04f || n2 > 25.0f) {
            return false;
          }
        }
        return true;
      };
      uint32_t repaired = 0;
      bool unrepairable = false;
      for (size_t b = 0; b < ncen && !unrepairable; ++b) {
        if (h.cen[b][0] <= 0.5f) {
          continue;  // not vertex-weighted: staging leftovers are normal
        }
        const size_t bi = b * 12;
        if (rows_sane(item.bones.data(), bi)) {
          continue;
        }
        if (ring_ok && rows_sane(latest.b.data(), bi)) {
          std::memcpy(item.bones.data() + bi, latest.b.data() + bi,
                      12 * sizeof(float));
          ++repaired;
        } else {
          unrepairable = true;
        }
      }
      if (repaired != 0 || unrepairable) {
        static std::atomic<uint64_t> s_repairs{0};
        const uint64_t n = s_repairs.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: vehicle palette {} mesh={:08X} fam={} src={} "
              "caster={} bones={} (n={})",
              unrepairable ? "UNREPAIRABLE (hidden)" : "bone-repair",
              item.mesh, item.char_family, item.dbg_src,
              item.caster_bank ? 1 : 0, repaired, n);
        }
        if (unrepairable) {
          item.draws.clear();
          continue;
        }
      }
    }
    // Caster-bank palettes carry ~40 ms-stale FINE animation but a fresh
    // GROSS pose, when they are the only capture stream they must keep
    // TRACKING (the old 0.5 s ring-hold froze passing vehicles mid-motion
    // in plain view, and the seed/hold/drift-out cycle re-seeded every
    // ~100 ms: "cars stop in their tracks and vanish"). Only while
    // PERSPECTIVE samples are fresh does a caster pose hold the ring pose
    // instead (ingesting both interleaves a wheel-phase sawtooth), a
    // <= 50 ms hold until the next perspective sample, imperceptible.
    // Retained re-publishes always hold: their stored pose is frames old
    // and ingesting it would step the ring backward. Ropa garments
    // included: the gate requires SKINNED mode with an identical palette
    // size, so the substitution cannot mix modes.
    // Stale caster bank: the ortho banks occasionally hold a genuinely OLD
    // palette (not just 40 ms of wheel phase), rendered raw, the vehicle
    // momentarily ghosted 10-20 m back along its own trail. A pose > 3 m
    // from a ring pose younger than 100 ms is physically impossible
    // (> 30 m/s of error); hold the fresh ring pose for the frame. The
    // 30 m ceiling keeps clone ring-swaps (45-75 m in the sighting logs,
    // valid poses of DIFFERENT vehicles) rendering raw.
    const bool caster_stale_jump = [&] {
      if (!skinned || !item.caster_bank || h.count == 0 ||
          latest.b.size() != item.bones.size() || now - latest.t > 0.1) {
        return false;
      }
      const float d2 = skinned_dist2(item.bones, latest.b, h);
      return d2 > 9.0f && d2 < 900.0f;
    }();
    // dbg_src 10 = LW-authoritative palette substitution: the pose IS the
    // entity's current sim tick; holding the ring's (older) pose over it
    // would re-stale exactly what the substitution fixed.
    if (skinned && h.count > 0 && latest.b.size() == item.bones.size() &&
        item.dbg_src != 10 &&
        ((item.caster_bank && now - h.last_persp_t <= 0.05) || item.retained ||
         caster_stale_jump)) {
      if (caster_stale_jump) {
        static std::atomic<uint64_t> s_stale_caster{0};
        const uint64_t n = s_stale_caster.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: stale caster pose held mesh={:08X} fam={} "
              "ring_age_ms={:.0f} (n={})",
              item.mesh, item.char_family, (now - latest.t) * 1e3, n);
        }
      }
      item.bones = latest.b;
    }
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
          // Translation jump at the non-spinning reference (see
          // skinned_dist2; bone 0 is a WHEEL on vehicles); rigid worlds
          // carry t in row 3.
          float d2;
          if (skinned) {
            d2 = skinned_dist2(item.bones, latest.b, h);
          } else {
            const float dx = item.world[12] - latest.w[12];
            const float dy = item.world[13] - latest.w[13];
            const float dz = item.world[14] - latest.w[14];
            d2 = dx * dx + dy * dy + dz * dz;
          }
          // Rigid uses the same tight one-tick bound as the claim cap
          // above: 0.78 m clone placements sit inside the vehicles' 1.5 m
          // gate, and lerping across a mispair IS the prop jitter.
          discontinuity = d2 > (skinned ? 2.25f : 0.09f);
          if (discontinuity && !skinned && d2 < 2.25f) {
            // A rigid step that only the tightened bound caught = a
            // close-clone mispair that would have LERPED (the prop jitter).
            static std::atomic<uint64_t> s_rigid_mispair{0};
            const uint64_t n =
                s_rigid_mispair.fetch_add(1, std::memory_order_relaxed);
            if (n < 24 || (n & 255u) == 0) {
              REXLOG_INFO(
                  "native-scene: rigid close-clone mispair reset mesh={:08X} "
                  "k={} d={:.2f}m (n={})",
                  item.mesh, k, std::sqrt(d2), n);
            }
          }
        }
        if (discontinuity) {
          h.count = 0;
        }
      }
      const double prev_t = h.count > 0 ? h.ring[h.newest].t : 0.0;
      h.newest = h.count == 0 ? 0 : (h.newest + 1) % kRing;
      DynPose& p = h.ring[h.newest];
      // Timestamp with the camera sampler's latest sim tick when fresh:
      // this entity's pose changed on the same sim tick, and frame-grid
      // timestamps alias against the sim rate once the render loop is
      // paced (the same problem the camera sampler solves).
      p.t = (g_latest_cam_tick > 0.0 && now - g_latest_cam_tick < 0.02)
                ? g_latest_cam_tick
                : now;
      if (prev_t > 0.0) {
        // Track the entity's OWN pose-change period (see DynHist::period).
        const double dt = p.t - prev_t;
        if (dt > 0.0005 && dt < 0.1) {
          h.period = h.period == 0.0 ? dt : h.period * 0.75 + dt * 0.25;
        }
      }
      p.b = item.bones;
      std::memcpy(p.w, item.world, sizeof(p.w));
      p.shape_seq = 0;
      if (item.ropa) {
        // Guest thread only, like the enqueue that writes it.
        const auto sit = g_ropa_last_seq.find(item.mesh);
        p.shape_seq = sit != g_ropa_last_seq.end() ? sit->second : 0;
      }
      h.count = std::min(h.count + 1, kRing);
      if (skinned && !item.caster_bank && !item.retained) {
        h.last_persp_t = p.t;
      }
      if (bs_rec) {
        BoneSigAppend(0, key, p.t, 0.0,
                      skinned ? item.bones.data() : item.world,
                      skinned ? uint32_t(item.bones.size()) : 16u);
      }
    }
    if (h.count < 3 || now - h.ring[h.newest].t > 0.1) {
      if (flick_check("raw")) {
        item.draws.clear();
      }
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
    // Per-entity playback point. Entities whose own pose stream is SLOWER
    // than the 60 Hz character cadence (traffic vehicles tick on their own
    // sim rate) pin the shared playback clock past their newest sample;
    // alpha clamps at 1.0, the whole smoothing machinery degenerates to
    // raw stepped poses, and every new sample renders as a visible jump
    // (the vehicle judder/catch-up that survived the entity boxcar).
    // Evaluating one own-period earlier keeps the eval point BRACKETED by
    // samples: the staircase renders as continuous piecewise-linear
    // motion, at the cost of that entity lagging one of ITS sim updates
    // behind the world, invisible for background traffic, and never
    // applied to 60 Hz entities (the skater/NPCs keep the shared clock).
    const double play_e =
        g_smooth_play -
        (h.period > 0.020 ? std::min(h.period - 1.0 / 60.0, 0.1) : 0.0);
    static std::vector<float> acc;  // guest render thread only
    float wacc[16] = {};
    bool ok = true;
    // Did the rigid world take the 8-tap boxcar this frame (vs the plain
    // pair-lerp)? The ROPA shape kernel below must match it exactly.
    bool rigid_world_boxcar = false;
    if (filter_w > 0.0005 && h.count >= 4) {
      constexpr int kTaps = 8;
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      for (int tap = 0; tap < kTaps && ok; ++tap) {
        const double tt =
            std::min(play_e - filter_w * 0.5 + (tap + 0.5) * filter_w / kTaps,
                     h.ring[h.newest].t);
        ok = accum_at(tt, 1.0f / kTaps, acc.data(), wacc);
      }
    } else {
      acc.assign(skinned ? item.bones.size() : 0, 0.0f);
      ok = accum_at(play_e, 1.0f, acc.data(), wacc);
    }
    if (!ok) {
      if (flick_check("raw-size")) {
        item.draws.clear();
      }
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
        if (h.ring[lo2].t <= play_e) {
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
    const float ba = float(std::clamp((play_e - q0.t) / bspan, 0.0, 1.0));
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
          // average, spinning wheels, and briefly fast-swinging limbs).
          // Base fallback: plain adjacent-sample lerp at the playback point
          // (R and t from the SAME pose pair, no orbit; ~7.5 ms spacing
          // keeps shrink to a few %). Bones whose mesh vertices yield a
          // skin-weighted BIND-SPACE CENTROID get the exact PIVOT-BOXCAR
          // form instead: render the orthonormalized lerp rotation
          // translated so the centroid rides the same boxcar path as every
          // other filtered bone: t_out = tbar + Rbar*c - Ro*c, where
          // Rbar/tbar is the boxcar affine already in acc[] (its action on
          // ANY fixed bind-space point IS that point's smoothed path). The
          // centroid is the wheel's geometric center (a wheel is symmetric
          // about its axle; verified in capture: mesh
          // centroids match the motion-solved spin pivot to a millimeter),
          // so the wheel is exact at its center and sub-mm across its
          // ~4 cm extent; a limb pins its own centroid to its smoothed
          // path (full-norm rotation, no lerp shrink). Offline-validated:
          // wheel-vs-deck
          // high-frequency wobble 1.3 -> 0.02 cm rms at constant speed /
          // 1.0 -> 0.29 cm at 25 m/s with speed changes, better than the
          // game's own 60 Hz output (0.05-0.56 cm). DO NOT replace the
          // centroid with a motion-ESTIMATED pivot: every estimator shape
          // tried (shared / per-window velocity elimination, quadratic
          // motion models, theta gates, consistency resets, axis
          // projection) was noisier than the artifact it fixed.
          // Junk palette rows
          // (unreferenced by vertices) get no centroid weight and keep the
          // lerp.
          ensure_cen(h, nbones);
          uint64_t pivot_upgraded = 0;
          for (size_t b = 0; b < nbones; ++b) {
            if (!s_collapsed[b]) {
              continue;
            }
            const size_t bi = b * 12;
            float lp[12];
            for (int i = 0; i < 12; ++i) {
              lp[i] = q0.b[bi + i] + (q1.b[bi + i] - q0.b[bi + i]) * ba;
            }
            const bool upgraded = [&]() {
              if (filter_w <= 0.0005 || b >= h.cen.size() ||
                  h.cen[b][0] <= 0.5f) {
                return false;  // no centroid (junk row / undecodable VB)
              }
              const float c[3] = {h.cen[b][1], h.cen[b][2], h.cen[b][3]};
              const float* bar = acc.data() + bi;
              // UNDERSAMPLED spin: traffic wheels at speed turn > 90 deg
              // between adjacent ~8 ms samples; the pair-lerp rotation is
              // then meaningless, and with a car wheel's bind centroid
              // meters from the model origin the (Rbar - Ro)*c pin wobbled
              // the wheel ~1 m around its well (bone-signal measured; the
              // board's slow wheels never hit this). When the pair spans
              // more than ~50 deg (Frobenius dot of the 3x3s: trace(R0^T
              // R1) = 1 + 2cos(theta) for rotations), render the pair's
              // NEWEST rotation instead; spin phase snaps once per
              // sample, invisible at those rev rates, while the centroid
              // pin still holds the wheel exactly on its smoothed path.
              double pair_tr = 0.0;
              for (int i = 0; i < 12; ++i) {
                if ((i & 3) == 3) {
                  continue;  // translation column
                }
                pair_tr += double(q0.b[bi + i]) * q1.b[bi + i];
              }
              const bool snap_spin = pair_tr < 2.28;  // 1 + 2cos(50 deg)
              const float* rsrc = snap_spin ? q1.b.data() + bi : lp;
              // Orthonormalize the source rotation (rows): recovers the
              // full-norm midpoint rotation from the lerp's shrunk chord
              // (or just cleans up the raw newest sample in snap mode).
              double r0[3] = {rsrc[0], rsrc[1], rsrc[2]};
              double r1[3] = {rsrc[4], rsrc[5], rsrc[6]};
              double n0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
              if (n0 < 1e-4) {
                return false;
              }
              for (double& v : r0) v /= n0;
              const double d01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
              for (int i = 0; i < 3; ++i) r1[i] -= d01 * r0[i];
              const double n1 =
                  std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
              if (n1 < 1e-4) {
                return false;
              }
              for (double& v : r1) v /= n1;
              const double r2[3] = {r0[1] * r1[2] - r0[2] * r1[1],
                                    r0[2] * r1[0] - r0[0] * r1[2],
                                    r0[0] * r1[1] - r0[1] * r1[0]};
              const double* rows[3] = {r0, r1, r2};
              // t_out = tbar + Rbar*c - Ro*c: the pivot lands exactly on
              // the boxcar path (in phase with the deck and every other
              // filtered bone), spin phase comes from the lerp pair.
              for (int r = 0; r < 3; ++r) {
                double rb = 0.0, ro = 0.0;
                for (int c2 = 0; c2 < 3; ++c2) {
                  rb += double(bar[r * 4 + c2]) * c[c2];
                  ro += rows[r][c2] * c[c2];
                }
                acc[bi + r * 4 + 0] = float(rows[r][0]);
                acc[bi + r * 4 + 1] = float(rows[r][1]);
                acc[bi + r * 4 + 2] = float(rows[r][2]);
                acc[bi + r * 4 + 3] = float(double(bar[r * 4 + 3]) + rb - ro);
              }
              return true;
            }();
            if (upgraded) {
              ++pivot_upgraded;
            } else {
              for (int i = 0; i < 12; ++i) {
                acc[bi + i] = lp[i];
              }
            }
          }
          // Sparse telemetry: how many bones ride the pivot upgrade.
          static uint64_t s_pivot_frames = 0, s_pivot_bones = 0;
          s_pivot_bones += pivot_upgraded;
          if (++s_pivot_frames % 3000 == 0 && s_pivot_bones > 0) {
            REXLOG_INFO("native-scene pivot: {} bone-upgrades / 3000 frames",
                        s_pivot_bones);
            s_pivot_bones = 0;
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
        rigid_world_boxcar = filter_w > 0.0005 && h.count >= 4;
      }
    }
    // ROPA shape pairing: express the EXACT temporal kernel the garment's
    // world (and the body's bones) were just evaluated with as weights over
    // the decoded shape generations (DynDecodeJob seq). The previous plain
    // 2-generation lerp removed the stepping but reconstructed the 60 Hz
    // limb signal SHARPLY against the 50 ms boxcar-rounded body, two
    // different frequency responses to the same signal, diverging at every
    // piecewise-linear corner by an excursion that scales with the per-tick
    // step size (the residual tee jelly, worse at a 60 fps guest cap;
    // emulated applies no filter at all and shows none). The kernel follows
    // the world's LIVE decision: boxcar taps only when the world actually
    // took the boxcar this frame (not the spin-collapse pair-lerp
    // fallback), so cloth and body always ride the same filter.
    if (item.ropa && !skinned) {
      item.shape_count = 0;
      const int bias = std::clamp<int>(
          REXCVAR_GET(skate3_native_render_scene_ropa_bias), -2, 2);
      // Bracket the ring at tt exactly like accum_at (same walk, same span
      // clamp) and return the bracketing pair's shape generations + alpha.
      const auto shape_at = [&](double tt, uint64_t& s0, uint64_t& s1,
                                float& a) {
        int hi = h.newest;
        int lo = (h.newest + kRing - 1) % kRing;
        for (int step = 1; step < h.count - 1; ++step) {
          if (h.ring[lo].t <= tt) {
            break;
          }
          hi = lo;
          lo = (lo + kRing - 1) % kRing;
        }
        // Live pairing trim (skate3_native_render_scene_ropa_bias): step
        // the shape source N ring poses fresher/older than the bracket.
        int blo = lo, bhi = hi;
        for (int b = 0; b < bias; ++b) {
          if (bhi == h.newest) break;
          blo = bhi;
          bhi = (bhi + 1) % kRing;
        }
        for (int b = 0; b > bias; --b) {
          bhi = blo;
          blo = (blo + kRing - 1) % kRing;
        }
        const DynPose& p0 = h.ring[blo];
        const DynPose& p1 = h.ring[bhi];
        if (p0.shape_seq == 0 || p1.shape_seq == 0) {
          return false;
        }
        const double span = std::max(h.ring[hi].t - h.ring[lo].t, 0.0005);
        s0 = p0.shape_seq;
        s1 = p1.shape_seq;
        a = float(std::clamp((tt - h.ring[lo].t) / span, 0.0, 1.0));
        return true;
      };
      const auto add_gen = [&](uint64_t seq, float wgt) {
        if (wgt <= 0.0f) {
          return;
        }
        for (int k = 0; k < item.shape_count; ++k) {
          if (item.shape_seq[k] == seq) {
            item.shape_w[k] += wgt;
            return;
          }
        }
        if (item.shape_count < DrawItem::kShapeGens) {
          item.shape_seq[item.shape_count] = seq;
          item.shape_w[item.shape_count] = wgt;
          ++item.shape_count;
        }
        // Overflow: the weight is dropped; the draw renormalizes over the
        // generations it has (kShapeGens=10 covers the 8-tap window's
        // distinct brackets even at a 140 Hz guest).
      };
      if (rigid_world_boxcar &&
          REXCVAR_GET(skate3_native_render_scene_ropa_boxcar)) {
        constexpr int kShapeTaps = 8;  // == the body kernel's kTaps
        for (int tap = 0; tap < kShapeTaps; ++tap) {
          const double tt = std::min(
              play_e - filter_w * 0.5 + (tap + 0.5) * filter_w / kShapeTaps,
              h.ring[h.newest].t);
          uint64_t s0 = 0, s1 = 0;
          float a = 0.0f;
          if (shape_at(tt, s0, s1, a)) {
            add_gen(s0, (1.0f - a) / kShapeTaps);
            add_gen(s1, a / kShapeTaps);
          }
        }
      } else {
        uint64_t s0 = 0, s1 = 0;
        float a = 0.0f;
        if (shape_at(play_e, s0, s1, a)) {
          add_gen(s0, 1.0f - a);
          add_gen(s1, a);
        }
      }
    }
    if (flick_check("interp")) {
      item.draws.clear();
    }
    if (bs_rec) {
      BoneSigAppend(1, key, now, play_e,
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

// Publish the frame's 2D overlay draws (BuildFrameScene, before any early
// return; menu and empty frames still carry 2D). The inline-ring vertex
// payloads are complete by frame end; convert them to little-endian and
// expand quad lists into triangle lists so the render side stays trivial.
void Publish2dDraws(uint8_t* base) {
  std::vector<Draw2d> frame_2d;
  {
    std::lock_guard<std::mutex> lock(g_2d_mutex);
    frame_2d.swap(g_frame_2d);
  }
  static thread_local std::vector<uint8_t> scratch_2d;
  std::vector<Draw2d> published;
  published.reserve(frame_2d.size());
  for (Draw2d& d : frame_2d) {
    // OFFSCREEN COMPOSITION draws: bracket bits carrying ONLY SimpleDraw
    // (0x20) / font (0x10) with none of the screen-pass brackets (bit 0
    // FrontEndManager::Render2D, bit 1 AptMovieIntegration, bit 2
    // DrawRenderingUnit, bit 3 the HUD render-to-texture pass) are the
    // game's internal render-target helpers, not screen UI; the
    // skater-portrait generator composes the card through bare SimpleDraw
    // quads in the TARGET's coordinate space (traced:
    // flags=20 fullscreen 1152x640 postfx blit + centered
    // 324x640 portrait compose quads on textures 03f47054/03f3f054,
    // replayed on screen they were the centered "poster" flash on every
    // menu entry / skater switch, sampling whatever the resolve arena
    // still held). Every real UI draw in gameplay, pause and the frontend
    // carries at least one screen bracket (observed flags 0d/19/29/2b/2d).
    // flags == 0 (unbracketed boot/loading capture) keeps its own gate.
    if (d.flags != 0 && (d.flags & 0x0Fu) == 0) {
      static std::atomic<uint32_t> s_offscreen_2d{0};
      const uint32_t n = s_offscreen_2d.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 2047u) == 0) {
        REXLOG_INFO(
            "native-scene: offscreen 2D compose draw dropped (flags={:02x} "
            "tex={:08x} count={}) (n={})",
            d.flags, d.fetch[1], d.count, n);
      }
      g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
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
        // Unbracketed captures (d.flags == 0, native loading/boot frames)
        // take the SimpleDraw layout readings for 16/28: EA's inline-quad
        // helpers order color before texcoords everywhere.
        if (d.stride == 16 && (simple || d.flags == 0)) {  // pos4, untextured
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
        } else if (d.stride == 28 && (simple || d.flags == 0)) {  // pos4+color+uv
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
      d.src_stride = d.stride;
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
    // BIG-QUAD TRACER (reported symptom: an unrelated "mongo poster" texture
    // flashes portrait-shaped at screen center on team/import entry and
    // skater switches; idle F11s never catch it). Edge-triggered: log each
    // TEXTURED replayed 2D draw whose transformed extent covers >= 8% of
    // the 1280x720 APT space (the portrait box itself is ~8%), once per
    // texture base per 5 s window. Names the quad's texture / bracket /
    // geometry / timing for the fix.
    if (d.fetch[0] != 0) {
      float mn[2] = {1e9f, 1e9f};
      float mx[2] = {-1e9f, -1e9f};
      const float* m = d.consts;  // c0..c8; c4..c7 = 2D transform rows
      const uint32_t nv = d.count;
      for (uint32_t v = 0; v < nv; ++v) {
        const uint8_t* p = d.verts.data() + size_t(v) * d.stride;
        float pos[4];
        std::memcpy(pos, p, 16);
        for (int c = 0; c < 2; ++c) {
          const float t = pos[0] * m[16 + c] + pos[1] * m[20 + c] +
                          pos[2] * m[24 + c] + pos[3] * m[28 + c];
          mn[c] = std::min(mn[c], t);
          mx[c] = std::max(mx[c], t);
        }
      }
      const float w = mx[0] - mn[0];
      const float h = mx[1] - mn[1];
      if (w > 0.0f && h > 0.0f && w * h >= 0.08f * 1280.0f * 720.0f &&
          w * h < 1e8f) {
        static std::unordered_set<uint32_t> s_seen;
        static int64_t s_window_s = 0;
        static std::atomic<uint32_t> s_big{0};
        const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        if (now_s - s_window_s >= 5) {
          s_window_s = now_s;
          s_seen.clear();
        }
        if (s_seen.size() < 24 && s_seen.insert(d.fetch[1]).second) {
          REXLOG_INFO(
              "native-scene: BIG 2D quad tex=({:08x},{:08x},{:08x}) flags={:02x} "
              "src_stride={} count={} bbox=({:.0f},{:.0f})-({:.0f},{:.0f}) "
              "c8=({:.2f},{:.2f},{:.2f},{:.2f}) (n={})",
              d.fetch[0], d.fetch[1], d.fetch[2], d.flags, d.src_stride, d.count,
              mn[0], mn[1], mx[0], mx[1], m[32], m[33], m[34], m[35],
              s_big.fetch_add(1, std::memory_order_relaxed));
        }
      }
    }
    // TRANSITION-FADE TRACER: the game's screen-to-screen fades are
    // fullscreen SimpleDraw fills (RenderMan::FinalQuadFade ->
    // Draw_QuadListColoured, stride 16, color+ramping alpha in VS c8; the
    // fade color/alpha global sits at [0x83083C38]+31376+848/864, enable
    // byte +880). A mid-ramp alpha logged here proves the fade is captured
    // and replaying natively; if transitions still look like hard cuts
    // with these lines present, the problem is render pacing, not capture.
    // Rolling-capped: 6 lines per 2 s window.
    if (d.src_stride == 16 && d.count >= 4) {
      float alpha = d.consts[35];
      if (alpha > 0.02f && alpha < 0.98f) {
        float x0, y0, x1, y1;
        std::memcpy(&x0, d.verts.data(), 4);
        std::memcpy(&y0, d.verts.data() + 4, 4);
        std::memcpy(&x1, d.verts.data() + size_t(2) * d.stride, 4);
        std::memcpy(&y1, d.verts.data() + size_t(2) * d.stride + 4, 4);
        if (std::fabs(x1 - x0) >= 1200.0f && std::fabs(y1 - y0) >= 680.0f) {
          static std::atomic<uint32_t> s_fade_logs{0};
          static std::atomic<int64_t> s_fade_win{0};
          const int64_t now_s =
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          int64_t win = s_fade_win.load(std::memory_order_relaxed);
          if (now_s - win >= 2 && s_fade_win.compare_exchange_strong(win, now_s)) {
            s_fade_logs.store(0, std::memory_order_relaxed);
          }
          if (s_fade_logs.fetch_add(1, std::memory_order_relaxed) < 6) {
            REXLOG_INFO(
                "native-scene: transition fade fill alpha={:.2f} "
                "rgb=({:.2f},{:.2f},{:.2f}) flags={:02x}",
                alpha, d.consts[32], d.consts[33], d.consts[34], d.flags);
          }
        }
      }
    }
    published.push_back(std::move(d));
  }
  std::lock_guard<std::mutex> lock(g_2d_mutex);
  g_scene_2d = std::move(published);
  ++g_scene_2d_generation;
}

// Publish the frame's in-world spline draws: evaluate the guest B-spline
// VS on the CPU (see SplineDraw for the decoded algorithm) into WORLD-space
// strip vertices; the render side projects them with the scene's (smoothed)
// view_proj like every other world item, keeping them in phase with the
// re-timed camera.
void PublishSplineDraws(uint8_t* base) {
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
      // Fade still evaluates against the draw's own clip z (the ramps
      // span hundreds of meters; the ~30 ms offset from the smoothed
      // camera is invisible), but the PUBLISHED position is WORLD-space:
      // the render side projects with the scene's (smoothed) view_proj so
      // the neon signs ride the exact same camera timeline as the world.
      // Baking the guest VP here was the "waypoint sign judders / lags
      // and catches up" bug once smooth_camera re-timed everything else.
      float clip_z;
      {
        const float* pr = row(2);
        clip_z = pr[0] * wp[0] + pr[1] * wp[1] + pr[2] * wp[2] + pr[3];
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
          std::min(ramp(clip_z, cv[0], cv[1]), 1.0f - ramp(clip_z, cv[2], cv[3]));
      dst[0] = wp[0];
      dst[1] = wp[1];
      dst[2] = wp[2];
      dst[3] = 1.0f;
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

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  if (!SceneEnabled()) {
    return;
  }
  // Published every frame (not just on world submissions): boot/menu frames
  // carry only 2D, and the render thread's 2D texture decodes need the
  // guest base from the very first natively rendered boot frame.
  g_guest_base.store(base, std::memory_order_relaxed);
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
  Publish2dDraws(base);
  PublishSplineDraws(base);
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
      // Screen-shaped views only. The skater-portrait render-to-texture
      // passes (team menu boxes, Import Skater card) submit their OWN
      // perspective SceneRenderView with a tall narrow projection; picking
      // it here published the portrait as the world scene: the skater
      // flashed FULL SCREEN behind the menu on every entry/scroll, and the
      // publish refreshed g_last_publish_ns so the mode flapped
      // pause-native <-> loading for the 300 ms freshness window each time.
      // aspect(w/h) = m11/m00 of the
      // raw projection; every real screen view is >= 4:3.
      const float m00 = std::fabs(LoadGuestF32(base, cam + 0x60 + 0 * 4));
      const float m11 = std::fabs(LoadGuestF32(base, cam + 0x60 + (1 * 4 + 1) * 4));
      if (!(m00 > 1e-6f) || m11 < m00 * 1.2f) {
        static std::atomic<uint64_t> s_aux_views{0};
        const uint64_t n = s_aux_views.fetch_add(1, std::memory_order_relaxed);
        if (n < 4 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: aux perspective view skipped (portrait RTT "
              "pass, proj aspect {:.2f}) (n={})",
              m00 > 1e-6f ? m11 / m00 : 0.0f, n);
        }
        continue;
      }
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
  // Per-INSTANCE candidates (ctx-keyed, character families): the mesh-keyed
  // rescue above is pub_count==1-gated, so a refused CLONE capture next to
  // a published twin dropped for the frame: the NPC visible/invisible
  // flicker (see g_bones_cache_ctx).
  std::unordered_map<uint32_t, const DrawItem*> pending_skinned_by_ctx;
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
          if (cand.ctx != 0 && cand.char_family != 0) {
            pending_skinned_by_ctx.try_emplace(cand.ctx, &cand);
          }
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
      } else {
        // Merge the per-pass captures of one context: PALETTE from the
        // perspective (z/main-pass) bank; the ortho caster-cascade banks
        // carry stale fine animation (see DrawItem::caster_bank), but
        // GEOMETRY from the fullest culled island list. The two must be
        // decided independently: a shadow list can be missing body parts
        // the main view needs, and conversely the main view CULLS most of
        // a vehicle the camera is standing inside/next to (wholesale
        // "prefer main pass" replacement published that partial list and
        // near vehicles went invisible).
        DrawItem& cur = scene.items[slot->second];
        const bool fresher = !cand.caster_bank && cur.caster_bank;
        const bool staler = cand.caster_bank && !cur.caster_bank;
        const bool fuller = total_indices(cand) > total_indices(cur);
        // State/geometry grafts require the same resolved mode: ropa
        // garments flip between rigid and skinned per capture, and mixing
        // one copy's palette with another's interpretation is the
        // mangled-ribbon bug. Same-mode ropa IS graftable; excluding ropa
        // wholesale meant a fuller caster-cascade list wholesale-won the
        // merge and the garment published the ortho bank's ~40 ms-stale
        // bone rows while the body meshes (graftable) got fresh palettes:
        // the garment rode ~10 cm off the moving body (speed x 40 ms), the
        // visible successor of the invisible-torso bug once the near-camera
        // acceptance landed. The mode equality check is what prevents the
        // ribbon, not the ropa flag.
        const bool graftable = cand.skinned == cur.skinned &&
                               cand.ropa == cur.ropa && cand.mesh == cur.mesh;
        if (fresher && fuller) {
          cur = cand;
        } else if (fresher && graftable) {
          // Fresher palette, smaller list: adopt the state, keep the
          // fuller geometry (same mesh and buffers, lists differ only in
          // which islands each pass kept).
          cur.bones = cand.bones;
          std::memcpy(cur.world, cand.world, sizeof(cur.world));
          std::memcpy(cur.char_rows, cand.char_rows, sizeof(cur.char_rows));
          std::memcpy(cur.tint, cand.tint, sizeof(cur.tint));
          cur.caster_bank = false;
        } else if (staler && fuller && graftable) {
          // Fuller caster list vs a fresher partial item: keep the fresh
          // palette, adopt the full geometry.
          const DrawItem state = cur;
          cur = cand;
          cur.bones = state.bones;
          std::memcpy(cur.world, state.world, sizeof(cur.world));
          std::memcpy(cur.char_rows, state.char_rows, sizeof(cur.char_rows));
          std::memcpy(cur.tint, state.tint, sizeof(cur.tint));
          cur.caster_bank = false;
        } else if (!fresher && !staler && fuller) {
          cur = cand;  // same pass class: fullest wins, as before
        } else if (staler && fuller) {
          cur = cand;  // ungraftable (ropa): pre-arbitration fullest-wins
        }
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
    } else {
      // Silent world-item drop attribution (the F7 rings show world meshes
      // MISSING for 8-11 frame episodes, the mesh
      // flicker): a transient BuildItemGeometry failure (guarded reads
      // fail while the streamer relocates the mesh/material structures)
      // is indistinguishable from a game-culled record without this log.
      static std::atomic<uint32_t> s_wbuild_logs{0};
      static std::atomic<int64_t> s_wbuild_win{0};
      const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      int64_t win = s_wbuild_win.load(std::memory_order_relaxed);
      if (now_s - win >= 5 && s_wbuild_win.compare_exchange_strong(win, now_s)) {
        s_wbuild_logs.store(0, std::memory_order_relaxed);
      }
      if (s_wbuild_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO("native-scene: world item build FAILED ctx={:08X}", r.a);
      }
    }
  }
  // Ropa garments whose resolved mode FLIPPED this frame: state as published
  // LAST frame, stashed before the cache refresh overwrites it: the
  // flip-hold pass below (after the dyn-job enqueue) may need to re-publish
  // it while the new mode's decode is still in flight (see g_ropa_resident).
  std::unordered_map<uint32_t, RopaResolvedState> ropa_flip_prev;
  // Cross-frame palette rescue + cache refresh (see g_bones_cache): exactly
  // one published copy of a skinned mesh -> refresh its cache entry; zero
  // copies but a refused/pending capture -> re-publish with the cached
  // palette (one frame of pose lag instead of a one-frame disappearance).
  if (REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
    // Dense publish-time coherence gate (see PublishedPaletteSane): a
    // 1-frame junk palette that passed the 6-sample capture gates would
    // otherwise draw the map-length-ribbon flash AND poison the rescue
    // caches (this runs before the refresh below). Re-publish the cached
    // state when a same-mode one exists; else drop the draws; a 1-frame
    // blink beats the ribbon.
    for (DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.empty() || item.pending) {
        continue;
      }
      float spread = 0.0f;
      if (PublishedPaletteSane(base, item, &spread)) {
        continue;
      }
      g_pub_incoherent.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> incoh_logged{0};
      const uint32_t ln = incoh_logged.fetch_add(1, std::memory_order_relaxed);
      if (ln < 32 || (ln & 255u) == 0) {
        REXLOG_INFO(
            "native-scene: publish INCOHERENT palette mesh={:08X} fam={} "
            "ropa={} src={} spread={:.2f} bind=({:.2f},{:.2f},{:.2f}) "
            "bone0_t=({:.1f},{:.1f},{:.1f})",
            item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
            spread, item.bbox_max[0] - item.bbox_min[0],
            item.bbox_max[1] - item.bbox_min[1],
            item.bbox_max[2] - item.bbox_min[2], item.bones[3], item.bones[7],
            item.bones[11]);
      }
      bool healed = false;
      if (item.ropa) {
        const auto cit = g_ropa_state_cache.find(item.mesh);
        if (cit != g_ropa_state_cache.end() && cit->second.skinned &&
            cit->second.bones.size() == item.bones.size()) {
          item.bones = cit->second.bones;
          item.dbg_src = 6;
          healed = true;
        }
      } else {
        // Prefer the per-instance cache (clone-exact) over the mesh-keyed
        // one; healing a clone with its TWIN's palette is a teleport.
        const auto cit = item.ctx != 0 ? g_bones_cache_ctx.find(item.ctx)
                                       : g_bones_cache_ctx.end();
        if (cit != g_bones_cache_ctx.end() &&
            cit->second.bones.size() == item.bones.size() &&
            g_guest_frame - cit->second.frame <= 10) {
          item.bones = cit->second.bones;
          item.dbg_src = 6;
          healed = true;
        } else {
          const auto bit = g_bones_cache.find(item.mesh);
          if (bit != g_bones_cache.end() &&
              bit->second.bones.size() == item.bones.size() &&
              g_guest_frame - bit->second.frame <= 10) {
            item.bones = bit->second.bones;
            item.dbg_src = 6;
            healed = true;
          }
        }
      }
      if (!healed) {
        item.draws.clear();
        // CRITICAL: also drop the junk palette. Leaving it on the item let
        // the cache-refresh loop below store it into the rescue caches;
        // the NEXT frame's trip then "healed" with the poisoned entry and
        // published the ribbon with draws intact (log 1284/1285: the
        // fam=6/fam=1 spread 183-405 bind-local palettes, bone0_t=(0,0,-.5),
        // were re-captured every frame by the src=2 fixup).
        item.bones.clear();
      }
    }
    std::unordered_map<uint32_t, uint32_t> pub_count;
    for (const DrawItem& item : scene.items) {
      // Ropa garments count in EITHER resolved mode (a rigid-resolved copy
      // has no bones but is very much alive; rescuing a pending clone next
      // to it would draw a duplicate garment).
      if (item.ropa || (item.skinned && !item.bones.empty())) {
        ++pub_count[item.mesh];
      }
      if (item.ropa && item.caster_bank) {
        g_ropa_caster.fetch_add(1, std::memory_order_relaxed);
      }
    }
    for (const DrawItem& item : scene.items) {
      if (!(item.ropa || (item.skinned && !item.bones.empty())) ||
          pub_count[item.mesh] != 1) {
        continue;
      }
      if (item.draws.empty()) {
        // Vetoed/dropped this frame (incoherent-palette trip, flip-hold
        // clone drop): whatever state it carries must not become the
        // "last good" rescue entry.
        continue;
      }
      if (item.ropa) {
        // Remember the resolved mode + transform (see g_ropa_state_cache).
        if (g_ropa_state_cache.size() < 512) {
          const bool now_skinned = item.skinned && !item.bones.empty();
          auto [cit, fresh] = g_ropa_state_cache.try_emplace(item.mesh);
          RopaResolvedState& c = cit->second;
          if (!fresh && c.skinned != now_skinned) {
            g_ropa_flip.fetch_add(1, std::memory_order_relaxed);
            ropa_flip_prev.emplace(item.mesh, c);  // pre-flip published state
          }
          c.skinned = now_skinned;
          std::memcpy(c.world, item.world, sizeof(c.world));
          c.bones = item.bones;
        }
      } else if (item.skinned && !item.bones.empty() && !item.caster_bank &&
                 g_bones_cache.size() < 512) {
        // caster_bank palettes are ~40 ms stale; a rescue re-publishing
        // one would jump the entity backwards (see the ring ingest guard).
        CachedBones& cb = g_bones_cache[item.mesh];
        cb.bones = item.bones;
        cb.frame = g_guest_frame;
      }
    }
    // Per-instance palette cache refresh (see g_bones_cache_ctx): every
    // published skinned character-family item keeps its OWN last palette
    // keyed by ctx: no pub_count gate needed, the key IS the instance.
    for (const DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.empty() || item.caster_bank ||
          item.ctx == 0 || item.char_family == 0 || item.ropa ||
          item.draws.empty()) {
        continue;
      }
      if (g_bones_cache_ctx.size() > 2048) {
        for (auto it = g_bones_cache_ctx.begin();
             it != g_bones_cache_ctx.end();) {
          it = g_guest_frame - it->second.frame > 60
                   ? g_bones_cache_ctx.erase(it)
                   : std::next(it);
        }
      }
      CachedBones& cb = g_bones_cache_ctx[item.ctx];
      cb.bones = item.bones;
      cb.frame = g_guest_frame;
    }
    // Per-instance rescue, BEFORE the mesh-keyed one: a refused/pending
    // skinned character capture whose ctx did not publish this frame
    // re-publishes with ITS OWN last palette (<= 10 frames fresh, same
    // bound as the mesh rescue), regardless of how many clones of the mesh
    // are alive. Rescued items bump pub_count so the mesh-keyed loop below
    // cannot double-publish the same candidate.
    for (const auto& [ctxk, cand] : pending_skinned_by_ctx) {
      if (dyn_slot.find(ctxk) != dyn_slot.end()) {
        continue;  // this instance published live
      }
      const auto bit = g_bones_cache_ctx.find(ctxk);
      if (bit == g_bones_cache_ctx.end() ||
          g_guest_frame - bit->second.frame > 10) {
        continue;  // stale = an old pose; one missing frame beats a teleport
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.bones = bit->second.bones;
      rescued.pending = false;
      rescued.dbg_src = 3;
      ++pub_count[rescued.mesh];
      // Mark the ctx published so the LW gap fill below cannot double-
      // publish the same instance this frame.
      dyn_slot.try_emplace(ctxk, scene.items.size() - 1);
      g_lw_ctx_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto& [mesh, cand] : pending_skinned_by_mesh) {
      if (pub_count.find(mesh) != pub_count.end()) {
        continue;  // a live copy published; nothing to rescue
      }
      const auto bit = g_bones_cache.find(mesh);
      if (bit == g_bones_cache.end() ||
          g_guest_frame - bit->second.frame > 10) {
        // Stale cache = an OLD pose: for a moving vehicle the rescue
        // rendered it 10-20 m behind its live position (the momentary
        // ghost-back). One missing frame beats a teleport.
        continue;
      }
      scene.items.push_back(*cand);
      DrawItem& rescued = scene.items.back();
      rescued.bones = bit->second.bones;
      rescued.pending = false;
      rescued.dbg_src = 3;
      if (rescued.ctx != 0) {
        dyn_slot.try_emplace(rescued.ctx, scene.items.size() - 1);
      }
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
      rescued.dbg_src = 4;
      if (rescued.ctx != 0) {
        dyn_slot.try_emplace(rescued.ctx, scene.items.size() - 1);
      }
      g_ropa_rescued.fetch_add(1, std::memory_order_relaxed);
    }
    // LW gap fill (see g_lw_last_items): republish live entities' items
    // whose ctx skipped this frame's records entirely, the class the
    // pending rescues cannot see (no capture happened at all).
    if (REXCVAR_GET(skate3_native_render_scene_lw_gap_fill)) {
      const uint64_t now = g_guest_frame;
      for (auto it = g_lw_last_items.begin(); it != g_lw_last_items.end();) {
        if (dyn_slot.find(it->first) != dyn_slot.end()) {
          ++it;  // published live this frame; refreshed below
          continue;
        }
        LwRetained& r = it->second;
        float alpha = 1.0f;
        uint32_t entity = 0;
        if (now - r.frame > 2 ||
            !skate3::native_lw::LookupLwCtx(it->first, &alpha, &entity)) {
          it = g_lw_last_items.erase(it);
          continue;
        }
        scene.items.push_back(r.item);
        scene.items.back().dbg_src = 9;  // gap fill (refresh below skips it)
        g_lw_gap_filled.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_fill_logged{0};
        const uint32_t ln = s_fill_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: LW gap fill ctx={:08X} mesh={:08X} fam={} "
              "age={} alpha={:.2f} (n={})",
              it->first, r.item.mesh, r.item.char_family, now - r.frame,
              alpha, ln);
        }
        ++it;
      }
      for (const DrawItem& item : scene.items) {
        // NOTE: lw_entity is not stamped yet here (the stamp pass runs just
        // before the smoothing block); LW membership is enforced at FILL
        // time by the store lookup; non-LW entries simply expire unused.
        if (item.char_family == 0 || item.ctx == 0 || item.ropa ||
            item.pending || item.caster_bank || item.dbg_src == 9 ||
            !item.skinned || item.bones.empty() || item.draws.empty()) {
          continue;
        }
        if (g_lw_last_items.size() > 512 &&
            g_lw_last_items.find(item.ctx) == g_lw_last_items.end()) {
          continue;  // growth backstop
        }
        LwRetained& r = g_lw_last_items[item.ctx];
        r.item = item;
        r.frame = now;
      }
    } else if (!g_lw_last_items.empty()) {
      g_lw_last_items.clear();
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
  // Publish-gap telemetry ("skater flickered invisible for a moment"): a
  // skinned/ropa/character mesh that published recently, vanished for 1-3
  // built frames, and came back. Rate-limited log names the mesh and gap so
  // the next flicker sighting is diagnosable from the log alone.
  {
    static std::unordered_map<uint32_t, uint64_t> s_last_pub;
    static uint64_t s_pub_frame = 0;
    ++s_pub_frame;
    if (s_last_pub.size() > 4096) {
      s_last_pub.clear();
    }
    for (const DrawItem& item : scene.items) {
      if (!(item.ropa || item.char_family != 0 ||
            (item.skinned && !item.bones.empty()))) {
        continue;
      }
      auto [it, fresh] = s_last_pub.try_emplace(item.mesh, s_pub_frame);
      if (fresh) {
        continue;
      }
      const uint64_t gap = s_pub_frame - it->second;
      it->second = s_pub_frame;
      if (gap >= 2 && gap <= 4) {
        g_dyn_gap.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> gap_logged{0};
        const uint32_t ln = gap_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 16 || (ln & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: dyn publish GAP mesh={:08X} missing {} frame(s) "
              "ropa={} skinned={} fam={} src={}",
              item.mesh, gap - 1, item.ropa ? 1 : 0, item.skinned ? 1 : 0,
              item.char_family, item.dbg_src);
        }
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
  // RAW guest view*proj, the pose the game culled its submissions with,
  // kept for the off-screen retention frustum tests below (smoothing
  // replaces scene.view_proj with the re-timed pose).
  float guest_vp[16];
  std::memcpy(guest_vp, scene.view_proj, sizeof(guest_vp));
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

  // Vehicle retention (see g_dyn_retained): re-publish the last live
  // capture of a view-culled vehicle for a short window. Runs BEFORE the
  // smoothing block on purpose; the re-published copy re-claims its pose
  // ring in InterpolateDynamicItems (position re-pairing) and renders the
  // ring's coherent history instead of a frozen step.
  if (REXCVAR_GET(skate3_native_render_scene_retain_offscreen) &&
      REXCVAR_GET(skate3_native_render_scene_dynamic_items) &&
      g_synpan_active.load(std::memory_order_relaxed) == 0) {
    // Covers the camera-smoothing lag only; vehicle pose data just ages.
    constexpr uint64_t kDynRetainFrames = 10;
    const uint64_t rnow = g_guest_frame;
    // World center from the palette: average the plausible bone
    // translations (junk rows past the real skeleton carry garbage; gate
    // on finiteness and distance to bone 0).
    const auto bone_center = [](const DrawItem& it, float out[3]) -> bool {
      const size_t nb = it.bones.size() / 12;
      if (nb == 0) {
        return false;
      }
      const float b0[3] = {it.bones[3], it.bones[7], it.bones[11]};
      double sum[3] = {0.0, 0.0, 0.0};
      int n = 0;
      for (size_t b = 0; b < nb; ++b) {
        const float t[3] = {it.bones[b * 12 + 3], it.bones[b * 12 + 7],
                            it.bones[b * 12 + 11]};
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) ||
            !std::isfinite(t[2])) {
          continue;
        }
        const float dx = t[0] - b0[0], dy = t[1] - b0[1], dz = t[2] - b0[2];
        if (dx * dx + dy * dy + dz * dz > 100.0f) {
          continue;  // > 10 m from bone 0: junk row
        }
        sum[0] += t[0];
        sum[1] += t[1];
        sum[2] += t[2];
        ++n;
      }
      if (n == 0) {
        return false;
      }
      out[0] = float(sum[0] / n);
      out[1] = float(sum[1] / n);
      out[2] = float(sum[2] / n);
      return true;
    };
    struct LivePos {
      uint32_t mesh;
      float p[3];
    };
    std::vector<LivePos> live;
    live.reserve(16);
    for (const DrawItem& it : scene.items) {
      if ((it.char_family != 6 && it.char_family != 7) || !it.skinned ||
          it.bones.empty() || it.pending || it.retained) {
        continue;
      }
      float p[3];
      if (!bone_center(it, p)) {
        continue;
      }
      live.push_back({it.mesh, {p[0], p[1], p[2]}});
      if (it.caster_bank) {
        continue;  // stale ortho pose: never store as the rescue state
      }
      DynRetained* slot = nullptr;
      for (DynRetained& r : g_dyn_retained) {
        const float dx = r.pos[0] - p[0], dy = r.pos[1] - p[1],
                    dz = r.pos[2] - p[2];
        if (r.item.mesh == it.mesh && dx * dx + dy * dy + dz * dz < 4.0f) {
          slot = &r;
          break;
        }
      }
      if (slot == nullptr) {
        if (g_dyn_retained.size() >= 64) {
          continue;
        }
        g_dyn_retained.emplace_back();
        slot = &g_dyn_retained.back();
      }
      slot->item = it;
      slot->item.retained = true;
      slot->item.selected = false;
      std::memcpy(slot->pos, p, sizeof(slot->pos));
      const float ex = it.bbox_max[0] - it.bbox_min[0];
      const float ey = it.bbox_max[1] - it.bbox_min[1];
      const float ez = it.bbox_max[2] - it.bbox_min[2];
      slot->half = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
      slot->last_seen = rnow;
    }
    for (size_t i = 0; i < g_dyn_retained.size();) {
      DynRetained& r = g_dyn_retained[i];
      if (r.last_seen == rnow) {
        ++i;
        continue;
      }
      bool keep = rnow - r.last_seen <= kDynRetainFrames;
      bool publish = keep;
      if (keep) {
        for (const LivePos& lp : live) {
          const float dx = lp.p[0] - r.pos[0], dy = lp.p[1] - r.pos[1],
                      dz = lp.p[2] - r.pos[2];
          if (lp.mesh == r.item.mesh && dx * dx + dy * dy + dz * dz < 36.0f) {
            // A live copy nearby (caster-only capture frame, or the 2 m
            // matcher missed a fast mover): keep the entry warm for the
            // frame the captures stop, but never double the vehicle.
            r.last_seen = rnow;
            publish = false;
            break;
          }
        }
      }
      if (publish) {
        // Unsubmitted + visible to the guest camera = really gone
        // (despawn); only view-culled vehicles re-publish.
        if (!BoxOutsideFrustum(r.pos, r.half, guest_vp, 0.97f)) {
          keep = false;
        }
      }
      if (!keep) {
        g_dyn_retained[i] = std::move(g_dyn_retained.back());
        g_dyn_retained.pop_back();
        continue;
      }
      if (publish) {
        scene.items.push_back(r.item);
        static std::atomic<uint64_t> s_dyn_retained_pub{0};
        const uint64_t n =
            s_dyn_retained_pub.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 1023u) == 0) {
          REXLOG_INFO(
              "native-scene: vehicle retention re-publish mesh={:08X} fam={} "
              "age_frames={} (n={})",
              r.item.mesh, r.item.char_family, rnow - r.last_seen, n);
        }
      }
      ++i;
    }
  }

  // (Moved BEFORE the smoothing block: the interp ring's pose ingestion
  // pairs each pose with g_ropa_last_seq; the shape snapshot of the SAME
  // frame must be enqueued first or every pose pairs with the previous
  // frame's shape, a constant one-period drape lag. The snapshot also now
  // reads RAW captured items, matching the raw VB it snapshots.)
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
      if (item.ropa && REXCVAR_GET(skate3_native_render_scene_ropa_inline)) {
        continue;  // ropa decodes inline on the render thread (zero-lag)
      }
      if (item.ropa && item.dbg_src == 4) {
        // Rescued ropa re-publishes LAST frame's resolved state (mode +
        // palette + world); decoding THIS frame's payload under it mixes
        // frames, at a sim flip that is the ribbon. Keep the previous
        // decode on the GPU: a fully coherent N-1 garment (the draw path
        // tolerates the fingerprint mismatch for dynamic payloads).
        continue;
      }
      // The decode is MODE-dependent for ropa (rigid decodes zero the blend
      // weight/index attributes, see DecodeMesh), so a mode flip must
      // re-enqueue even when the payload bytes did not change: fold the
      // resolved mode into the dedup key.
      const uint64_t fp_key =
          item.fingerprint ^
          ((item.ropa && item.skinned && !item.bones.empty()) ? 1u : 0u);
      const auto prev = s_dyn_fp_sent.find(item.mesh);
      const bool first_sight = prev == s_dyn_fp_sent.end();
      const uint64_t prev_fp = first_sight ? 0 : prev->second;
      if (!first_sight && prev_fp == fp_key) {
        continue;
      }
      DynDecodeJob job;
      job.item = item;
      job.item.bones.clear();
      job.seq = ++s_dyn_seq;
      job.vb.resize(item.vb_bytes);
      if (!GuestTryCopy(job.vb.data(), base + item.vb_addr, item.vb_bytes)) {
        continue;
      }
      if (item.ropa && item.skinned && !item.bones.empty() &&
          !RopaPayloadCoherent(item, job.vb)) {
        // The cloth sim rewrote this VB between the draw-time capture and
        // this frame-end snapshot (mode flip in flight): decoding it under
        // the skinned palette is the mangled ribbon. Keep the old decode
        // this frame; next frame's capture sees the flipped flag and
        // resolves mode and payload together.
        g_ropa_mismatch.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      job.ib.resize(size_t(item.ib_count) * 2);
      if (!GuestTryCopy(job.ib.data(), base + item.ib_addr, job.ib.size())) {
        continue;
      }
      s_dyn_fp_sent[item.mesh] = item.fingerprint;
      if (item.ropa) {
        // The pose <-> shape pairing key (see DynPose::shape_seq). Recorded
        // at CREATION (the delay queue below postpones submission, not
        // identity).
        g_ropa_last_seq[item.mesh] = job.seq;
        if (g_ropa_last_seq.size() > 256) {
          g_ropa_last_seq.clear();
        }
      }
      jobs.push_back(std::move(job));
    }
    // ROPA phase alignment (skate3_native_render_scene_ropa_delay): the
    // body renders on the motion-smoothing play clock, ~2 guest periods
    // behind now; a cloth snapshot submitted immediately decodes into a
    // shape ~2 frames AHEAD of the rendered body (the drape hangs where
    // the body WILL be; jelly / clip-through on direction changes; the
    // console pairs body N with shape N). Ropa snapshots pass through a
    // small per-mesh delay queue so the committed shape lands on the same
    // clock as the interpolated body.
    {
      const int32_t delay = REXCVAR_GET(skate3_native_render_scene_ropa_delay);
      static std::unordered_map<uint32_t, std::deque<DynDecodeJob>> s_ropa_delay;
      if (delay > 0) {
        std::vector<DynDecodeJob> ready;
        ready.reserve(jobs.size());
        for (DynDecodeJob& j : jobs) {
          if (!j.item.ropa) {
            ready.push_back(std::move(j));
            continue;
          }
          auto& q = s_ropa_delay[j.item.mesh];
          q.push_back(std::move(j));
          while (q.size() > size_t(delay)) {
            ready.push_back(std::move(q.front()));
            q.pop_front();
          }
        }
        jobs.swap(ready);
        if (s_ropa_delay.size() > 256) {
          s_ropa_delay.clear();  // outfit-change growth backstop
        }
      } else if (!s_ropa_delay.empty()) {
        s_ropa_delay.clear();
      }
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

  // LivingWorld entity store stamp: map
  // each character-family item's MeshContext to its owning LW entity and
  // stamp the entity's authoritative opacity + per-instance identity. Runs
  // after every dyn publish path (captures, merges, rescues, vehicle
  // retention) and before the smoothing block so the pose rings can key by
  // identity. Items with no fresh store entry (player skater - a different
  // fade system, CAC entities, despawned/retained leftovers) keep the
  // captured-row behavior untouched.
  if (REXCVAR_GET(skate3_native_render_scene_lw_fade) ||
      REXCVAR_GET(skate3_native_render_scene_lw_identity)) {
    // Fade serving also honors the master entity-fade switch: with it off
    // the routing ignores fades entirely, so the alpha row must keep its
    // captured value (the shader still reads it on some paths).
    const bool serve_fade =
        REXCVAR_GET(skate3_native_render_scene_lw_fade) &&
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    const bool serve_id = REXCVAR_GET(skate3_native_render_scene_lw_identity);
    for (DrawItem& item : scene.items) {
      if (item.char_family == 0 || item.ctx == 0) {
        continue;
      }
      float alpha = 1.0f;
      uint32_t entity = 0;
      if (!skate3::native_lw::LookupLwCtx(item.ctx, &alpha, &entity)) {
        continue;
      }
      g_lw_stamped.fetch_add(1, std::memory_order_relaxed);
      if (serve_id) {
        item.lw_entity = entity;
      }
      // Per-ctx lighting/paint rows (see g_char_rows_cache_ctx): refresh
      // from validated captures, serve on capture-failed frames, entity-
      // checked so a recycled instance never inherits foreign paint. This
      // keeps edge-of-view vehicles (caster-only capture stretches, every
      // read rejected) on their own shading instead of legacy flat.
      if (item.char_rows[14 * 4 + 1] > 0.0f) {
        if (g_char_rows_cache_ctx.size() > 4096) {
          g_char_rows_cache_ctx.clear();
        }
        CharRowsCtx& cr = g_char_rows_cache_ctx[item.ctx];
        std::memcpy(cr.rows.data(), item.char_rows, sizeof(item.char_rows));
        cr.entity = entity;
      } else {
        const auto rit = g_char_rows_cache_ctx.find(item.ctx);
        if (rit != g_char_rows_cache_ctx.end() &&
            rit->second.entity == entity) {
          std::memcpy(item.char_rows, rit->second.rows.data(),
                      sizeof(item.char_rows));
          g_lw_rows_served.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // Authoritative palette for every vehicle pose that did NOT come
      // from a fresh perspective capture: caster-only frames (ortho banks
      // = GUESSED palette base + ~40 ms-stale animation, the edge-of-view
      // mangle class), retention re-publishes (last capture aging up to 10
      // frames; at driving speed a stale republish renders a GHOST
      // meters BEHIND the live position right after the vehicle exits the
      // view; a garbage last capture makes the ghost sideways), rescues
      // and gap fills. The entity's OWN packed palette (m_matrices,
      // written by the pack writer this sim tick) is its true current
      // pose: a genuinely-exited vehicle lands off-screen (no ghost), a
      // pan-trailing retention case lands exactly right.
      if (item.skinned && !item.ropa &&
          (item.char_family == 6 || item.char_family == 7) &&
          (item.caster_bank || item.retained || item.dbg_src == 3 ||
           item.dbg_src == 6 || item.dbg_src == 9) &&
          !item.bones.empty() &&
          REXCVAR_GET(skate3_native_render_scene_lw_palette)) {
        float rows[96 * 12];
        const uint32_t n =
            skate3::native_lw::LookupLwPalette(item.ctx, rows, 96 * 12);
        if (n != 0 && size_t(n) * 12 <= item.bones.size()) {
          std::memcpy(item.bones.data(), rows, size_t(n) * 12 * sizeof(float));
          item.caster_bank = false;
          item.dbg_src = 10;  // lw palette substitution
          g_lw_pal_sub.fetch_add(1, std::memory_order_relaxed);
          static std::atomic<uint32_t> s_sub_logged{0};
          const uint32_t ln =
              s_sub_logged.fetch_add(1, std::memory_order_relaxed);
          if (ln < 16 || (ln & 511u) == 0) {
            REXLOG_INFO(
                "native-scene: LW palette substituted ctx={:08X} mesh={:08X} "
                "fam={} rows={} (n={})",
                item.ctx, item.mesh, item.char_family, n, ln);
          }
        }
      }
      if (serve_fade) {
        item.lw_alpha = alpha;
        // The exact shading path reads the alpha ROW (cbuffer CH row 14.x)
        // - overwrite it with the entity value for the families where that
        // row IS the raw entity fade on console (c13.x / c21.x / c22.x /
        // c20.x). Hair (strand-scale composed) and vehicle glass
        // (tint-composed) keep their captured rows; CharFadeAlpha bounds
        // them by the entity alpha instead.
        if (item.char_rows[14 * 4 + 1] > 0.0f &&
            (item.char_family == 1 || item.char_family == 2 ||
             item.char_family == 3 || item.char_family == 6)) {
          item.char_rows[14 * 4 + 0] = std::clamp(alpha, 0.0f, 1.0f);
        }
      }
    }
  }

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
    // Auto-armed bone-signal recordings (diagnosis, see the cvar): first
    // window ~30 s after the scene comes up, re-armed every 90 s, 3 max.
    {
      const double auto_s = REXCVAR_GET(skate3_native_render_scene_bonesig_auto);
      if (auto_s > 0.0) {
        static int s_auto_count = 0;
        static double s_auto_next = 0.0;
        if (s_auto_next == 0.0) {
          s_auto_next = now_s + 30.0;
        } else if (s_auto_count < 3 && now_s >= s_auto_next) {
          ++s_auto_count;
          s_auto_next = now_s + 90.0;
          REXLOG_INFO("native-scene: auto bone-signal recording {} ({} s)",
                      s_auto_count, auto_s);
          RecordBoneSignal(std::min(auto_s, 30.0));
        }
      }
    }
    // Walking-vehicle detector (diagnosis): a livingworld_vehicles item
    // whose bone-0 translation sits on top of a character item's bone-0 is
    // the "player becomes the vehicle" bug. Log the coincidence WITH the
    // first bone rows of both palettes: identical rows = the same bank was
    // captured for both (a capture-attribution bug); distinct rows = the
    // vehicle's own palette tracks the character (a different mechanism).
    {
      static double s_last_attach_log = 0.0;
      if (now_s - s_last_attach_log > 2.0) {
        bool logged = false;
        for (const DrawItem& v : scene.items) {
          if ((v.char_family != 6 && v.char_family != 7) || !v.skinned ||
              v.bones.size() < 12) {
            continue;
          }
          for (const DrawItem& c : scene.items) {
            if (&c == &v || !c.skinned || c.bones.size() < 12 ||
                c.char_family == 0 || c.char_family >= 6) {
              continue;
            }
            const float dx = v.bones[3] - c.bones[3];
            const float dy = v.bones[7] - c.bones[7];
            const float dz = v.bones[11] - c.bones[11];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 4.0f) {
              const bool same_rows =
                  std::memcmp(v.bones.data(), c.bones.data(),
                              12 * sizeof(float)) == 0;
              REXLOG_INFO(
                  "native-scene ATTACH: vehicle mesh={:08X} fam={} src={} "
                  "pend={} d={:.2f} char mesh={:08X} fam={} src={} "
                  "same_bone0={} v_r0=({:.3f},{:.3f},{:.3f},{:.2f}) "
                  "c_r0=({:.3f},{:.3f},{:.3f},{:.2f})",
                  v.mesh, v.char_family, v.dbg_src, v.pending, std::sqrt(d2),
                  c.mesh, c.char_family, c.dbg_src, same_rows, v.bones[0],
                  v.bones[1], v.bones[2], v.bones[3], c.bones[0], c.bones[1],
                  c.bones[2], c.bones[3]);
              s_last_attach_log = now_s;
              logged = true;
              break;
            }
          }
          if (logged) {
            break;
          }
        }
      }
    }
    float smooth_vp[16], smooth_cam[3];
    if (SmoothCamera(cam_view, proj, scene.view_proj, scene.cam_pos, now_s, smooth_vp,
                     smooth_cam)) {
      std::memcpy(scene.view_proj, smooth_vp, sizeof(smooth_vp));
      std::memcpy(scene.cam_pos, smooth_cam, sizeof(smooth_cam));
      // Keep the skater/NPCs/props in phase with the smoothed camera:
      // interpolate their palettes/worlds at the same playback time.
      InterpolateDynamicItems(base, scene, now_s);
    }
  }

  // Off-screen retention (see g_retained_items): re-append statics the game
  // view-culled this frame while the trailing rendered pose can still see
  // them. Runs AFTER the smoothing block so retained copies never enter the
  // dynamic pose histories, and stands down while the synthetic-pan probe
  // maintains its own full-surround union.
  if (REXCVAR_GET(skate3_native_render_scene_retain_offscreen) &&
      g_synpan_active.load(std::memory_order_relaxed) == 0) {
    if (g_retained_clear.exchange(false, std::memory_order_relaxed)) {
      g_retained_items.clear();
      g_dyn_retained.clear();
    }
    const uint64_t now = g_guest_frame;
    std::unordered_set<uint64_t> submitted;
    const size_t published = scene.items.size();
    submitted.reserve(published);
    for (size_t i = 0; i < published; ++i) {
      const DrawItem& it = scene.items[i];
      if (it.skinned || it.cloth_quads || it.ropa || it.pending ||
          !it.bones.empty()) {
        continue;
      }
      const uint64_t key = SynPanItemKey(it);
      submitted.insert(key);
      if (g_retained_items.size() >= 20000 &&
          g_retained_items.find(key) == g_retained_items.end()) {
        continue;  // growth backstop
      }
      auto [slot, inserted] = g_retained_items.try_emplace(key);
      slot->second.last_seen = now;
      // Copy the item core on first sight and whenever its payload identity
      // or transform moved on; a retained copy with a stale fingerprint
      // skips at draw (see draw_item's retained gate) and would re-tear.
      if (inserted || slot->second.item.fingerprint != it.fingerprint ||
          std::memcmp(slot->second.item.world, it.world, sizeof(it.world)) != 0) {
        slot->second.item = it;
        slot->second.item.retained = true;
        slot->second.item.selected = false;
      }
    }
    // TTL bounds how long a never-resubmitted entry lives (streaming reuses
    // arena addresses; the render side additionally fingerprint-gates
    // retained draws). ~90 guest frames = 0.6-1.5 s across fps caps,
    // far beyond the smoothing lag it needs to cover.
    constexpr uint64_t kRetainTtlFrames = 90;
    for (auto rit = g_retained_items.begin(); rit != g_retained_items.end();) {
      if (submitted.find(rit->first) != submitted.end()) {
        ++rit;
        continue;
      }
      const RetainedItem& r = rit->second;
      // Unsubmitted + visible to the guest camera = the game really removed
      // it. The 0.97 margin shrinks the tested frustum so bounds poking
      // just inside an edge still count as view-culled.
      if (now - r.last_seen > kRetainTtlFrames ||
          !ItemOutsideFrustum(r.item, guest_vp, 0.97f)) {
        rit = g_retained_items.erase(rit);
        continue;
      }
      // Draw it only if the RENDERED pose can actually see it (widened
      // frustum: only clearly-outside skips); after a fast 180 the trail
      // behind the camera stays retained but costs nothing.
      if (!ItemOutsideFrustum(r.item, scene.view_proj, 1.05f)) {
        scene.items.push_back(r.item);
      }
      ++rit;
    }
  }

  // Camera-signal recorder: per-frame raw + smoothed heading while the
  // window is open; write + reset when it closes (CamSigFrameTick).
  if (g_camsig_deadline.load(std::memory_order_relaxed) > 0.0) {
    const double rec_now =
        std::chrono::duration<double>(build_t0.time_since_epoch()).count();
    float rot[16] = {};
    const float* smoothed = nullptr;
    if (g_smooth_active) {
      ViewRotFromQuat(g_smooth_pose.q, rot);
      smoothed = rot;
    }
    CamSigFrameTick(rec_now, cam_view, smoothed, g_smooth_play);
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
  if (g_sky_sun_have) {
    std::memcpy(scene.sky_sun, g_sky_sun, sizeof(g_sky_sun));
    scene.sky_sun_valid = true;
  }
  const bool blur_active = g_ui_blur_seen || g_ui_blur_hold > 0;
  scene.ui_blur = blur_active ? g_ui_blur : 0.0f;
  std::memcpy(scene.ui_blur_color, g_ui_blur_color, sizeof(scene.ui_blur_color));
  if (g_ui_blur_seen) {
    g_ui_blur_hold = 2;
  } else if (g_ui_blur_hold > 0) {
    --g_ui_blur_hold;
  }
  {
    static bool s_blur_was_active = false;
    if (blur_active != s_blur_was_active) {
      REXLOG_INFO(
          "native-scene: popup background blur {} (kernel scale {:.1f}, fade "
          "{:.2f}/{:.2f}/{:.2f})",
          blur_active ? "ON" : "off", g_ui_blur, g_ui_blur_color[0],
          g_ui_blur_color[1], g_ui_blur_color[2]);
      s_blur_was_active = blur_active;
      if (!blur_active) {
        // Don't carry one popup's fade into the next popup's first frame
        // if its own c1 read misses the coherence gate.
        g_ui_blur_color[0] = g_ui_blur_color[1] = g_ui_blur_color[2] = 1.0f;
      }
    }
  }
  g_ui_blur_seen = false;
  // Photo-editor postfx: publish the live pass captures when the photo
  // EDITOR itself is up (not the wider TakePhoto readback window: the
  // chain must never run over ordinary gameplay frames) and every pass has
  // fresh rows (the game stages its postfx constants each frame regardless
  // of draw suppression). The vignette and grain fetch words come from the
  // fisheye/uber fetch-shadow snapshots (slot 2 / slot 6 per the
  // ring-verified bindings).
  if (uint8_t* pfx_base = g_guest_base.load(std::memory_order_relaxed);
      pfx_base != nullptr && g_photo_flow_frame.load(std::memory_order_relaxed) &&
      PhotoEditorSignal(pfx_base) != nullptr) {
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerfClock::now().time_since_epoch())
                               .count();
    bool fresh = true;
    for (int p = 0; p < kPfxPassCount; ++p) {
      if (g_pfx_cap[p].ps_ns == 0 || now_ns - g_pfx_cap[p].ps_ns > 1'000'000'000 ||
          !g_pfx_cap[p].vs_seen) {
        fresh = false;
      }
    }
    if (!fresh) {
      // Name the missing pass(es), once per ~2 s while in the window.
      static int64_t s_diag_ns = 0;
      if (now_ns - s_diag_ns > 2'000'000'000) {
        s_diag_ns = now_ns;
        char buf[160];
        int off = 0;
        for (int p = 0; p < kPfxPassCount; ++p) {
          const int64_t age_ms =
              g_pfx_cap[p].ps_ns == 0 ? -1 : (now_ns - g_pfx_cap[p].ps_ns) / 1'000'000;
          off += std::snprintf(buf + off, sizeof(buf) - off, " p%d[ps%lldms vs%d]", p,
                               (long long)age_ms, g_pfx_cap[p].vs_seen ? 1 : 0);
          if (off >= int(sizeof(buf)) - 24) break;
        }
        REXLOG_INFO("native-scene: pfx captures NOT fresh:{}", buf);
      }
    }
    if (fresh) {
      scene.photo_fx.valid = true;
      for (int p = 0; p < kPfxPassCount; ++p) {
        std::memcpy(scene.photo_fx.ps[p], g_pfx_cap[p].ps,
                    sizeof(scene.photo_fx.ps[p]));
        std::memcpy(scene.photo_fx.vs[p], g_pfx_cap[p].vs,
                    sizeof(scene.photo_fx.vs[p]));
      }
      std::memcpy(scene.photo_fx.vignette_fetch, g_pfx_cap[kPfxFisheye].fetch[2],
                  sizeof(scene.photo_fx.vignette_fetch));
      std::memcpy(scene.photo_fx.grain_fetch, g_pfx_cap[kPfxUber].fetch[6],
                  sizeof(scene.photo_fx.grain_fetch));
      static bool s_pfx_logged = false;
      if (!s_pfx_logged) {
        s_pfx_logged = true;
        REXLOG_INFO(
            "native-scene: photo postfx captures LIVE (visualfx c0=({:.3f},{:.3f},"
            "{:.3f},{:.3f}) dof c0.x={:.4f} uber c5=({:.3f},{:.3f},{:.3f},{:.3f}) "
            "fisheye c1=({:.3f},{:.3f},{:.3f}) vignette=[{:08X} {:08X}] "
            "grain=[{:08X} {:08X}])",
            g_pfx_cap[kPfxVisualFx].ps[0][0], g_pfx_cap[kPfxVisualFx].ps[0][1],
            g_pfx_cap[kPfxVisualFx].ps[0][2], g_pfx_cap[kPfxVisualFx].ps[0][3],
            g_pfx_cap[kPfxDof].ps[0][0], g_pfx_cap[kPfxUber].ps[5][0],
            g_pfx_cap[kPfxUber].ps[5][1], g_pfx_cap[kPfxUber].ps[5][2],
            g_pfx_cap[kPfxUber].ps[5][3], g_pfx_cap[kPfxFisheye].ps[1][0],
            g_pfx_cap[kPfxFisheye].ps[1][1], g_pfx_cap[kPfxFisheye].ps[1][2],
            scene.photo_fx.vignette_fetch[0], scene.photo_fx.vignette_fetch[1],
            scene.photo_fx.grain_fetch[0], scene.photo_fx.grain_fetch[1]);
      }
    }
  }
  std::memcpy(g_fog_cam, scene.cam_pos, sizeof(g_fog_cam));
  g_fog_frame_done = false;
  g_shadow_frame_done = false;
  g_sky_frame_done = false;
  g_tree_frame_done = false;
  g_proxy_frame_done = false;
  g_dynobj_frame_done = false;


  // Ropa mode-flip HOLD (must run AFTER the dyn-job enqueue above so the
  // new mode's decode is already in flight with the fresh capture): the
  // GPU-resident decode still pairs with the OTHER mode; drawing the new
  // mode over it is the 1-2 frame ribbon (skinned palette over sim-deformed
  // verts) or a collapsed garment (rigid decodes zero the blend attributes;
  // skinning them lands every vert at the origin). Re-publish the pre-flip
  // state until the flipped decode commits and g_ropa_resident catches up;
  // with no pre-flip state to hold (clones: the cache is single-instance)
  // drop the garment for the flip frames: a blink beats the ribbon.
  if (REXCVAR_GET(skate3_native_render_scene_dynamic_items)) {
    for (DrawItem& item : scene.items) {
      if (!item.ropa || item.pending || item.dbg_src == 4 ||
          item.fingerprint == 0 || item.draws.empty()) {
        continue;
      }
      const bool mode_skinned = item.skinned && !item.bones.empty();
      RopaResidentDecode res;
      {
        std::lock_guard<std::mutex> lock(g_ropa_resident_mutex);
        const auto rit = g_ropa_resident.find(item.mesh);
        if (rit == g_ropa_resident.end()) {
          continue;  // nothing resident yet: the draw defers until commit
        }
        res = rit->second;
      }
      if (res.skinned == mode_skinned) {
        continue;  // resident decode pairs with the published mode
      }
      const auto pit = ropa_flip_prev.find(item.mesh);
      if (pit != ropa_flip_prev.end() && pit->second.skinned == res.skinned) {
        item.skinned = pit->second.skinned;
        item.bones = pit->second.bones;
        std::memcpy(item.world, pit->second.world, sizeof(item.world));
        item.dbg_src = 5;
        // Keep the cache on the HELD state so a multi-frame hold (decode
        // still in flight next frame) re-detects the flip and re-stashes.
        if (g_ropa_state_cache.size() < 512) {
          g_ropa_state_cache[item.mesh] = pit->second;
        }
      } else {
        item.draws.clear();
      }
      g_ropa_hold.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Draw-time STRETCH VETO: the last line of defense, judging what the GPU
  // will ACTUALLY draw: the RESIDENT decode's sample verts (g_skin_probe,
  // cached by DecodeMesh) skinned with the FINAL palette (after the merge,
  // rescues, interpolation and flip-hold above). Every upstream gate judges
  // the LIVE guest VB, so a decode-content/palette pairing mismatch, or
  // junk introduced by an interpolation substitution, passes all of them
  // and still flashes the 1-frame map-length ribbon (every ropa[] counter
  // stayed clean in testing). Trip: clear the item's draws (a blink,
  // and the caster pass shares the item so no ribbon shadow either), log,
  // and dump palette + samples for offline diagnosis.
  if (REXCVAR_GET(skate3_native_render_scene_stretch_guard)) {
    for (DrawItem& item : scene.items) {
      if (!item.skinned || item.bones.size() < 12 || item.pending ||
          item.draws.empty()) {
        continue;
      }
      SkinProbe probe;
      {
        std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
        const auto pit = g_skin_probe.find(item.mesh);
        if (pit == g_skin_probe.end()) {
          continue;  // not decoded yet: nothing will be drawn either
        }
        probe = pit->second;
      }
      const float bind_diag = BindDiag(item);
      // 6x like PublishedPaletteSane: judging accepted palettes, where junk
      // measures 100-400x and legit small-mesh articulation reaches ~3.3x.
      const float max_spread = std::max(6.0f * bind_diag, bind_diag + 2.0f);
      // Decoded-buffer convention: component k = byte k (see DecodeMesh's
      // SwapU32 + per-byte extraction); unpack into the shared sample form.
      std::vector<SkinSampleVert> sverts(probe.s.size());
      for (size_t i = 0; i < probe.s.size(); ++i) {
        const SkinProbeSample& ps = probe.s[i];
        SkinSampleVert& sv = sverts[i];
        sv.p[0] = ps.p[0];
        sv.p[1] = ps.p[1];
        sv.p[2] = ps.p[2];
        sv.pos_finite = true;
        for (int k = 0; k < 4; ++k) {
          sv.w[k] = uint8_t((ps.bw >> (8 * k)) & 0xFF);
          sv.bone[k] = uint8_t((ps.bi >> (8 * k)) & 0xFF);
        }
      }
      float spread = 0.0f;
      int n = 0;
      if (SkinnedSpreadHostRows(sverts.data(), uint32_t(sverts.size()),
                                item.bones.data(), item.bones.size(), /*min_n=*/4,
                                /*garbage_fails=*/false, &spread, &n) != 1) {
        continue;
      }
      if (spread <= max_spread) {
        continue;
      }
      g_stretch_veto.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_stretch_logged{0};
      const uint32_t ln =
          s_stretch_logged.fetch_add(1, std::memory_order_relaxed);
      if (ln < 24 || (ln & 255u) == 0) {
        REXLOG_INFO(
            "native-scene: STRETCH veto mesh={:08X} fam={} ropa={} src={} "
            "caster={} spread={:.1f} bind={:.2f} n={} fp_match={} "
            "bone0_t=({:.1f},{:.1f},{:.1f})",
            item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
            item.caster_bank ? 1 : 0, spread, bind_diag, n,
            probe.fp == item.fingerprint ? 1 : 0, item.bones[3],
            item.bones[7], item.bones[11]);
      }
      if (ln < 6) {
        // Full palette + probe dump for offline diagnosis (which rows are
        // junk, which bones the stretched samples weight to).
        char path[128];
        std::snprintf(path, sizeof(path), "logs/stretch_%u_%08X.txt", ln,
                      item.mesh);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fprintf(f,
                       "mesh=%08X fam=%u ropa=%d src=%u caster=%d spread=%f "
                       "bind=%f fp=%016llX probe_fp=%016llX vb=%08X\n",
                       item.mesh, item.char_family, item.ropa ? 1 : 0,
                       item.dbg_src, item.caster_bank ? 1 : 0, spread,
                       bind_diag, (unsigned long long)item.fingerprint,
                       (unsigned long long)probe.fp, item.vb_addr);
          const uint32_t bones = uint32_t(item.bones.size() / 12);
          for (uint32_t b = 0; b < bones; ++b) {
            const float* r = item.bones.data() + b * 12;
            std::fprintf(f,
                         "bone %3u | %9.4f %9.4f %9.4f %10.3f | %9.4f %9.4f "
                         "%9.4f %10.3f | %9.4f %9.4f %9.4f %10.3f\n",
                         b, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                         r[8], r[9], r[10], r[11]);
          }
          for (const SkinProbeSample& ps : probe.s) {
            std::fprintf(f, "vert p=(%f,%f,%f) bw=%08X bi=%08X\n", ps.p[0],
                         ps.p[1], ps.p[2], ps.bw, ps.bi);
          }
          std::fclose(f);
        }
      }
      item.draws.clear();
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

  // v3 tail: shadow-mode compares (Phase 2 observers) and the shadow-mat
  // readers. Timed as one block (`v3=` in the perf line); this tail is on
  // the guest render thread, and its spikes were the pan stutter.
  {
    const auto v3_t0 = PerfClock::now();
    skate3::native_v3::OnFrameBuilt(base, records, count, scene);
    skate3::native_v3_mat::OnFrameBuilt(base, records, count, scene);
    g_pw_v3.Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now() - v3_t0)
                             .count()));
  }

  // AUX-publish gate: the skater-portrait render-to-texture passes submit a
  // real perspective SceneRenderView, and on the frontend menu screens
  // (team boxes, Import Skater) it is the ONLY publisher; publishing it
  // rendered the portrait FULL SCREEN behind the menu for the whole
  // pause-freshness window on every entry/scroll (flapping
  // pause-native <-> loading in 300 ms cycles). Its projection is
  // screen-shaped (the aspect gate at view selection above never fired), so
  // identify it by CONTENT: a menu-context (presence 0) scene consisting of
  // nothing but character-family pieces (skater + board, every material
  // "character.*") is a portrait pass, never the visible frame. Real menu
  // backdrops always carry world geometry (the pause plaza ~700 items, the
  // CAS editor room has env-family walls). Skipped publishes also skip the
  // freshness stamp, so the mode never flips.
  // CAS-editor exemption: the STARTUP-flow editor's scene is skater-only
  // (no garage world pre-gameplay: 10 all-char items, exactly a portrait
  // pass's shape; without the exemption this gate ate it, 3D black
  // behind a live menu). The editor's _nis shader heartbeat separates the
  // two: editor draws stamp it every frame, portrait passes use the
  // _default compiles and never do.
  if (rex::graphics::ultrawide_debug::Skate3GameplayContextValue() == 0 &&
      !scene.items.empty() && scene.items.size() <= 48 &&
      !CasEditorActive(base)) {
    bool all_char = true;
    for (const DrawItem& it : scene.items) {
      if (it.char_family == 0) {
        all_char = false;
        break;
      }
    }
    if (all_char) {
      static std::atomic<uint32_t> s_aux_pub{0};
      const uint32_t n = s_aux_pub.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n & 255u) == 0) {
        const float m00 = LoadGuestF32(base, viewcam + 0x60 + 0 * 4);
        const float m11 = LoadGuestF32(base, viewcam + 0x60 + (1 * 4 + 1) * 4);
        REXLOG_INFO(
            "native-scene: portrait-pass publish skipped ({} char items, cam "
            "({:.1f},{:.1f},{:.1f}), proj m00={:.3f} m11={:.3f}) (n={})",
            scene.items.size(), scene.cam_pos[0], scene.cam_pos[1],
            scene.cam_pos[2], m00, m11, n);
      }
      return;
    }
  }
  g_last_publish_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count(),
                          std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_scene_mutex);
  scene.generation = ++g_generation;
  g_scene = std::make_shared<const FrameScene>(std::move(scene));
}

// StartRecording / WriteRecording: native/skate3_native_diagnostics.cpp.

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
  // ROPA only: the decoded vertex array (num_verts x 14 floats, the scene
  // VS layout) retained for draw-time shape blending onto the play clock.
  std::vector<float> ropa_verts;
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
  // Consecutive failed decodes of this entry (payload still streaming in):
  // drives the escalating retry backoff at commit; the first failure
  // retries fast (the payload usually lands within a few frames; a fixed
  // +120 held freshly streamed textures white for ~half a second), repeat
  // failures back off toward the old cadence.
  uint8_t fail_count = 0;
  // Successful payload rechecks so far: fresh entries re-verify fast
  // (2/4/8 frames; a mid-stream garbage decode otherwise stays on screen
  // up to the full recheck interval) before settling at the 16-frame
  // steady-state cadence.
  uint8_t recheck_count = 0;
  // Tiled-aware payload probes: absolute 0xA-mirror addresses of up to 16
  // of this texture's OWN mip-0 blocks (a 4x4 spread over the block grid,
  // resolved through the tiled address swizzle). The old contiguous
  // [payload_addr, payload_size) fingerprint was WRONG for streamed world
  // textures: they are pitch-packed into shared mip pools and tiled across
  // padded macro rows, so the range interleaved NEIGHBOR textures' bytes;
  // pool churn kept the fingerprint flapping, the commit-time stability
  // verify then rejected every legitimate heal, and freshly promoted mips
  // stayed stale/low-res for seconds (the medium-distance texture pop-in;
  // one promote re-logged 8x while its heals were refused).
  // probe_count == 0 falls back to the range fingerprint (cubes).
  // 8x8 = 64 probes: the first 4x4 = 16 grid was too sparse; a mid-stream
  // decode whose 16 probed blocks happened to be final (other regions still
  // garbage) COMMITTED and never healed, visible as a black/garbage decal
  // until the next words change replaced it ("decal flash and replace").
  uint32_t probe_addr[64] = {};
  uint8_t probe_count = 0;
  // SRV recipe (2D textures) so EXTRA views of this resource can be created
  // into paired descriptor slots (the fam 5/6 masks+normal 2-descriptor
  // table at t4/t5); descriptors can't be copied out of the shader-visible
  // heap, so pairs re-create views from the recipe instead.
  DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
  UINT srv_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  uint32_t srv_mips = 0;
  bool valid = false;
  // Store LRU clock: last frame this entry was served/touched. Superseded
  // words states (old mip levels, pre-demote detail sets) age out once
  // nothing routes to them.
  uint64_t last_used_frame = 0;
  // A tiled mip's padded macro-row copy faulted and fell back to the
  // reported size; blocks beyond it uploaded as ZERO (the half-black
  // banner mip: tiled addressing puts the image's bottom rows past
  // min_size). The entry serves (better than white) but keeps re-decoding
  // until a complete copy lands; the commit prefers complete decodes.
  bool incomplete = false;
  // Decode-time SampleProbeNearBlack verdict: the payload was near-uniform
  // black when this decode was taken (a lightmap page mid-compose). The
  // lightmap-slot resolve serves the white fallback instead (tint.r == 0 ->
  // the unshadowed-bright window, 59810af semantics) until a heal lands
  // real content; other slots ignore it (black diffuse content is legal).
  bool near_black = false;
  // Same-content confirmations of a near-black verdict (commit dedups of
  // forced re-decodes). The forcing covers ONE race, a decode reading a
  // page mid-compose while the fingerprint sampled the composed result,
  // so a few confirmations prove the content is genuinely uniform and the
  // forcing stops (the plain fp poll still heals a later in-place compose).
  // Unbounded forcing re-decoded permanently-uniform textures every poll
  // forever, each a discarded worker round trip.
  uint8_t nb_redecodes = 0;
  // Frame of the last decode that landed CHANGED content for this key
  // (stamped by the words-key commit and the inline 2D decode). Classifies
  // video-plane content changes as mid-playback (recent change -> serve the
  // stale frame while the worker re-decodes, keeps 30 fps cadence) vs a
  // playback-START edge (quiet for seconds -> the entry holds the PREVIOUS
  // video's last frame; serving it flashed old content at every video
  // boundary; hold the quad black until fresh content commits).
  uint64_t last_change_frame = 0;
};

// THE texture identity key: FNV-1a
// over the six fetch-constant words, console identity semantics for the
// content store. Object resolves, draw-time word bindings, 2D/HUD art, and
// the decode workers all key the same g_r.tex_store entries with it.
inline uint64_t FetchWordsKey(const uint32_t words[6]) {
  uint64_t key = 1469598103934665603ull;
  for (int k = 0; k < 6; ++k) {
    // Clamp modes (dword_0 bits 10-18, clamp_x/y/z) are SAMPLER state, not
    // content identity; the same texture object binds with different clamp
    // bits per draw (observed: one texture decoded twice under 01004802 vs
    // 01024802, bit 17), and keying on them decodes/stores every variant
    // separately. The store holds textures + SRVs only; samplers are
    // static in the replay pipelines, so clamp-variant entries are exact
    // duplicates. Sign bits stay: they select the host SRV format.
    key ^= k == 0 ? (words[k] & ~0x0007FC00u) : words[k];
    key *= 1099511628211ull;
  }
  return key;
}

// Base-level pixel area from stored fetch words (word 2 packs width-1 /
// height-1 in 13-bit fields), the material-detail downgrade compare key.
inline uint64_t FetchWordsArea(const uint32_t words[6]) {
  return uint64_t((words[2] & 0x1FFFu) + 1u) *
         uint64_t(((words[2] >> 13) & 0x1FFFu) + 1u);
}

// Escalating retry backoff (native frames) for failed texture decodes: the
// payload is usually mid-stream and lands within a few frames; the first
// retry is fast (a fixed +120 held freshly streamed textures white for
// ~half a second); repeated failures back off toward the old cadence.
inline uint64_t RetryBackoff(uint8_t fails) {
  const uint32_t n = fails > 0 ? uint32_t(fails) - 1 : 0u;
  return std::min<uint64_t>(120, 8ull << std::min(n, 4u));
}
inline uint8_t BumpFail(uint8_t v) { return v < 250 ? uint8_t(v + 1) : v; }

// FNV-1a guest-payload fingerprint (SEH-guarded reads; streaming can
// decommit the range). Returns 0 only on unreadable payloads.
// Payloads up to 64 KB hash FULLY: the runtime-composed lightmap atlas
// pages (32 KB DXT1) receive REGIONAL in-place writes as the game composes
// late-streamed cells into a page we already decoded; the old 16-qword
// probe missed writes that fell between its samples, so the revalidation
// pass never re-decoded and the affected cells served the half-composed
// first decode forever (the 2x-bright canopy / dark awning reflective_trans
// glass: one texture, early-composed cells correct, late cells
// stale). Larger payloads (streamed assets whose fetch words change when
// content moves) keep a strided sample, densified 16 -> 64 qwords.
uint64_t SamplePayloadFingerprint(uint8_t* base, uint32_t addr, uint32_t size) {
  if (addr == 0 || size < 8) {
    return 0;
  }
  uint64_t h = 1469598103934665603ull;
  if (size <= 65536) {
    uint64_t buf[512];
    for (uint32_t off = 0; off < size; off += sizeof(buf)) {
      const uint32_t n = std::min<uint32_t>(sizeof(buf), (size - off) & ~7u);
      if (n == 0) {
        break;
      }
      if (!GuestTryCopy(buf, base + addr + off, n)) {
        return 0;
      }
      for (uint32_t k = 0; k < n / 8; ++k) {
        h = (h ^ buf[k]) * 1099511628211ull;
      }
    }
    return h;
  }
  for (uint32_t k = 0; k < 64; ++k) {
    const uint32_t off = uint32_t(uint64_t(size - 8) * k / 63u) & ~7u;
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + addr + off, sizeof(v))) {
      return 0;
    }
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// Heal-pipeline telemetry (see the 600-frame stats line): verify-fail =
// commit-time stability rejections, decode-fail = failed re-decodes of a
// still-serving entry, demote-hold = mip-0 demotes served from the cached
// full-chain decode without a doomed re-decode.
std::atomic<uint64_t> g_heal_verify_fail{0};
std::atomic<uint64_t> g_heal_decode_fail{0};
std::atomic<uint64_t> g_demote_hold{0};
// Sticky/skip serving (the anti-white-flash layer): sticky = draws served
// with the item's previous texture while a fresh object decodes; skipnew =
// first-sight draws suppressed entirely for the in-flight window.
std::atomic<uint64_t> g_tex_sticky_served{0};
std::atomic<uint64_t> g_skip_new{0};
// Per-mesh pipeline trace (skate3_native_render_scene_trace_mesh). Render
// thread only: the traced mesh address (parsed once per frame), the traced
// mesh's current store keys (so worker-commit events can be matched), and
// the last logged per-ctx state signature (summaries log on change).
uint32_t g_trace_mesh_addr = 0;
std::unordered_set<uint64_t> g_trace_keys;
std::unordered_map<uint32_t, uint64_t> g_trace_sig;
// Words-keyed (event-ad / streamed-artwork) serving: stale = site served
// its previous art while a new-words decode is in flight; none = nothing
// decoded for the site yet (caller shows the baked placeholder).
std::atomic<uint64_t> g_ad_stale_served{0};
std::atomic<uint64_t> g_ad_placeholder{0};

// Fill GuestTexture::probe_addr with up to 16 of the texture's own mip-0
// blocks: a 4x4 spread over the block grid, each resolved through the same
// tiled/linear addressing the decode uses, clamped to the guarded copy's
// readable range. Probes crossing into the padded macro-row tail are skipped
// rather than clamped; the tail belongs to pool neighbors.
void BuildPayloadProbes(const rex::graphics::TextureInfo& info, uint32_t mip0_addr,
                        uint32_t ox, uint32_t oy, uint32_t pitch_blocks,
                        uint32_t copy_size, GuestTexture& out) {
  out.probe_count = 0;
  const rex::graphics::FormatInfo* fi = info.format_info();
  const uint32_t bpb = fi->bytes_per_block();
  if (mip0_addr == 0 || bpb == 0 || (bpb & (bpb - 1)) != 0) {
    return;
  }
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(bpb));
  const uint32_t width = info.width + 1u;
  const uint32_t height = info.height + 1u;
  const uint32_t cols = (width + fi->block_width - 1) / fi->block_width;
  const uint32_t rows = (height + fi->block_height - 1) / fi->block_height;
  for (uint32_t gy = 0; gy < 8; ++gy) {
    for (uint32_t gx = 0; gx < 8; ++gx) {
      const uint32_t bx = cols > 1 ? gx * (cols - 1) / 7 : 0;
      const uint32_t by = rows > 1 ? gy * (rows - 1) / 7 : 0;
      uint32_t off;
      if (info.is_tiled) {
        off = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(bx + ox), int32_t(by + oy), pitch_blocks, bpb_log2));
      } else {
        off = ((by + oy) * pitch_blocks + bx + ox) * bpb;
      }
      if (off + bpb > copy_size) {
        continue;
      }
      out.probe_addr[out.probe_count++] = (0xA0000000u | mip0_addr) + off;
    }
  }
}

// Probe-based payload fingerprint: hashes one block-leading qword (or the
// whole block for narrow formats) at each probe address. Returns 0 only when
// a probe is unreadable: same semantics as the range fingerprint. Falls
// back to the range hash when no probes were built (cube maps).
uint64_t SampleProbeFingerprint(uint8_t* base, const GuestTexture& t) {
  if (t.probe_count == 0) {
    return SamplePayloadFingerprint(base, t.payload_addr, t.payload_size);
  }
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < t.probe_count; ++i) {
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + t.probe_addr[i], sizeof(v))) {
      return 0;
    }
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// Near-UNIFORM payload detector, sampled over the same probe grid at decode
// time. A COMPOSED lightmap page or weathering overlay is never one flat
// value across its whole mip-0 (a daylight bake / grime map atlases many
// surfaces), but a page decoded mid-compose is uniform fill: zeroed OR a
// flat grey (the reported "black/grey squares": a zero-only detector
// is structurally blind to grey). A real-but-garbage lightmap binds with
// tint.r > 0 so the CSM min-clamp renders BLACK (the door 59810af's
// white-fallback shader gate cannot see), and since 59810af a garbage macro
// multiplies OVER the decal paint. Consumers act on this only for the
// WHITE-NEUTRAL slots (lightmap/macro); uniform diffuse/spec content is
// legal and unaffected.
bool SampleProbeNearBlack(uint8_t* base, const GuestTexture& t) {
  if (t.probe_count < 16) {
    return false;  // range-fallback entries (cubes): not enough coverage
  }
  uint64_t a = 0, b = 0;
  bool have_a = false, have_b = false;
  for (uint32_t i = 0; i < t.probe_count; ++i) {
    uint64_t v = 0;
    if (!GuestTryCopy(&v, base + t.probe_addr[i], sizeof(v))) {
      return false;
    }
    if (!have_a) {
      a = v;
      have_a = true;
    } else if (v != a && !have_b) {
      b = v;
      have_b = true;
    } else if (v != a && v != b) {
      return false;  // 3+ distinct block values = real composed content
    }
  }
  return true;  // <= 2 distinct block values across the whole probe grid
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
  // Entity-fade variant of the transparent PSO: same straight alpha blend
  // but z-write ON. A fading character/vehicle is a solid object at partial
  // opacity; z-write-off blending composites every overlapping piece (skin
  // under clothes, far-side doors/wheels through the body shell) into an
  // x-ray. With depth writes the nearest surface wins and each pixel blends
  // once, matching the game's main-pass fade.
  ID3D12PipelineState* pso_fade = nullptr;
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
  // Native FMV: overlay-2D pipeline variant whose PS combines the movie
  // player's three CPU-filled YUV plane textures (ps_yuv2d; substituted for
  // the captured movie quad in the 2D replay, see OnMovieFrame).
  ID3D12PipelineState* pso_yuv2d = nullptr;
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
  // Photo-editor postfx chain (photo_fx.hlsl: exact ucode ports, see
  // FrameScene::PhotoFx). Own root signature: root CBV b0 (the pass's 256
  // captured/baked constant rows) + eight single-SRV tables t0..t7 + three
  // static samplers. Intermediates at the game's exact half/quarter sizes
  // (the DOF tap offsets are baked for them); the full-res stages run at
  // output resolution. pfx_quarter persists across frames (accumulation
  // input). All pfx color targets idle in RENDER_TARGET state.
  static constexpr uint32_t kPfxHalfW = 576, kPfxHalfH = 320;
  static constexpr uint32_t kPfxQuarterW = 288, kPfxQuarterH = 160;
  ID3D12RootSignature* pfx_root_sig = nullptr;
  // PSO order: depthpack, visualfx, dof_down, dof_mb, dof, uber, fisheye, blit.
  ID3D12PipelineState* pfx_pso[8] = {};
  ID3D12Resource* pfx_full[2] = {};   // output-res RGBA8 (visualfx out, uber out)
  ID3D12Resource* pfx_half[2] = {};   // 576x320 RGBA8
  ID3D12Resource* pfx_quarter = nullptr;  // 288x160 RGBA8 accumulation
  ID3D12Resource* pfx_depth = nullptr;    // output-res packed-depth RGBA8
  ID3D12Resource* pfx_lut = nullptr;      // 32^3 identity grade LUT (RGBA8)
  ID3D12Resource* pfx_lut_upload = nullptr;
  bool pfx_lut_uploaded = false;
  ID3D12Resource* pfx_cb = nullptr;  // upload heap, persistently mapped
  uint8_t* pfx_cb_ptr = nullptr;
  // Fixed SRV slots: 0/1 = pfx_full, 2/3 = pfx_half, 4 = quarter, 5 =
  // packed depth, 6 = LUT, 7 = the native MSAA/1x depth resource.
  uint32_t pfx_srv[8] = {};
  bool pfx_srv_allocated = false;
  uint32_t pfx_width = 0, pfx_height = 0;
  bool pfx_ready = false;
  bool pfx_failed = false;
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
  // ROPA shape-generation ring (per garment mesh): the last decoded vertex
  // arrays keyed by dyn_seq, so the draw can combine the generations under
  // the interp pass's kernel weights (the body's own 8-tap boxcar /
  // pair-lerp: the stepped OR filter-mismatched shape against the
  // interpolated body was the tee jelly). Plus a persistent-mapped upload
  // ring the per-frame blended verts are written into (regioned like the
  // bone ring).
  struct RopaGen {
    uint64_t seq = 0;
    std::vector<float> verts;  // num_verts x 14 floats (scene VS layout)
  };
  std::unordered_map<uint32_t, std::deque<RopaGen>> ropa_shapes;
  static constexpr uint32_t kRopaRegionSize = 1u << 20;
  ID3D12Resource* ropa_ring = nullptr;
  uint8_t* ropa_ring_cpu = nullptr;
  uint32_t ropa_ring_offset = 0;
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
  // Last successfully resolved words-key per streamed-art site
  // (mesh << 1 | slot; slot 0 = diffuse override, 1 = decal override).
  // Serves the site's previous art while a new-mip-words decode is in
  // flight (see resolve_fetch_words). Render thread only.
  std::unordered_map<uint64_t, uint64_t> words_sticky;
  // Paired material descriptors for the fam 5/6 masks+normal 2-descriptor
  // table (t4 = spec/ecc/refl masks, t5 = the material's normal map):
  // (spec_tex << 32 | normal_tex) -> {base heap slot, last refresh frame}.
  // Both views are (re)created every frame on first use so texture
  // replacement (content revalidation / prewarm swaps) can never leave a
  // stale descriptor. Slots come from the monotonic allocator and are never
  // retired (bounded by the distinct reflective materials seen).
  std::unordered_map<uint64_t, std::pair<uint32_t, uint64_t>> mat_pairs;
  // THE texture content store (console identity
  // semantics): fetch-words key -> decode. One
  // words state = one entry; both states of a streaming flap A<->B simply
  // stay resident under their own keys (what the old lookaside simulated
  // with park/take), and superseded states age out via the LRU. Shared by
  // the 3D draw path (through the object routes below), the 2D/HUD pass,
  // and the draw-time fetch-word overrides (posters/ads). Render thread
  // only.
  std::unordered_map<uint64_t, GuestTexture> tex_store;
  // Texture object -> its last STABLE fetch-words state (seqlock
  // double-read at resolve). A route is a lookup aid, never an owner: the
  // game freely retargets objects (mip promote/demote, detail demote,
  // object reuse) and every retarget is just a different store key; a
  // reused object can never serve another binding's art. Render thread
  // only.
  struct TexRoute {
    uint32_t words[6] = {};
    uint64_t key = 0;
    // Live words carry no mip-0 base (streamer demoted the top level; the
    // old pool range is already reused). The route holds the pre-demote
    // state, its cached decode carries the full chain, strictly better,
    // and payload polls are suspended while held (the probes would read
    // the reused pool). A re-promote publishes fresh words and re-routes.
    bool demoted = false;
  };
  std::unordered_map<uint32_t, TexRoute> tex_routes;
  // Sticky texture serving (see resolve_texture in draw_item): the last
  // ADOPTED (served-live) words state per (mesh << 3 | slot). Serves the
  // site's previous art while the current binding's decode is in flight
  // (the console's own mip-transition look), and powers the
  // detail-downgrade hold: a strict base-area shrink keeps serving the
  // previous state's store entry for the hold window. Render thread only.
  struct TexStickySite {
    uint64_t words_key = 0;
    uint64_t area = 0;
    // Nonzero = a downgrade is being held, first seen at this frame. The
    // site adopts the smaller binding once the downgrade persists past
    // skate3_native_render_scene_detail_hold frames.
    uint64_t downgrade_since = 0;
  };
  std::unordered_map<uint64_t, TexStickySite> tex_sticky;
  // First frame a texture object resolved white-with-heal-in-flight:
  // brand-new items (no sticky fallback) skip drawing for a short window
  // instead of flashing white.
  std::unordered_map<uint32_t, uint64_t> tex_pending_first;
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

// ---- Content store helpers --------------------------------------------------
// The store is words-keyed; the guest
// streamer's object retargeting (mip flap A<->B, detail demote, object
// reuse) is just different keys, so both states of any transition stay
// resident and no rebind can ever serve another binding's art.
std::atomic<uint64_t> g_store_evicted{0};
constexpr size_t kTexStoreCap = 3072;

uint32_t SwapU32(uint32_t v);  // defined with the decode helpers below

// Seqlock-stable read of a texture object's six fetch words, guest -> host
// order. The streamer rewrites the words word-by-word on its own thread; a
// mixed snapshot decodes a coherent image of the WRONG memory (a shared
// mip-pool page reads as a collage of neighbor art), and the result is
// stable, valid-looking content no fingerprint can reject after the fact;
// the read itself must be self-consistent. Two consecutive identical
// snapshots (4 attempts) or the caller keeps its previous route/skips.
bool ReadStableTexWords(uint8_t* base, uint32_t tex_ptr, uint32_t out[6]) {
  uint32_t raw[6];
  uint32_t raw2[6];
  bool stable = false;
  for (int attempt = 0; attempt < 4 && !stable; ++attempt) {
    if (!GuestTryCopy(raw, base + tex_ptr + 7 * 4, sizeof(raw)) ||
        !GuestTryCopy(raw2, base + tex_ptr + 7 * 4, sizeof(raw2))) {
      return false;
    }
    stable = std::memcmp(raw, raw2, sizeof(raw)) == 0;
  }
  if (!stable) {
    return false;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    out[i] = SwapU32(raw[i]);
  }
  return true;
}

// Store LRU eviction, run once per frame: superseded words states (old mip
// levels, pre-demote detail sets, one-shot UI art) hold SRV slots + GPU
// memory until nothing has routed to them for a while. Never evicts
// entries touched within the last few frames.
void EvictTexStore(uint64_t frame_number, uint64_t submission) {
  if (g_r.tex_store.size() <= kTexStoreCap) {
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> ages;  // (last-used frame, key)
  ages.reserve(g_r.tex_store.size());
  for (const auto& [k, t] : g_r.tex_store) {
    if (t.last_used_frame + 4 < frame_number) {
      ages.emplace_back(t.last_used_frame, k);
    }
  }
  const size_t excess = g_r.tex_store.size() - kTexStoreCap / 2;
  const size_t n = std::min(ages.size(), excess);
  if (n == 0) {
    return;
  }
  std::nth_element(ages.begin(), ages.begin() + (n - 1), ages.end());
  for (size_t i = 0; i < n; ++i) {
    const auto it = g_r.tex_store.find(ages[i].second);
    if (it != g_r.tex_store.end()) {
      RetireGuestTexture(it->second, submission);
      g_r.tex_store.erase(it);
    }
  }
  g_store_evicted.fetch_add(n, std::memory_order_relaxed);
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
  // Cube map (environment cubes): mips[] holds face-major (face * levels +
  // mip) subresource copies, matching D3D12 subresource numbering, and
  // the SRV is TEXTURECUBE with cube_mip_levels levels. The cube MIP CHAIN
  // is load-bearing: the game's reflective glass perturbs its reflection
  // vector with a per-pixel normal map, so the hardware cube fetch runs at
  // a DEEP gradient-derived LOD; a mip-0-only cube shows the plaza cube's
  // baked streetlight heads as a sharp magnified smear the real console
  // output blurs away.
  bool cube = false;
  uint32_t cube_mip_levels = 1;
  // 6 faces x up to 10 levels (512 -> 1 full generated chain).
  StagedMipCopy mips[64] = {};
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
    srv.TextureCube.MipLevels = sc.cube_mip_levels;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = sc.mip_count;
    gt.srv_format = sc.srv_format;
    gt.srv_mapping = sc.swizzle_mapping;
    gt.srv_mips = sc.mip_count;
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
  uint64_t words_key = 0;  // != 0: content-store result (g_r.tex_store)
  bool cube = false;       // environment cube (g_r.cube_textures)
  GuestTexture gt;
  StagedTexCommit commit;
  bool valid = false;
  // The commit-time stability verify rejected this decode (payload moved
  // between the worker's read and the commit): retry fast, not on the
  // failed-decode ladder.
  bool verify_failed = false;
  // 2D/HUD overlay origin: skip the stability verify (see PrewarmEntry::ui).
  bool ui = false;
};
struct PrewarmResult {
  DrawItem item;
  MeshBuffers buffers;
  bool mesh_valid = false;
  std::vector<StagedTexResult> textures;
  // Draw-path miss result (visible right now): the commit takes it this
  // frame regardless of the gameplay per-frame cap.
  bool miss = false;
};
std::mutex g_prewarm_out_mutex;
std::vector<PrewarmResult> g_prewarm_out;
// Failed builds (buffer objects not initialized yet) land here; the render
// thread re-injects them each frame so retries are frame-paced instead of
// hot-spinning the workers.
std::vector<PrewarmEntry> g_prewarm_retry;  // under g_prewarm_out_mutex

// Shader sources live in src/native/shaders/*.hlsl; the build embeds them
// into this generated header (cmake/EmbedShaders.cmake) so the exe stays
// self-contained. Same kFooSource char arrays as before.
#include "skate3_native_shaders.h"

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
    if (item.env_family != 0 &&
        (item.env_family <= 6 || item.env_family == 13) &&
        item.uv2_fmt == 26) {
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
  // Stretch-veto probe (see g_skin_probe): cache ~32 decoded sample verts
  // so the guest thread can cheaply skin what the GPU will actually draw.
  if (item.skinned) {
    constexpr uint32_t kProbe = 32;
    const uint32_t pn = std::min(kProbe, num_verts);
    SkinProbe probe;
    probe.fp = item.fingerprint;
    probe.s.reserve(pn);
    for (uint32_t s = 0; s < pn; ++s) {
      const uint32_t v = pn > 1 ? (s * (num_verts - 1) / (pn - 1)) : 0;
      SkinProbeSample ps;
      ps.p[0] = dst[v * 14 + 0];
      ps.p[1] = dst[v * 14 + 1];
      ps.p[2] = dst[v * 14 + 2];
      std::memcpy(&ps.bw, &dst[v * 14 + 7], 4);
      std::memcpy(&ps.bi, &dst[v * 14 + 8], 4);
      probe.s.push_back(ps);
    }
    std::lock_guard<std::mutex> lock(g_skin_probe_mutex);
    if (g_skin_probe.size() > 4096) {
      g_skin_probe.clear();
    }
    g_skin_probe[item.mesh] = std::move(probe);
  }
  // ROPA shape blending: retain the decoded vertex array (14 floats per
  // vertex, the scene VS layout) so consecutive generations can be lerped
  // onto the motion-smoothing play clock at draw time; the stepped shape
  // against the interpolated body was the tee jelly (worse at LOWER fps:
  // the excursion scales with the guest period; emulated pairs pose N with
  // shape N and shows none of it).
  if (item.ropa) {
    out.ropa_verts.assign(dst, dst + size_t(num_verts) * 14);
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
          item.env_family != 7 && item.env_family != 9 &&
          item.env_family != 10 && item.env_family != 13;
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
  // Endpoint expansion by BIT REPLICATION, what GPU hardware does. The
  // previous integer-division expansion (v*255/31) reads up to 1/255 dark;
  // on the reflective glass that error, folded through the constant detail
  // texture, tilted every reflection ~0.8 deg (found empirically via the
  // F12 trim sliders: the hand-matched values equaled the
  // replication expansion exactly).
  const auto expand = [](uint16_t c, uint8_t* o) {
    const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    o[0] = uint8_t((r5 << 3) | (r5 >> 2));
    o[1] = uint8_t((g6 << 2) | (g6 >> 4));
    o[2] = uint8_t((b5 << 3) | (b5 >> 2));
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

  // Diagnostic dump: what the RUNTIME decoded, for an offline byte diff
  // against gsnap_tex_decode of the same fetch words (the reflective_trans
  // glass panels' wrong-brightness hunt: their composed lightmap pages take
  // exactly this generated-mips path).
  if (REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
    char path[260];
    std::snprintf(path, sizeof(path),
                  "native_texture_dumps/gen_%08X_%ux%u_t%u.rgba",
                  info.memory.base_address, width, height,
                  info.is_tiled ? 1u : 0u);
    if (FILE* f = std::fopen(path, "wb")) {
      std::fwrite(rgba.data(), 1, rgba.size(), f);
      std::fclose(f);
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
    out.srv_format = srv.Format;
    out.srv_mapping = srv.Shader4ComponentMapping;
    out.srv_mips = mip_count;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(out.texture, &srv, slot);
  }
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = size;
  BuildPayloadProbes(info, addr, ox, oy, pitch_blocks, size, out);
  out.payload_fp = SampleProbeFingerprint(base, out);
  out.near_black = SampleProbeNearBlack(base, out);
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
  // Pre-validate the raw format BEFORE Prepare: TextureInfo::Prepare calls
  // TextureExtent::Calculate, which divides by the format table's
  // block_width/block_height/bytes_per_block; a garbage fetch constant
  // (freed texture object read mid-teardown during a map change) carries a
  // format whose table entry has zeros and crashed a prewarm worker with an
  // integer divide-by-zero (dump skate3.exe.pre-icon.24004, second load
  // into Daly Estates).
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
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
      // Tiled addressing swizzles across 32x32-BLOCK macro tiles AND, for
      // narrow block formats, interleaves across 64x64/128x128-block
      // portions whose byte extent EXCEEDS the linear size; 16bpp reaches
      // 0xC00 bytes from a 32x32 tile origin vs the naive 32*32*2 = 0x800.
      // The previous padded-macro-ROW estimate (added for the 32x32 DXT5
      // HUD compass icons, whose last block sat at 5952 vs base_size 1024)
      // missed that interleave: every 16bpp mip's swizzled offsets for the
      // BOTTOM half of its rows landed past the estimate, the range guard
      // zeroed them, and every PCU Library banner rendered with its lower
      // half black at mip-1 viewing distance (MIP DIAG: guard_zeroed
      // exactly total/2 on 64x64 fmt-4 mips, total/1 on packed oy=16 mips).
      // Size the copy with the SDK's swizzle-aware upper bound instead; if
      // that over-reaches the committed allocation the copy loop below
      // falls back to the reported size and marks the decode incomplete.
      const uint32_t mw = std::max(width >> m, 1u);
      const uint32_t mh = std::max(height >> m, 1u);
      const uint32_t right = (mw + block_w - 1) / block_w + ox;
      const uint32_t bottom = (mh + block_h - 1) / block_h + oy;
      s.size = std::max(
          s.size, rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                      right, bottom, s.pitch_blocks, bytes_per_block_log2));
    }
    s.ox = ox;
    s.oy = oy;
    s.scratch_off = scratch_total;
    scratch_total += s.size;
  }
  tex_scratch.resize(scratch_total);
  uint32_t mips_copied = 0;
  bool copy_truncated = false;
  for (uint32_t m = 0; m < mip_count; ++m) {
    MipSrc& s = srcs[m];
    if (!GuestTryCopy(tex_scratch.data() + s.scratch_off,
                      base + (0xA0000000u | s.addr), s.size)) {
      if (s.min_size >= s.size ||
          !GuestTryCopy(tex_scratch.data() + s.scratch_off,
                        base + (0xA0000000u | s.addr), s.min_size)) {
        break;
      }
      // Tiled fallback: the padded macro rows hold real blocks past
      // min_size; every one of them uploads as ZERO below (the PCU
      // Library half-black banner mips). The decode is marked incomplete
      // so the draw path keeps retrying until the pool commits.
      s.size = s.min_size;
      copy_truncated = true;
    }
    ++mips_copied;
  }
  if (mips_copied == 0) {
    return false;
  }
  mip_count = mips_copied;
  out.incomplete = copy_truncated;
  if (copy_truncated) {
    static std::atomic<uint32_t> s_trunc_logs{0};
    if (s_trunc_logs.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXLOG_INFO(
          "native-scene: texture decode INCOMPLETE (tiled mip copy fell back "
          "to min_size: zeroed tail blocks) {}x{} mips={} w1={:08X}; will "
          "re-decode until complete",
          width, height, mip_count, words[1]);
    }
  }

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
    uint32_t guard_zeroed = 0;  // blocks zeroed by the range guard (diag)
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
          ++guard_zeroed;
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
    // Half-black-mip diagnostic (PCU Library banners): discriminate "the
    // guest pool genuinely holds zeros for this mip" from "our addressing
    // zeroed/misread it". Samples 32 uploaded blocks spread over the mip;
    // guard_zeroed separates range-guard zeroing from zero CONTENT.
    if (m > 0) {
      uint32_t zero_samples = 0;
      const uint32_t total_blocks = p.rows * p.cols;
      for (uint32_t s = 0; s < 32; ++s) {
        const uint32_t bi = uint32_t(uint64_t(total_blocks - 1) * s / 31u);
        const uint32_t by = bi / p.cols;
        const uint32_t bx = bi % p.cols;
        uint64_t q = 0;
        std::memcpy(&q, mapping + p.offset + size_t(by) * p.pitch +
                            size_t(bx) * bytes_per_block,
                    std::min<uint32_t>(8, bytes_per_block));
        zero_samples += q == 0 ? 1 : 0;
      }
      if (total_blocks >= 32 && (guard_zeroed * 4 >= total_blocks ||
                                 zero_samples >= 12)) {
        static std::atomic<uint32_t> s_mip_diag{0};
        if (s_mip_diag.fetch_add(1, std::memory_order_relaxed) < 24) {
          REXLOG_INFO(
              "native-scene: MIP DIAG {}x{} mip {}/{} zero_samples={}/32 "
              "guard_zeroed={}/{} ox={} oy={} pitch_b={} size={} min={} "
              "tiled={} fmt={} w0={:08X} w1={:08X} w2={:08X} mip_addr={:08X}",
              width, height, m, mip_count, zero_samples, guard_zeroed,
              total_blocks, ox, oy, src_pitch_blocks, src_size,
              srcs[m].min_size, info.is_tiled ? 1 : 0, uint32_t(info.format),
              words[0], words[1], words[2], srcs[m].addr);
        }
      }
    }
  }
  // Diagnostic dump (skate3_native_render_scene_lm_dump): no-chain textures
  // through the PLAIN path (the >512px composed lightmap pages the
  // generated-mips gate excludes): mip 0 as linear block rows, raw guest
  // block format post-endian-swap, for offline decode + byte-diff against
  // gsnap_tex_decode of the same fetch words.
  if (mip_count == 1 && REXCVAR_GET(skate3_native_render_scene_lm_dump)) {
    char path[260];
    std::snprintf(path, sizeof(path),
                  "native_texture_dumps/plain_%08X_%ux%u_f%u_t%u.blk",
                  info.memory.base_address, width, height,
                  uint32_t(info.format), info.is_tiled ? 1u : 0u);
    if (FILE* f = std::fopen(path, "wb")) {
      const MipPlan& p0 = plans[0];
      for (uint32_t by = 0; by < p0.rows; ++by) {
        std::fwrite(mapping + p0.offset + size_t(by) * p0.pitch, 1,
                    size_t(p0.cols) * bytes_per_block, f);
      }
      std::fclose(f);
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
    out.srv_format = srv.Format;
    out.srv_mapping = srv.Shader4ComponentMapping;
    out.srv_mips = mip_count;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(out.texture, &srv, slot);
  }
  // Payload sample for content revalidation (see GuestTexture).
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = srcs[0].size;
  BuildPayloadProbes(info, srcs[0].addr, srcs[0].ox, srcs[0].oy,
                     srcs[0].pitch_blocks, srcs[0].size, out);
  out.payload_fp = SampleProbeFingerprint(base, out);
  out.near_black = SampleProbeNearBlack(base, out);
  out.recheck_frame = 0;
  out.valid = g_tex_stage_out == nullptr;  // staged: live only after commit
  return true;
}

// (The object-keyed EnsureGuestTexture wrapper is gone: everything decodes
// from an explicit stable words snapshot, ReadStableTexWords +
// EnsureGuestTextureFromWords, and lands in the words-keyed store.)

// Environment CUBE map for the water / reflective-glass reflection term
// (t6). Six faces untiled independently per level (Xenos cubes are 2D-tiled
// per face slice), WITH the guest mip chain; gradient-derived LOD on the
// normal-mapped reflection vector is what blurs baked cube detail
// (streetlight heads) into the soft reflections of the real console output.
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
  // Same divide-by-zero pre-guard as EnsureGuestTextureFromWords (garbage
  // format -> zero block dims inside Prepare's extent math).
  const rex::graphics::FormatInfo* pre_fi =
      rex::graphics::FormatInfo::Get(uint32_t(fetch.format));
  if (pre_fi == nullptr || pre_fi->block_width == 0 || pre_fi->block_height == 0 ||
      pre_fi->bytes_per_block() == 0) {
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
  // A FULL mip chain is load-bearing for the reflective families. The real
  // baseenvironmentreflective PS perturbs its reflection vector with a
  // per-pixel normal map, so adjacent pixels reflect degrees apart; the
  // hardware cube fetch runs at a VERY deep gradient-derived LOD and the
  // whole reflection resolves to a near-face-average wash (the emulated
  // frame's uniform blue glass; offline: a CUBE_LOD=8 box-downsample probe
  // reproduces the emulated facade's uniformity where mip 0 shows a sharp
  // baked tree/streetlight blob). A truncated chain
  // clamps the LOD shallow and leaves that blob visible. DXT1 cubes (every
  // env cube observed) decode mip 0 to RGBA8 and generate the complete
  // chain down to 1x1 below; other formats fall back to copying the guest
  // chain (down to 32px: smaller levels live packed inside a shared 32x32
  // tile, see GetPackedTileOffset).
  const bool rgba_chain =
      rex::graphics::GetBaseFormat(info.format) == xenos::TextureFormat::k_DXT1 &&
      width >= 8 && (width & (width - 1)) == 0 && width == height;
  uint32_t mip_levels = 1;
  if (!rgba_chain && info.memory.mip_address != 0 && (width & (width - 1)) == 0 &&
      (height & (height - 1)) == 0) {
    const uint32_t avail = std::min(info.mip_levels(), info.GetMaxMipLevels());
    while (mip_levels < avail && (width >> mip_levels) >= 32 &&
           (height >> mip_levels) >= 32) {
      uint32_t ox = 0, oy = 0;
      if (info.GetMipLocation(mip_levels, &ox, &oy, true) == 0 || ox != 0 || oy != 0) {
        break;
      }
      ++mip_levels;
    }
  }
  // Per-level guest layout: each level stores the six face slices
  // consecutively (extent depth = 6; GetMipLocation walks whole levels).
  struct CubeLevel {
    uint32_t addr, pitch_blocks, slice_bytes, cols, rows, scratch_off;
    uint32_t up_pitch, up_face_bytes, w, h;
  };
  CubeLevel lv[6] = {};
  uint32_t scratch_total = 0;
  for (uint32_t m = 0; m < mip_levels; ++m) {
    CubeLevel& L = lv[m];
    const auto ext = m == 0 ? info.extent : info.GetMipExtent(m, true);
    uint32_t ox = 0, oy = 0;
    L.addr = m == 0 ? info.memory.base_address : info.GetMipLocation(m, &ox, &oy, true);
    L.pitch_blocks = ext.block_pitch_h;
    L.slice_bytes = ext.block_pitch_h * ext.block_pitch_v * bytes_per_block;
    const uint32_t mw = std::max(width >> m, 1u);
    const uint32_t mh = std::max(height >> m, 1u);
    L.cols = (mw + block_w - 1) / block_w;
    L.rows = (mh + block_h - 1) / block_h;
    L.w = L.cols * block_w;
    L.h = L.rows * block_h;
    L.up_pitch = (L.cols * bytes_per_block + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) &
                 ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    L.up_face_bytes =
        (L.up_pitch * L.rows + (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u)) &
        ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u);
    L.scratch_off = scratch_total;
    scratch_total += L.slice_bytes * 6;
  }
  if (scratch_total == 0 || scratch_total > 24u * 1024u * 1024u) {
    return false;
  }
  static thread_local std::vector<uint8_t> cube_scratch;
  cube_scratch.resize(scratch_total);
  for (uint32_t m = 0; m < mip_levels; ++m) {
    if (!GuestTryCopy(cube_scratch.data() + lv[m].scratch_off,
                      base + (0xA0000000u | lv[m].addr), lv[m].slice_bytes * 6)) {
      if (m == 0) {
        return false;
      }
      mip_levels = m;  // truncate the chain at the first unreadable level
      break;
    }
  }

  if (rgba_chain) {
    // Decode DXT1 mip 0 -> RGBA8 per face, box-filter the full chain to
    // 1x1, upload as an RGBA cube. CPU cost is one-time per cube (runs on
    // the decode workers).
    ID3D12Device* device = context.d3d12.device;
    const uint32_t levels = 1u + uint32_t(std::countr_zero(width));
    struct Level {
      uint32_t w, pitch, face_bytes, upload_off;  // upload_off within a face
    };
    Level lvs[16] = {};
    uint32_t face_upload = 0;
    for (uint32_t m = 0; m < levels; ++m) {
      Level& L = lvs[m];
      L.w = std::max(width >> m, 1u);
      L.pitch = (L.w * 4u + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)) &
                ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
      L.face_bytes = (L.pitch * L.w + (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u)) &
                     ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u);
      L.upload_off = face_upload;
      face_upload += L.face_bytes;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = UINT16(levels);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    static thread_local std::vector<uint8_t> rgba;   // level 0 of one face
    static thread_local std::vector<uint8_t> down;   // downsample scratch
    static thread_local std::vector<uint8_t> bc_row; // one untiled block row
    const CubeLevel& L0 = lv[0];
    bc_row.resize(size_t(L0.cols) * bytes_per_block);
    for (uint32_t face = 0; face < 6; ++face) {
      // Per-face: the downsample loop below SWAPS rgba/down, so their sizes
      // end the chain tiny; the next face's full-size decode writes must
      // not index a shrunken buffer.
      rgba.resize(size_t(width) * height * 4);
      down.resize(size_t(width / 2) * (height / 2) * 4);
      const uint8_t* guest =
          cube_scratch.data() + L0.scratch_off + size_t(face) * L0.slice_bytes;
      for (uint32_t by = 0; by < L0.rows; ++by) {
        for (uint32_t bx = 0; bx < L0.cols; ++bx) {
          uint32_t source_offset;
          if (info.is_tiled) {
            source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), L0.pitch_blocks, bytes_per_block_log2));
          } else {
            source_offset = (by * L0.pitch_blocks + bx) * bytes_per_block;
          }
          if (source_offset + bytes_per_block > L0.slice_bytes) {
            std::memset(&bc_row[size_t(bx) * bytes_per_block], 0, bytes_per_block);
            continue;
          }
          std::memcpy(&bc_row[size_t(bx) * bytes_per_block], guest + source_offset,
                      bytes_per_block);
        }
        SwapGuestEndian(bc_row.data(), uint32_t(bc_row.size()), info.endianness);
        for (uint32_t bx = 0; bx < L0.cols; ++bx) {
          uint8_t px[16][4];
          DecodeBc1Block(&bc_row[size_t(bx) * bytes_per_block], px);
          for (uint32_t r = 0; r < 4; ++r) {
            std::memcpy(&rgba[(size_t(by * 4 + r) * width + bx * 4) * 4], px[r * 4],
                        16);
          }
        }
      }
      // Upload level 0, then box-filter down the chain in place.
      const uint8_t* src = rgba.data();
      uint32_t w = width;
      for (uint32_t m = 0; m < levels; ++m) {
        uint8_t* up = mapping + size_t(face) * face_upload + lvs[m].upload_off;
        for (uint32_t y = 0; y < w; ++y) {
          std::memcpy(up + size_t(y) * lvs[m].pitch, src + size_t(y) * w * 4,
                      size_t(w) * 4);
        }
        if (m + 1 >= levels) {
          break;
        }
        const uint32_t hw = w / 2;
        for (uint32_t y = 0; y < hw; ++y) {
          for (uint32_t x = 0; x < hw; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
              const uint32_t s =
                  uint32_t(src[((y * 2) * w + x * 2) * 4 + c]) +
                  uint32_t(src[((y * 2) * w + x * 2 + 1) * 4 + c]) +
                  uint32_t(src[((y * 2 + 1) * w + x * 2) * 4 + c]) +
                  uint32_t(src[((y * 2 + 1) * w + x * 2 + 1) * 4 + c]);
              down[(size_t(y) * hw + x) * 4 + c] = uint8_t((s + 2) / 4);
            }
          }
        }
        rgba.swap(down);
        src = rgba.data();
        w = hw;
      }
    }
    out.upload->Unmap(0, nullptr);
    REXLOG_INFO("native-scene: cube {:08X} {}x{} DXT1 -> RGBA full chain ({} levels)",
                tex_ptr, width, height, levels);
    const UINT swizzle_mapping =
        ComposeSrvSwizzle(fetch.swizzle, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    if (g_tex_stage_out != nullptr) {
      StagedTexCommit& sc = *g_tex_stage_out;
      sc.copy_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sc.srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sc.swizzle_mapping = swizzle_mapping;
      sc.cube = true;
      sc.cube_mip_levels = levels;
      sc.mip_count = 6 * levels;
      for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t m = 0; m < levels; ++m) {
          sc.mips[face * levels + m] = {face * face_upload + lvs[m].upload_off,
                                        lvs[m].pitch, lvs[m].w, lvs[m].w};
        }
      }
      out.payload_addr = 0xA0000000u | info.memory.base_address;
      out.payload_size = lv[0].slice_bytes * 6;
      out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
      out.recheck_frame = 0;
      out.valid = false;  // live only after commit
      return true;
    }
    auto& list = context.d3d12.command_processor->GetDeferredCommandList();
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t m = 0; m < levels; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = out.texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = face * levels + m;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = out.upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = size_t(face) * face_upload + lvs[m].upload_off;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = lvs[m].w;
        src.PlacedFootprint.Footprint.Height = lvs[m].w;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = lvs[m].pitch;
        list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
    }
    context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                          out.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!AllocGuestSrvSlot(out.srv_slot)) {
      return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = swizzle_mapping;
    srv.TextureCube.MipLevels = levels;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
    device->CreateShaderResourceView(out.texture, &srv, slot);
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = lv[0].slice_bytes * 6;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = true;
    return true;
  }

  uint32_t face_upload = 0;  // one face's full mip chain in the upload buffer
  for (uint32_t m = 0; m < mip_levels; ++m) {
    face_upload += lv[m].up_face_bytes;
  }
  ID3D12Device* device = context.d3d12.device;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = host_width;
  desc.Height = host_height;
  desc.DepthOrArraySize = 6;
  desc.MipLevels = UINT16(mip_levels);
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
  const auto upload_offset = [&](uint32_t face, uint32_t m) {
    uint32_t off = face * face_upload;
    for (uint32_t k = 0; k < m; ++k) {
      off += lv[k].up_face_bytes;
    }
    return off;
  };
  // (The "Xenos cube T runs bottom-up" flip that briefly lived here was
  // WRONG; it matched two probe pixels by coincidence and turned every
  // facade pavement-white in game. The stored face orientation is correct
  // as-is; the emulated look comes from LOD depth, not orientation.)
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t m = 0; m < mip_levels; ++m) {
      const CubeLevel& L = lv[m];
      const uint8_t* guest =
          cube_scratch.data() + L.scratch_off + size_t(face) * L.slice_bytes;
      uint8_t* up = mapping + upload_offset(face, m);
      const uint32_t row_bytes = L.cols * bytes_per_block;
      for (uint32_t by = 0; by < L.rows; ++by) {
        uint8_t* out_row = up + size_t(by) * L.up_pitch;
        for (uint32_t bx = 0; bx < L.cols; ++bx) {
          uint32_t source_offset;
          if (info.is_tiled) {
            source_offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                int32_t(bx), int32_t(by), L.pitch_blocks, bytes_per_block_log2));
          } else {
            source_offset = (by * L.pitch_blocks + bx) * bytes_per_block;
          }
          if (source_offset + bytes_per_block > L.slice_bytes) {
            std::memset(out_row + size_t(bx) * bytes_per_block, 0, bytes_per_block);
            continue;
          }
          std::memcpy(out_row + size_t(bx) * bytes_per_block, guest + source_offset,
                      bytes_per_block);
        }
        SwapGuestEndian(out_row, row_bytes, info.endianness);
      }
    }
  }
  out.upload->Unmap(0, nullptr);

  REXLOG_INFO("native-scene: cube {:08X} {}x{} fmt={} mips={} (of {} avail)", tex_ptr,
              width, height, uint32_t(info.format), mip_levels, info.mip_levels());
  if (g_tex_stage_out != nullptr) {
    // Decode worker: export the commit recipe, face-major (face * levels +
    // mip) entries matching D3D12 subresource numbering.
    StagedTexCommit& sc = *g_tex_stage_out;
    sc.copy_format = host.resource_format;
    sc.srv_format = host.srv_format;
    sc.swizzle_mapping = ComposeSrvSwizzle(fetch.swizzle, host.host_swizzle);
    sc.cube = true;
    sc.cube_mip_levels = mip_levels;
    sc.mip_count = 6 * mip_levels;
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t m = 0; m < mip_levels; ++m) {
        sc.mips[face * mip_levels + m] = {upload_offset(face, m), lv[m].up_pitch,
                                          lv[m].w, lv[m].h};
      }
    }
    out.payload_addr = 0xA0000000u | info.memory.base_address;
    out.payload_size = lv[0].slice_bytes * 6;
    out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
    out.recheck_frame = 0;
    out.valid = false;  // live only after commit
    return true;
  }

  auto& list = context.d3d12.command_processor->GetDeferredCommandList();
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t m = 0; m < mip_levels; ++m) {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = out.texture;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = face * mip_levels + m;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = out.upload;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = upload_offset(face, m);
      src.PlacedFootprint.Footprint.Format = host.resource_format;
      src.PlacedFootprint.Footprint.Width = lv[m].w;
      src.PlacedFootprint.Footprint.Height = lv[m].h;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = lv[m].up_pitch;
      list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
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
  srv.TextureCube.MipLevels = mip_levels;
  D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
  slot.ptr += size_t(out.srv_slot) * g_r.srv_size;
  device->CreateShaderResourceView(out.texture, &srv, slot);
  out.payload_addr = 0xA0000000u | info.memory.base_address;
  out.payload_size = lv[0].slice_bytes * 6;
  out.payload_fp = SamplePayloadFingerprint(base, out.payload_addr, out.payload_size);
  out.recheck_frame = 0;
  out.valid = true;
  return true;
}

// Scene vertex layout (DecodeMesh's 56-byte output): shared by the scene
// PSO family and the shadow-caster PSO.
constexpr D3D12_INPUT_ELEMENT_DESC kSceneInputLayout[7] = {
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

// ---- EnsurePipeline helpers ------------------------------------------------
// Split from the former ~1,000-line EnsurePipeline: one resource
// group per function, bodies unchanged. Every helper is idempotent (guarded
// by its own g_r state) and returns false only on a failure that must abort
// the native path (g_r.failed set where the original did).

bool EnsureRootSignature(const NativeGuestOutputRenderContext& context) {
  if (g_r.root_signature) {
    return true;
  }
  {
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
    srv_range[3].BaseShaderRegister = 4;  // decal art / spec masks (t4)
    // Second descriptor of the t4 table = the fam 5/6 normal map (t5), a
    // paired heap slot (see RendererState::mat_pairs). Range growth is free
    // root-space-wise; draws without a pair leave t5 pointing at whatever
    // follows their single slot; the shader only samples t5 when
    // overlay.w == 4 (pair bound).
    srv_range[3].NumDescriptors = 2;
    srv_range[4] = srv_range[0];
    srv_range[4].BaseShaderRegister = 7;  // shadow atlas (t7)
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
  return true;
}

// MSAA level selection + the main scene PSO and its culling/blend/depth
// variants (cull-back sheets, transparent, entity fade, hair 2-pass,
// no-depth, outline mask).
bool EnsureScenePsoFamily(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  pso.InputLayout = {kSceneInputLayout, 7};
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
  // Entity-fade variant: z-write ON (see RendererState::pso_fade). Drawn
  // at the head of the blended sub-pass so glass/hair still composite
  // over the faded body.
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_fade)))) {
    g_r.pso_fade = nullptr;  // fade items fall back to the z-write-off blend
  }
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
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
  return true;
}

// Fullscreen MSAA resolve PSO (no-op below MSAA 2x).
bool EnsureResolvePso(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// Popup background blur pipelines; PSO-create failure only disables blur.
bool EnsureBlurPsos(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// Selection-outline edge composite PSO; create failure disables outline.
bool EnsureOutlineEdgePso(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// 2D overlay pipeline.
bool Ensure2dPso(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
    // FMV variant: identical pipeline, ps_yuv2d combine (see the movie-quad
    // substitution in the 2D replay). Failure only loses native FMV; the
    // emulated yield fallback covers it.
    ID3DBlob* uyuv = nullptr;
    if (SUCCEEDED(D3DCompile(kShader2dSource, sizeof(kShader2dSource) - 1, "native_2d",
                             nullptr, nullptr, "ps_yuv2d", "ps_5_0", 0, 0, &uyuv,
                             nullptr))) {
      up.PS = {uyuv->GetBufferPointer(), uyuv->GetBufferSize()};
      if (g_r.pso_yuv2d) g_r.pso_yuv2d->Release();
      if (FAILED(device->CreateGraphicsPipelineState(&up, IID_PPV_ARGS(&g_r.pso_yuv2d)))) {
        REXLOG_WARN("native-scene: FMV 2D PSO creation failed - movies yield");
        g_r.pso_yuv2d = nullptr;
      }
      uyuv->Release();
    } else {
      REXLOG_WARN("native-scene: ps_yuv2d compile failed - movies yield");
    }
    uvs->Release();
    ups->Release();
    if (uerrors) uerrors->Release();
    if (FAILED(hr4)) {
      REXLOG_ERROR("native-scene: 2D PSO creation failed {:08X}", uint32_t(hr4));
      g_r.failed = true;
      return false;
    }
  }
  return true;
}

// In-world spline pipelines (darken + additive default).
bool EnsureSplinePsos(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// Dynamic-shadow caster + per-tile blur/convert PSOs.
bool EnsureShadowPsos(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
    cp.InputLayout = {kSceneInputLayout, 7};
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
  return true;
}

// Descriptor heaps (RTV/DSV/SRV) + the bone and 2D-vertex upload rings.
bool EnsureHeapsAndRings(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
  if (!g_r.rtv_heap) {
    // Slots 0/1 = guest output / MSAA color; 2+ = APT render-to-texture
    // targets; 5/6 = blur intermediates; 7 = outline mask; 8..13 = the
    // photo-editor postfx chain (full x2, half x2, quarter, packed depth).
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16,
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

  if (!g_r.ropa_ring) {
    g_r.ropa_ring = CreateUploadBuffer(
        device, size_t(RendererState::kRopaRegionSize) * RendererState::kBoneRegions);
    if (!g_r.ropa_ring ||
        FAILED(g_r.ropa_ring->Map(0, nullptr,
                                  reinterpret_cast<void**>(&g_r.ropa_ring_cpu)))) {
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
  return true;
}

// ---- Photo-editor postfx chain (photo_fx.hlsl) -----------------------------
// Root signature + eight PSOs + the fixed-size intermediates. Lazy: built on
// the first photo-editor frame (a one-time ~100 ms compile the frozen-scene
// editor absorbs invisibly). Output-sized targets are (re)built per frame by
// the render block on size change.
bool EnsurePhotoFxPipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.pfx_ready) {
    return true;
  }
  if (g_r.pfx_failed) {
    return false;
  }
  ID3D12Device* device = context.d3d12.device;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("native-scene: photo postfx pipeline setup failed ({})", what);
    g_r.pfx_failed = true;
    return false;
  };
  // Root signature: root CBV b0 + eight single-SRV tables (t0..t7; each can
  // point anywhere in the SRV heap, the established pattern) + samplers.
  if (g_r.pfx_root_sig == nullptr) {
    D3D12_ROOT_PARAMETER params[9] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE ranges[8] = {};
    const uint32_t regs[8] = {0, 1, 2, 3, 4, 6, 7, 5};
    for (int i = 0; i < 8; ++i) {
      ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[i].NumDescriptors = 1;
      ranges[i].BaseShaderRegister = regs[i];
      params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
      params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
      params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC smp[3] = {};
    smp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp[0].MaxLOD = D3D12_FLOAT32_MAX;
    smp[0].ShaderRegister = 0;
    smp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    smp[1] = smp[0];
    smp[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;  // packed depth
    smp[1].ShaderRegister = 1;
    smp[2] = smp[0];
    smp[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;  // grain
    smp[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    smp[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    smp[2].ShaderRegister = 2;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 9;
    desc.pParameters = params;
    desc.NumStaticSamplers = 3;
    desc.pStaticSamplers = smp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (!context.d3d12.create_root_signature(context.d3d12.command_processor_user_data,
                                             &desc, &g_r.pfx_root_sig)) {
      return fail("root signature");
    }
  }
  // Shaders + PSOs. Pass order matches RendererState::pfx_pso.
  struct Entry {
    const char* vs;
    const char* ps;
    DXGI_FORMAT rtv;
  };
  const Entry entries[8] = {
      {"vs_raw", "ps_depthpack", DXGI_FORMAT_R8G8B8A8_UNORM},
      {"vs_offset", "ps_visualfx", DXGI_FORMAT_R8G8B8A8_UNORM},
      {"vs_offset", "ps_dof_down", DXGI_FORMAT_R8G8B8A8_UNORM},
      {"vs_offset", "ps_dof_mb", DXGI_FORMAT_R8G8B8A8_UNORM},
      {"vs_offset", "ps_dof", DXGI_FORMAT_R8G8B8A8_UNORM},
      {"vs_raw", "ps_uber", DXGI_FORMAT_R8G8B8A8_UNORM},
      {"vs_scaled", "ps_fisheye", context.d3d12.guest_output_format},
      {"vs_raw", "ps_blit", DXGI_FORMAT_R8G8B8A8_UNORM},
  };
  const D3D_SHADER_MACRO msaa_defines[] = {{"PFX_MSAA", "1"}, {nullptr, nullptr}};
  for (int i = 0; i < 8; ++i) {
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* errors = nullptr;
    const D3D_SHADER_MACRO* defs = (i == 0 && g_r.msaa > 1) ? msaa_defines : nullptr;
    if (FAILED(D3DCompile(kPhotoFxShaderSource, sizeof(kPhotoFxShaderSource) - 1,
                          "photo_fx", defs, nullptr, entries[i].vs, "vs_5_0", 0, 0,
                          &vs, &errors)) ||
        FAILED(D3DCompile(kPhotoFxShaderSource, sizeof(kPhotoFxShaderSource) - 1,
                          "photo_fx", defs, nullptr, entries[i].ps, "ps_5_0", 0, 0,
                          &ps, &errors))) {
      REXLOG_ERROR("native-scene: photo postfx shader compile failed ({}): {}",
                   entries[i].ps,
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      g_r.pfx_failed = true;
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_r.pfx_root_sig;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = entries[i].rtv;
    pso.SampleDesc.Count = 1;
    const HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pfx_pso[i]));
    vs->Release();
    ps->Release();
    if (FAILED(hr)) {
      return fail(entries[i].ps);
    }
  }
  // Fixed-size intermediates + the identity grade LUT + the CB ring.
  {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    struct Fixed {
      ID3D12Resource** res;
      uint32_t w, h;
    };
    const Fixed fixed[3] = {
        {&g_r.pfx_half[0], RendererState::kPfxHalfW, RendererState::kPfxHalfH},
        {&g_r.pfx_half[1], RendererState::kPfxHalfW, RendererState::kPfxHalfH},
        {&g_r.pfx_quarter, RendererState::kPfxQuarterW, RendererState::kPfxQuarterH},
    };
    for (const Fixed& f : fixed) {
      if (*f.res != nullptr) {
        continue;
      }
      desc.Width = f.w;
      desc.Height = f.h;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 &clear, IID_PPV_ARGS(f.res)))) {
        return fail("intermediate target");
      }
    }
    if (g_r.pfx_lut == nullptr) {
      // 32^3 identity grade LUT (the editor captures run with the LUT blend
      // weight at 0; identity keeps any treatment that enables it neutral
      // instead of garbage). Coordinate mapping mirrors the uber literals:
      // u = g*0.96875 + 0.015625, v = r*(-0.96875) + 0.984375 (flipped),
      // w = b*0.96875 + 0.015625, so voxel (ix,iy,iz) stores
      // r = (0.984375 - v)/0.96875, g = (u - 0.015625)/0.96875, b likewise.
      D3D12_RESOURCE_DESC lut{};
      lut.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      lut.Width = 32;
      lut.Height = 32;
      lut.DepthOrArraySize = 32;
      lut.MipLevels = 1;
      lut.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      lut.SampleDesc.Count = 1;
      if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &lut,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g_r.pfx_lut)))) {
        return fail("LUT");
      }
      // Upload: 32 rows of 32 texels x 32 slices, 256-byte row pitch.
      const uint32_t row_pitch = 256;
      const uint32_t slice_pitch = row_pitch * 32;
      g_r.pfx_lut_upload = CreateUploadBuffer(device, size_t(slice_pitch) * 32);
      if (g_r.pfx_lut_upload == nullptr) {
        return fail("LUT upload");
      }
      uint8_t* p = nullptr;
      if (FAILED(g_r.pfx_lut_upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) {
        return fail("LUT map");
      }
      for (uint32_t iz = 0; iz < 32; ++iz) {
        for (uint32_t iy = 0; iy < 32; ++iy) {
          uint8_t* row = p + size_t(iz) * slice_pitch + size_t(iy) * row_pitch;
          const float v = (float(iy) + 0.5f) / 32.0f;
          const float w = (float(iz) + 0.5f) / 32.0f;
          const float r = (0.984375f - v) / 0.96875f;
          const float b = (w - 0.015625f) / 0.96875f;
          for (uint32_t ix = 0; ix < 32; ++ix) {
            const float u = (float(ix) + 0.5f) / 32.0f;
            const float g = (u - 0.015625f) / 0.96875f;
            row[ix * 4 + 0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[ix * 4 + 3] = 255;
          }
        }
      }
      g_r.pfx_lut_upload->Unmap(0, nullptr);
    }
    if (g_r.pfx_cb == nullptr) {
      // 8 pass slots x 4 KB x 4 frames in flight.
      g_r.pfx_cb = CreateUploadBuffer(device, 8u * 4096u * 4u);
      if (g_r.pfx_cb == nullptr ||
          FAILED(g_r.pfx_cb->Map(0, nullptr,
                                 reinterpret_cast<void**>(&g_r.pfx_cb_ptr)))) {
        return fail("constant ring");
      }
    }
  }
  if (!g_r.pfx_srv_allocated) {
    for (uint32_t& s : g_r.pfx_srv) {
      s = g_r.srv_next++;
    }
    g_r.pfx_srv_allocated = true;
  }
  // Fixed-target RTVs (heap slots 10/11 = halves, 12 = quarter) + SRVs.
  {
    const auto make_views = [&](ID3D12Resource* res, uint32_t rtv_slot,
                                uint32_t srv_slot) {
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += size_t(rtv_slot) * g_r.rtv_size;
      device->CreateRenderTargetView(res, nullptr, rtv);
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
      slot.ptr += size_t(srv_slot) * g_r.srv_size;
      device->CreateShaderResourceView(res, &srv, slot);
    };
    make_views(g_r.pfx_half[0], 10, g_r.pfx_srv[2]);
    make_views(g_r.pfx_half[1], 11, g_r.pfx_srv[3]);
    make_views(g_r.pfx_quarter, 12, g_r.pfx_srv[4]);
    // LUT SRV (Texture3D).
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture3D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += size_t(g_r.pfx_srv[6]) * g_r.srv_size;
    device->CreateShaderResourceView(g_r.pfx_lut, &srv, slot);
  }
  g_r.pfx_ready = true;
  REXLOG_INFO("native-scene: photo postfx pipeline ready (8 passes, msaa={})",
              g_r.msaa);
  return true;
}

// Shadow atlas targets + the always-bound b1 receiver constant buffer.
bool EnsureShadowResources(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// Blur intermediates + the output-sized selection-outline mask. Failures
// only disable their feature, never abort the native path.
bool EnsureBlurOutlineTargets(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// 1x1 white diffuse fallback + 1x1x6 mid-gray environment-cube fallback.
bool EnsureFallbackTextures(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

// Depth buffer + MSAA color target, rebuilt on output-size change.
bool EnsureOutputSizedTargets(const NativeGuestOutputRenderContext& context) {
  ID3D12Device* device = context.d3d12.device;
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
  return true;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) return false;
  ID3D12Device* device = context.d3d12.device;
  g_r.device = device;

  if (!EnsureRootSignature(context)) {
    return false;
  }

  if (!g_r.pso || g_r.rtv_format != context.d3d12.guest_output_format) {
    if (!EnsureScenePsoFamily(context) || !EnsureResolvePso(context) ||
        !EnsureBlurPsos(context) || !EnsureOutlineEdgePso(context) ||
        !Ensure2dPso(context) || !EnsureSplinePsos(context) ||
        !EnsureShadowPsos(context)) {
      return false;
    }
    REXLOG_INFO("native-scene: pipelines created (MSAA x{})", g_r.msaa);
    g_r.rtv_format = context.d3d12.guest_output_format;
  }

  if (!EnsureHeapsAndRings(context) || !EnsureShadowResources(context)) {
    return false;
  }
  EnsureBlurOutlineTargets(context);
  if (!EnsureFallbackTextures(context) || !EnsureOutputSizedTargets(context)) {
    return false;
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
    uint32_t words[6];
    if (!ReadStableTexWords(base, tex_ptr, words) || words[1] == 0) {
      return;  // unreadable / mid-rewrite / demoted: the draw path routes it
    }
    const uint64_t key = FetchWordsKey(words);
    auto it = g_r.tex_store.find(key);
    if (it != g_r.tex_store.end()) {
      if (!it->second.valid || frame_number < it->second.recheck_frame ||
          !REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        return;  // negative caches retry via the draw path's schedule
      }
      if (!within()) {
        ++wc.deferred;
        return;
      }
      it->second.recheck_frame = frame_number + 16;
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      if (!it->second.incomplete && (fp == 0 || fp == it->second.payload_fp)) {
        return;
      }
      RetireGuestTexture(it->second, command_processor->GetCurrentSubmission());
      g_r.tex_store.erase(it);
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    if (!gt.valid) {
      // Negative-cache exactly like the draw path so a permanently
      // unreadable payload cannot hold warmup open.
      std::memcpy(gt.fetch_words, words, sizeof(gt.fetch_words));
      gt.retry_after_frame = frame_number + 120;
    }
    gt.last_used_frame = frame_number;
    g_r.tex_store.emplace(key, gt);
  };
  // Draw-time fetch-word bindings (streamed artwork / decal ad overrides)
  // share the same store.
  const auto warm_fetch_words = [&](const uint32_t words[6]) {
    if (words[1] == 0) {
      return;
    }
    const uint64_t fkey = FetchWordsKey(words);
    if (g_r.tex_store.contains(fkey)) {
      return;
    }
    if (!within()) {
      ++wc.deferred;
      return;
    }
    ++wc.decodes;
    GuestTexture gt;
    EnsureGuestTextureFromWords(context, base, words, gt);
    gt.last_used_frame = frame_number;
    g_r.tex_store.emplace(fkey, gt);
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
  if ((item.env_family != 0 && !item.decal && item.env_family != 10) ||
      item.unlit) {  // unlit = sky: spec_tex is the 1D sun gradient
    warm_texture(item.spec_tex);
  }
  // Environment cube (negative-cached like the draw path).
  if ((item.water || item.char_family >= 6 ||
       (item.env_family >= 5 && item.env_family <= 6) ||
       item.env_family == 13) &&
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
  if (g_miss_queue.size() < 65536 && g_miss_inflight_mesh.insert(mesh).second) {
    PrewarmEntry e{mesh, 8};
    e.miss = true;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}


// Words-keyed texture miss (streamed-artwork posters / event ads): the art
// exists only as draw-time fetch words. Decoded unbudgeted inline these were
// a traversal hitch (a poster decode costs the same ~10 ms as any texture);
// while a decode is in flight the item falls back to its channel diffuse
// (the placeholder poster), not white.
void EnqueueWordsMiss(uint64_t key, const uint32_t words[6], bool ui = false) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_words.insert(key).second) {
    PrewarmEntry e{0, 0, 0, key};
    std::memcpy(e.words, words, sizeof(e.words));
    e.miss = true;
    e.ui = ui;
    g_miss_queue.push_back(e);
    g_prewarm_cv.notify_one();
  }
}

// Environment-cube miss: one cube decode measured up to ~100 ms inline;
// the gray fallback cube shows for the 1-3 frames the workers need instead.
void EnqueueCubeMiss(uint32_t tex) {
  std::lock_guard<std::mutex> lock(g_prewarm_mutex);
  if (g_miss_queue.size() < 65536 && g_miss_inflight_tex.insert(tex).second) {
    PrewarmEntry e{0, 0, tex};
    e.cube = true;
    e.miss = true;
    g_miss_queue.push_back(e);
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
    tr.ui = e.ui;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.backend = NativeGuestOutputBackend::kD3D12;
    stage_ctx.d3d12.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, e.words, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;
    res.mesh_valid = false;
    res.miss = e.miss;
    res.textures.push_back(std::move(tr));
    std::lock_guard<std::mutex> lock(g_prewarm_out_mutex);
    g_prewarm_out.push_back(std::move(res));
    return;
  }
  if (e.mesh == 0 && e.tex != 0) {
    // Environment-cube miss (see EnqueueCubeMiss, the only object-keyed
    // texture path left): stage the decode up to a filled upload resource;
    // the commit records the GPU copies + SRV.
    StagedTexResult tr;
    tr.key = e.tex;
    tr.cube = true;
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.backend = NativeGuestOutputBackend::kD3D12;
    stage_ctx.d3d12.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestCubeTexture(stage_ctx, base, e.tex, tr.gt);
    g_tex_stage_out = nullptr;
    PrewarmResult res;
    res.item.mesh = 0;  // texture-only result (DrawItem::mesh has no default)
    res.mesh_valid = false;
    res.miss = e.miss;
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
  res.miss = e.miss;
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
    // Stable words snapshot: the decode and its store key both come from
    // this snapshot, so a mid-rewrite object can never stage a mixed state.
    uint32_t words[6];
    if (!ReadStableTexWords(base, tex_ptr, words) || words[1] == 0) {
      return;
    }
    const uint64_t wkey = FetchWordsKey(words);
    {
      // Words-aware dedupe: the same content staged once per load; a
      // rebound object (new words) re-stages under its new key.
      std::lock_guard<std::mutex> lock(g_prewarm_mutex);
      const auto [it, fresh] = g_prewarm_tex_seen.try_emplace(tex_ptr, wkey);
      if (!fresh) {
        if (it->second == wkey) {
          return;
        }
        it->second = wkey;
      }
    }
    StagedTexResult tr;
    tr.words_key = wkey;
    // Staged mode uses only context.d3d12.device (copies/barrier/SRV are
    // exported for the commit), so a device-only context suffices.
    NativeGuestOutputRenderContext stage_ctx{};
    stage_ctx.backend = NativeGuestOutputBackend::kD3D12;
    stage_ctx.d3d12.device = g_r.device;
    g_tex_stage_out = &tr.commit;
    tr.valid = EnsureGuestTextureFromWords(stage_ctx, base, words, tr.gt);
    g_tex_stage_out = nullptr;
    if (!tr.valid) {
      std::memcpy(tr.gt.fetch_words, words, sizeof(tr.gt.fetch_words));
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
  if ((item.env_family != 0 && !item.decal && item.env_family != 10) ||
      item.unlit) {  // unlit = sky: spec_tex is the 1D sun gradient
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
        return !g_prewarm_queue.empty() || !g_miss_queue.empty() ||
               !g_dyn_jobs.empty();
      });
      if (!g_dyn_jobs.empty()) {
        // Dynamic cloth first: these are per-frame payloads whose result
        // should land at the very next commit.
        dyn = std::move(g_dyn_jobs.front());
        g_dyn_jobs.erase(g_dyn_jobs.begin());
        have_dyn = true;
      } else if (!g_miss_queue.empty()) {
        // Draw-path misses next: this content is visible RIGHT NOW (white /
        // skipped geometry). On the old shared LIFO queue a streaming
        // registration burst kept cutting the line ahead of the visible
        // miss; medium-distance pop-in lasted the whole backlog.
        e = g_miss_queue.front();
        g_miss_queue.erase(g_miss_queue.begin());
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
        res.miss = true;  // per-frame cloth: never behind the commit cap
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
  // Below-normal priority makes extra workers near-free (they only soak
  // idle cores; the guest always wins the contention) while the pool size
  // governs how fast a gameplay streaming burst decodes; 4 workers left
  // medium-distance pop-in on big sectors visibly behind the emulated
  // renderer.
  const unsigned n = std::clamp(hw / 3u, 2u, 8u);
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
      // Oldest first: draining from the END starved early results under a
      // sustained streaming burst (they sat behind an ever-refilling tail,
      // so exactly the content that had waited longest stayed popped-out).
      // Draw-path MISS results (content visible right now: white/skipped)
      // bypass the cap entirely: under a big sector streaming burst they
      // otherwise queued behind hundreds of speculative prewarm results
      // for several frames of visible pop-in. Only a handful arrive per
      // frame, so the bypass cannot recreate the commit hitch the cap
      // exists to prevent.
      std::vector<PrewarmResult> rest;
      rest.reserve(g_prewarm_out.size());
      size_t taken = 0;
      for (PrewarmResult& r : g_prewarm_out) {
        const bool is_miss = r.miss;
        if (is_miss || taken < kMaxCommitPerFrame) {
          if (!is_miss) {
            ++taken;
          }
          done.push_back(std::move(r));
        } else {
          rest.push_back(std::move(r));
        }
      }
      g_prewarm_out.swap(rest);
    }
  }
  if (done.empty()) {
    return;
  }
  const auto commit_t0 = PerfClock::now();
  bool committed_tex = false;
  // For the payload-stability verify below (SEH-guarded sampled reads).
  uint8_t* verify_base = g_guest_base.load(std::memory_order_relaxed);
  auto* command_processor = context.d3d12.command_processor;
  for (PrewarmResult& r : done) {
    if (r.mesh_valid) {
      auto mit = g_r.meshes.find(r.item.mesh);
      const bool superseded =
          mit != g_r.meshes.end() && mit->second.dyn_seq > r.buffers.dyn_seq;
      if (r.item.ropa && r.buffers.dyn_seq != 0 && !superseded) {
        // Record the payload identity + mode this ropa decode pairs with:
        // the guest thread's flip-hold pass keeps publishing the previous
        // resolved state until the resident decode matches the published
        // mode (see g_ropa_resident). Also correct on the identical-
        // fingerprint drop below: the cached content equals this job's, so
        // the pairing is this job's mode either way.
        std::lock_guard<std::mutex> lock(g_ropa_resident_mutex);
        if (g_ropa_resident.size() > 512) {
          g_ropa_resident.clear();
        }
        RopaResidentDecode& res = g_ropa_resident[r.item.mesh];
        res.fp = r.buffers.fingerprint;
        res.skinned = r.item.skinned;
      }
      // ROPA shape-generation ring: retain this decode's vertex array
      // (keyed by dyn_seq) for the draw-time blend onto the play clock.
      // Runs even when the GPU buffers get dropped as identical below;
      // the SEQ still advances and the interp ring may reference it.
      if (r.item.ropa && r.buffers.dyn_seq != 0 && !superseded &&
          !r.buffers.ropa_verts.empty()) {
        auto& ring = g_r.ropa_shapes[r.item.mesh];
        ring.push_back({r.buffers.dyn_seq, std::move(r.buffers.ropa_verts)});
        // 16 generations = ~114 ms at a 140 Hz guest: the 8-tap boxcar
        // kernel reaches filter_w/2 (~25 ms) past the play clock (itself
        // ~2 guest periods behind), plus decode-latency slack.
        while (ring.size() > 16) {
          ring.pop_front();
        }
        if (g_r.ropa_shapes.size() > 64) {
          g_r.ropa_shapes.clear();  // outfit-change growth backstop
        }
      }
      if (mit != g_r.meshes.end() &&
          (mit->second.fingerprint == r.buffers.fingerprint || superseded)) {
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
      // Payload-stability verify: a decode read while its payload was still
      // STREAMING IN is a garbage interleave (the mip-churn "goes black,
      // then reloads" flash; fresh mip words repoint mid-upload, and the
      // fingerprint sampled right after the worker's read can look stable).
      // The commit runs 1-3 frames later: re-sample here and FAIL unstable
      // results, so the cache keeps the previous good decode and the retry
      // clock re-runs the heal once the payload settles. Cubes are exempt
      // (static assets; a failed cube negative-caches permanently).
      // UI-origin results skip the verify (see PrewarmEntry::ui): animating
      // APT art legitimately rewrites its payload every guest frame, so the
      // re-sample below would reject every mid-animation commit and freeze
      // the element; the 2D resolve's content probe is the heal path there.
      if (t.valid && !t.cube && !t.ui && verify_base != nullptr &&
          t.gt.payload_addr != 0 &&
          SampleProbeFingerprint(verify_base, t.gt) != t.gt.payload_fp) {
        if (t.gt.texture) {
          t.gt.texture->Release();
          t.gt.texture = nullptr;
        }
        if (t.gt.upload) {
          t.gt.upload->Release();
          t.gt.upload = nullptr;
        }
        t.valid = false;
        t.verify_failed = true;
        g_heal_verify_fail.fetch_add(1, std::memory_order_relaxed);
      }
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
        // Store commit: the result files under its decode-time words key
        // unconditionally; a rebound object simply routes elsewhere, so a
        // worker result can never land on the wrong identity. The only
        // remaining valid->valid swap class is an in-place content change
        // at the same words (event-ad rotation, mip-pool fills, composed
        // lightmap pages); the payload verify above covers exactly that.
        const bool tr_key =
            !g_trace_keys.empty() && g_trace_keys.count(t.words_key) != 0;
        auto wit = g_r.tex_store.find(t.words_key);
        if (tr_key) {
          REXLOG_INFO(
              "tex-trace: f{} COMMIT key={:016X} valid={} vfail={} "
              "fp={:016X} inc={} nb={} cached={}",
              frame_number, t.words_key, t.valid ? 1 : 0,
              t.verify_failed ? 1 : 0, t.gt.payload_fp,
              t.gt.incomplete ? 1 : 0, t.gt.near_black ? 1 : 0,
              wit != g_r.tex_store.end()
                  ? (wit->second.valid ? "valid" : "invalid")
                  : "none");
        }
        if (wit != g_r.tex_store.end()) {
          // Same-content dedup, except a complete re-decode always
          // displaces an incomplete cached entry (truncated tiled-mip copy:
          // the zeroed blocks live in mips the fingerprint never samples).
          const bool same_content = t.valid && wit->second.valid &&
                                    t.gt.payload_fp == wit->second.payload_fp &&
                                    !(wit->second.incomplete && !t.gt.incomplete);
          if (same_content || (!t.valid && wit->second.valid)) {
            // Keep the cached decode ("keep the old decode when the payload
            // became unreadable": mips stream out at range). A failed heal
            // of a still-serving entry needs no retry stamp: the payload
            // poll re-detects on its own cadence and the miss-inflight set
            // already dedupes.
            if (!t.valid && !t.verify_failed) {
              g_heal_decode_fail.fetch_add(1, std::memory_order_relaxed);
            }
            if (same_content && t.gt.near_black && wit->second.near_black &&
                wit->second.nb_redecodes < 255) {
              // A forced near-black re-decode came back identical: one more
              // confirmation toward "genuinely uniform content".
              ++wit->second.nb_redecodes;
            }
            if (t.gt.texture) t.gt.texture->Release();
            if (t.gt.upload) t.gt.upload->Release();
            continue;
          }
          if (t.valid && wit->second.valid) {
            // In-place content swap: the only commit class a player can
            // SEE; rolling-capped log so a flicker sighting names its
            // texture.
            static std::atomic<uint32_t> s_swap_logs{0};
            static std::atomic<int64_t> s_swap_win{0};
            const int64_t now_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            int64_t swin = s_swap_win.load(std::memory_order_relaxed);
            if (now_s - swin >= 5 &&
                s_swap_win.compare_exchange_strong(swin, now_s)) {
              s_swap_logs.store(0, std::memory_order_relaxed);
            }
            if (s_swap_logs.fetch_add(1) < 8) {
              REXLOG_INFO(
                  "native-scene: texture heal commit key={:016X} fp {:016X} "
                  "-> {:016X}",
                  t.words_key, wit->second.payload_fp, t.gt.payload_fp);
            }
          }
          if (!t.valid) {
            t.gt.fail_count = wit->second.fail_count;  // keep the backoff arc
          }
          t.gt.last_used_frame = wit->second.last_used_frame;
          RetireGuestTexture(wit->second, command_processor->GetCurrentSubmission());
          g_r.tex_store.erase(wit);
        }
        if (t.valid) {
          CommitStagedGuestTexture(context, t.gt, t.commit);
          committed_tex = true;
          // Content landed this frame; the video-start cold/hot classifier
          // (GuestTexture::last_change_frame) keys off commit times.
          t.gt.last_change_frame = frame_number;
        } else {
          t.gt.fail_count = BumpFail(t.gt.fail_count);
          t.gt.retry_after_frame = frame_number + RetryBackoff(t.gt.fail_count);
          // Failed decodes render white: log each once (capped) so white
          // meshes stay attributable to a specific texture.
          static std::unordered_set<uint64_t> logged_failed;
          if (logged_failed.size() < 64 &&
              logged_failed.insert(t.words_key).second) {
            REXLOG_INFO(
                "native-scene: texture decode FAILED key={:016X} "
                "fetch=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                t.words_key, t.gt.fetch_words[0], t.gt.fetch_words[1],
                t.gt.fetch_words[2], t.gt.fetch_words[3], t.gt.fetch_words[4],
                t.gt.fetch_words[5]);
          }
        }
        g_r.tex_store.emplace(t.words_key, t.gt);
        continue;
      }
      // No words key and not a cube: an empty/failed stage slot; release
      // whatever it carries (nothing routes to it).
      if (t.gt.texture) t.gt.texture->Release();
      if (t.gt.upload) t.gt.upload->Release();
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

// Render-thread mirror of the presence-context check (set in YieldForMenus):
// menu screens shorten the 2D texture liveness recheck cadence so in-place
// UI-texture rewrites (portrait resolves) heal fast.
std::atomic<bool> g_in_menus_frame{false};

// Set by YieldForMenus, consumed by RenderScene in the same call (render
// thread only): this frame is a NATIVE loading-screen frame; no world scene
// exists (or it is the previous map's stale one), so RenderScene renders a
// black backdrop + the captured 2D loading UI instead of the world.
bool g_loading_native_frame = false;
// Render thread only: the previous rendered frame was a native loading
// frame. Used to HOLD the loading visuals through the post-load takeover
// gate window; the emulated output was suppressed all through the native
// loading screen, so yielding there would flash the stale pre-load frame.
bool g_loading_hold = false;

// Menus / pause / loading yield gate (see the comment at the call site):
// returns true when RenderScene must yield this frame to the emulated
// output. Handles the cache clears on entry and the takeover re-arm +
// loading-screen pipeline build / prewarm commit while in a load, whether
// the loading pixels themselves render emulated (yield) or natively
// (g_loading_native_frame).
bool YieldForMenus(const NativeGuestOutputRenderContext& context) {
  static bool s_in_loading = false;
  static bool s_seen_gameplay = false;
  static bool s_pause_native = false;
  const bool in_menus = rex::graphics::ultrawide_debug::Skate3GameplayContextValue() == 0;
  // Render-thread mirror for the 2D texture resolver: menu screens shorten
  // the content-liveness recheck cadence (see resolve_2d_texture) so
  // in-place rewrites of UI textures (the one-shot skater-portrait resolves)
  // heal within a couple of frames instead of up to 16.
  g_in_menus_frame.store(in_menus, std::memory_order_relaxed);
  if (!in_menus) {
    s_seen_gameplay = true;
  }
  // In-game pause menu: the presence context reads 0, but the world keeps
  // resubmitting perspective scenes behind the menu (loading screens and the
  // boot frontend stop publishing): stay native there so the pause backdrop
  // renders natively and the caches survive the pause. The 2D pause UI rides
  // the same captured-APT overlay replay as the gameplay HUD. If publishes
  // go stale (a load was picked from the pause menu, or the game stops
  // redrawing the world), this degrades to the yield path below within
  // ~300 ms, cache clears and all.
  // boot_native lifts the first-gameplay prerequisite from both native menu
  // modes: a boot-frontend 3D backdrop renders like a pause backdrop, and
  // everything else (videos, menus, the first load) renders as 2D-over-black.
  const bool boot_native = REXCVAR_GET(skate3_native_render_scene_boot_native);
  bool pause_native = false;
  if (in_menus && (s_seen_gameplay || boot_native) &&
      REXCVAR_GET(skate3_native_render_scene_pause_native)) {
    const int64_t last_ns = g_last_publish_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    pause_native = last_ns >= 0 && now_ns - last_ns < 300'000'000;
  }
  if (pause_native != s_pause_native) {
    s_pause_native = pause_native;
    if (pause_native) {
      REXLOG_INFO(
          "native-scene: pause menu over live world - staying NATIVE "
          "(2d stats at entry: draws_2d={} other={} dropped={})",
          g_draws_2d.load(std::memory_order_relaxed),
          g_draws_2d_other.load(std::memory_order_relaxed),
          g_draws_2d_dropped.load(std::memory_order_relaxed));
    } else {
      REXLOG_INFO(
          "native-scene: leaving native pause ({}; 2d stats at exit: "
          "draws_2d={} other={} dropped={})",
          in_menus ? "scene publishes went stale - loading/frontend"
                   : "gameplay resumed",
          g_draws_2d.load(std::memory_order_relaxed),
          g_draws_2d_other.load(std::memory_order_relaxed),
          g_draws_2d_dropped.load(std::memory_order_relaxed));
    }
  }
  // Menu-context un-suppression: the game produces some content as one-shot
  // off-screen renders; the team-menu skater portrait boxes are a
  // render-to-texture pass issued once when the screen opens (and re-issued
  // after an edit). With emulated draws suppressed those passes never
  // execute, the resolve never writes the portrait texture, and the boxes
  // stay empty forever (the F11 emulated pair-shot showed the same empty box
  // - the texture is persistent guest state that was simply never filled).
  // While a menu context is up, clear the SDK suppress cvar so the emulated
  // pipeline keeps every RTT/composite current; the extra GPU cost is
  // menu-only. The saved value is restored on the first gameplay frame, so a
  // user toggle of the underlying cvar in the debug dialog survives (it is
  // re-read at each menu entry).
  {
    static bool s_unsup_forced = false;
    static bool s_unsup_saved = false;
    const bool want =
        in_menus && REXCVAR_GET(skate3_native_render_scene_menu_unsuppress);
    if (want && !s_unsup_forced) {
      s_unsup_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
      if (s_unsup_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, false);
        REXLOG_INFO(
            "native-scene: menu context - emulated draw suppression OFF "
            "(one-shot render-to-texture passes execute; restored on "
            "gameplay)");
      }
      s_unsup_forced = true;
    } else if (!want && s_unsup_forced) {
      if (s_unsup_saved) {
        REXCVAR_SET(native_render_suppress_emulated_draws, true);
        REXLOG_INFO(
            "native-scene: leaving menu context - emulated draw suppression "
            "restored");
      }
      s_unsup_forced = false;
    }
  }
  // Menu-context suppression FILTER relaxation (the near-native sibling of
  // the full lift above): mode 0 keeps the framebuffer passes suppressed,
  // the screen stays natively composed, but lets the sub-framebuffer RTT
  // passes execute, which is where the one-shot skater-portrait renders
  // live (their pitch sits inside the mode-2 suppressed band; with mode 2
  // active in menus the boxes stayed empty).
  {
    static bool s_mode_forced = false;
    static int32_t s_mode_saved = 0;
    const bool want =
        in_menus && REXCVAR_GET(skate3_native_render_scene_menu_rtt_passes);
    if (want && !s_mode_forced) {
      s_mode_saved = REXCVAR_GET(native_render_suppress_mode);
      if (s_mode_saved != 0) {
        REXCVAR_SET(native_render_suppress_mode, 0);
        REXLOG_INFO(
            "native-scene: menu context - suppress mode {} -> 0 (portrait "
            "RTT passes execute; restored on gameplay)",
            s_mode_saved);
      }
      s_mode_forced = true;
    } else if (!want && s_mode_forced) {
      if (s_mode_saved != 0) {
        REXCVAR_SET(native_render_suppress_mode, s_mode_saved);
        REXLOG_INFO("native-scene: leaving menu context - suppress mode {} restored",
                    s_mode_saved);
      }
      s_mode_forced = false;
    }
    // Same menu window: shader compilation goes SYNCHRONOUS. With
    // async_shader_compilation on, the d3d12 command processor SKIPS any
    // draw whose pipeline is still compiling (command_processor.cpp
    // ConfigurePipeline tail): fine mid-gameplay, but the skater-portrait
    // boxes are ONE-SHOT renders: pieces skipped during a first-run compile
    // are baked into the portrait forever (the armless/torso-less
    // skaters; later runs are fine because the
    // shader/pipeline disk storage is warm). Menus tolerate the one-time
    // compile stalls invisibly.
    static bool s_async_forced = false;
    static bool s_async_saved = false;
    if (want && !s_async_forced) {
      s_async_saved = REXCVAR_GET(async_shader_compilation);
      if (s_async_saved) {
        REXCVAR_SET(async_shader_compilation, false);
        REXLOG_INFO(
            "native-scene: menu context - shader compilation synchronous "
            "(one-shot portrait renders can't skip still-compiling pieces)");
      }
      s_async_forced = true;
    } else if (!want && s_async_forced) {
      if (s_async_saved) {
        REXCVAR_SET(async_shader_compilation, true);
      }
      s_async_forced = false;
    }
  }
  const bool in_loading = in_menus && !pause_native;
  // Loading screens themselves render natively too (black + the captured 2D
  // loading UI) when enabled, everything after the first gameplay. The
  // housekeeping below runs for the loading STATE either way; only the
  // yield decision changes.
  const bool loading_native =
      in_loading && (s_seen_gameplay || boot_native) &&
      REXCVAR_GET(skate3_native_render_scene_loading_native);
  g_loading_native_frame = loading_native;
  if (in_loading != s_in_loading) {
    s_in_loading = in_loading;
    if (in_loading) {
      REXLOG_INFO(
          "native-scene: menus/loading - {} (presence context)",
          loading_native ? "rendering the loading screen NATIVELY"
                         : "yielding to emulated output");
      // Arena addresses are reused across map loads: let the next load's
      // registrations re-queue meshes (and re-stage textures) at reused
      // addresses, and drop the cached item cores built from them.
      ClearItemCache();
      // Off-screen retention holds guest-address-keyed copies too.
      g_retained_clear.store(true, std::memory_order_relaxed);
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
  if (in_loading) {
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
    if (loading_native) {
      // RenderScene renders this frame (black + 2D loading UI) and runs
      // the prewarm commit itself with the loading budget.
      return false;
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
    return true;
  }
  return false;
}

  // Photo-mission photo editor (the "Pick a photo" screen with the depth of
  // field / saturation / brightness / contrast controls; FE screen class
  // PhotoSelect, challenge/photoselect.swf): the editor's effects ARE the
  // game's postfx chain, which native rendering suppresses, so natively
  // the photo showed the raw scene and the controls did nothing. Yield to
  // the emulated output while the editor is up: the emulated frame there is
  // complete and exact, and the scene is frozen so emulated-path
  // performance is fine. Unlike the menus branch above this touches no
  // caches and no takeover gates; native rendering resumes on the next
  // frame after the editor closes.
  //
  // Detection is a per-frame poll of the game's FE state (stateless, so it
  // can't get stuck): FrontEndManager singleton ptr global 0x830CFE14
  // (TU3; from SingletonHolder<FrontEndManager>::Instance = 824AD2F0),
  // whose NIS FE push-state stack (eastl::vector of 20-byte records at
  // +0x210) holds a {1, 11} record exactly while the photographer NIS has
  // the editor pushed. Surveyed across 50 gsnaps spanning gameplay, menus
  // and other missions: every non-editor record reads {x, -1}; only the
  // photo-editor capture shows a non-(-1) second field. The
  // PhotoReplayController heartbeat (sub_825623F0 hook) is kept as a
  // secondary signal for photo flows that bypass the photographer NIS.
// FMV playback (intro logos, any full-motion video): the frame is
// CPU-decoded into a texture (VideoRenderer_RwTexture Lock/Fill/Unlock) and
// reaches the screen through the game's postfx chain + swap; no capturable
// 2D draw exists (captured FMV frames show ~15 draws, all postfx
// passes + fade fills), so the native path has nothing to replay. Yield
// while the MovieDecoder::Decode heartbeat is fresh; the emulated frame is
// complete and correct there (photo-editor class). Touches no caches or
// takeover gates; native rendering resumes on the next frame after the
// movie ends.
bool YieldForMovie() {
  if (!REXCVAR_GET(skate3_native_render_scene_fmv_yield)) {
    return false;
  }
  const int64_t last_ns = g_movie_decode_last_ns.load(std::memory_order_relaxed);
  if (last_ns < 0) {
    return false;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             PerfClock::now().time_since_epoch())
                             .count();
  const bool active = now_ns - last_ns < 500'000'000;
  // LAST-RESORT gating: yielding INSTANTLY at video start presented the
  // emulated framebuffer, which under draw suppression still holds a stale
  // frame: the flash of the PREVIOUS video at every video boundary
  // (measured: yield, planes published +16 ms, substitution
  // native +38 ms, a ~5-frame stale-frame window). The substitution path
  // gets 400 ms from the first heartbeat to engage, and once it has drawn
  // recently the yield stays off entirely (covers the video-END race the
  // same way). Genuine native-FMV failure (fmv_native off, pso missing,
  // plane decode failure) still reaches the emulated yield after the
  // grace window.
  static int64_t s_fresh_since = -1;
  if (!active) {
    s_fresh_since = -1;
  } else if (s_fresh_since < 0) {
    s_fresh_since = now_ns;
  }
  const int64_t native_ns = g_movie_native_last_ns.load(std::memory_order_relaxed);
  const bool yield = active && now_ns - s_fresh_since >= 400'000'000 &&
                     (native_ns < 0 || now_ns - native_ns > 1'000'000'000);
  static bool s_active = false;
  if (yield != s_active) {
    s_active = yield;
    if (yield) {
      REXLOG_INFO(
          "native-scene: FMV playing - yielding to emulated output "
          "(MovieDecoder heartbeat; substitution did not engage)");
    } else {
      REXLOG_INFO("native-scene: FMV ended - native output resumes");
    }
  }
  return yield;
}

// The photo-editor detection, shared by the yield and the photo-grab
// readback window: nullptr when inactive, else the name of the signal.
const char* PhotoEditorSignal(uint8_t* base) {
  const char* signal = nullptr;
  {
    constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
    uint32_t mgr = 0, beg = 0, end = 0;
    if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
        GuestTryLoadU32(base, mgr + 0x210, &beg) &&
        GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
        end - beg <= 20 * 16) {
      const uint32_t n = (end - beg) / 20;
      for (uint32_t i = 0; i < n && signal == nullptr; ++i) {
        uint32_t f0 = 0, f1 = 0;
        if (GuestTryLoadU32(base, beg + i * 20, &f0) &&
            GuestTryLoadU32(base, beg + i * 20 + 4, &f1) && f0 == 1 && f1 == 11) {
          signal = "FE PhotoSelect push-state";
        }
      }
    }
  }
  if (signal == nullptr) {
    const int64_t last_ns = g_photo_replay_last_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerfClock::now().time_since_epoch())
                               .count();
    if (last_ns >= 0 && now_ns - last_ns < 250'000'000) {
      signal = "PhotoReplayController heartbeat";
    }
  }
  return signal;
}

// The photo-grab readback window (see skate3_native_render_scene_photo_readback):
// while the photo editor is up OR a TakePhoto fired within the last few
// seconds, (a) arm the SDK's forced small-resolve CPU readback so the
// resolved screenshot target actually lands in guest memory for the CPU
// JPEG encode, and (b) lift emulated-draw suppression so the passes that
// render that target execute even when the native output is active (a
// no-op while the photo yield has the native output inactive anyway).
// Runs every frame from RenderScene regardless of the yield decisions.
void UpdatePhotoGrabWindow(uint8_t* base) {
  // The grab target is 1152x640x4 = 0x2D0000 bytes (the same surface the
  // SDK's Import-Skater special case reads); the thumb path is smaller.
  // Framebuffer-sized resolves (0x384000 at 1280x720) stay excluded.
  constexpr int32_t kForceReadbackMaxLength = 0x2D0000;
  static bool s_armed = false;
  static int32_t s_readback_saved = 0;
  static bool s_suppress_saved = false;
  bool want = false;
  if (REXCVAR_GET(skate3_native_render_scene_photo_readback)) {
    want = PhotoEditorSignal(base) != nullptr;
    if (!want) {
      const int64_t last_ns = g_take_photo_last_ns.load(std::memory_order_relaxed);
      if (last_ns >= 0) {
        const int64_t now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                PerfClock::now().time_since_epoch())
                .count();
        want = now_ns - last_ns < 3'000'000'000;
      }
    }
  }
  // Same window arms the photo postfx constant capture (CapturePfxConstants
  // runs on the guest thread at the SetPending hook).
  g_photo_flow_frame.store(want, std::memory_order_relaxed);
  if (want && !s_armed) {
    s_readback_saved = REXCVAR_GET(native_render_force_resolve_readback_max_length);
    if (s_readback_saved <= 0) {
      REXCVAR_SET(native_render_force_resolve_readback_max_length,
                  kForceReadbackMaxLength);
    }
    s_suppress_saved = REXCVAR_GET(native_render_suppress_emulated_draws);
    if (s_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, false);
    }
    REXLOG_INFO(
        "native-scene: photo flow - forcing small-resolve CPU readback "
        "(the photo grab reads the resolved screenshot target from guest "
        "memory){}",
        s_suppress_saved ? " + emulated draw suppression OFF" : "");
    s_armed = true;
  } else if (!want && s_armed) {
    if (s_readback_saved <= 0) {
      REXCVAR_SET(native_render_force_resolve_readback_max_length,
                  s_readback_saved);
    }
    if (s_suppress_saved) {
      REXCVAR_SET(native_render_suppress_emulated_draws, true);
    }
    REXLOG_INFO(
        "native-scene: photo flow ended - resolve readback{} restored",
        s_suppress_saved ? " + suppression" : "");
    s_armed = false;
  }
}

bool YieldForPhotoEditor(uint8_t* base) {
  // The native photo-fx chain (photo_fx.hlsl, photo_native cvar) takes
  // precedence: the editor stays native and RenderScene applies the game's
  // postfx as exact ported passes with live-captured constants.
  const bool native_fx = REXCVAR_GET(skate3_native_render_scene_photo_native);
  if (!native_fx && !REXCVAR_GET(skate3_native_render_scene_photo_yield)) {
    return false;
  }
  static bool s_in_photo_editor = false;
  const char* signal = PhotoEditorSignal(base);
  const bool photo_active = signal != nullptr;
  if (photo_active != s_in_photo_editor) {
    s_in_photo_editor = photo_active;
    if (photo_active) {
      REXLOG_INFO(
          "native-scene: photo editor - {} ({})",
          native_fx ? "staying NATIVE (ported postfx chain)"
                    : "yielding to emulated output (the game's postfx applies "
                      "the photo effects)",
          signal);
    } else {
      REXLOG_INFO("native-scene: photo editor closed");
    }
  }
  if (native_fx) {
    return false;
  }
  return photo_active && REXCVAR_GET(skate3_native_render_scene_photo_yield);
}

// Create-a-skater editor (the 'Edit Skater' screen: skater + garage wall,
// Skin/Clothing/Body Mods panels): a special FE renderer the native scene
// does not model; the skater draws with editor-only CAC shader variants
// (cacstamp_skin_nisPS / cac_cloth_nisPS / cac_face_nisPS...) whose constant
// layouts differ from gameplay (the cacstamp map shifted +1 row: light c10,
// key c16, SH c25..c33 scale c22.y, tint c24, alpha c23.x, measured
// in capture), so every char-lighting capture is rejected and
// the skater rendered legacy-shaded (grey tank, pale skin, black jeans). The
// editor also runs per-frame texture-space composite passes (cac*_unwrapPS
// paint the edited garment/skin art into textures) and its own DOF postfx;
// live-edit previews are only correct with the full emulated chain. Yield
// while it is up (photo-editor class: scene is a small frozen room, emulated
// performance is fine; no cache or takeover-gate side effects; native
// resumes the frame after the editor closes, and the un-suppressed yield
// window also lets the game re-render the team-box skater portrait RTT that
// follows an accepted edit).
//
// Detection: stateless per-frame poll of the FrontEndManager push-state
// stack (same struct as YieldForPhotoEditor above) for a record with screen
// id 15, surveyed across the 40 gsnaps on hand (gameplay, pause root 56,
// team screen 63, photo editor {1,11}, FMV): id 15 appears exactly in the
// CAS editor capture and nowhere else.
bool YieldForCasEditor(uint8_t* base) {
  if (!REXCVAR_GET(skate3_native_render_scene_cas_yield)) {
    return false;
  }
  static bool s_in_cas_editor = false;
  const char* signal = nullptr;
  {
    constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
    uint32_t mgr = 0, beg = 0, end = 0;
    if (GuestTryLoadU32(base, kFrontEndManagerPtr, &mgr) && mgr != 0 &&
        GuestTryLoadU32(base, mgr + 0x210, &beg) &&
        GuestTryLoadU32(base, mgr + 0x214, &end) && beg < end &&
        end - beg <= 20 * 16) {
      const uint32_t n = (end - beg) / 20;
      for (uint32_t i = 0; i < n && signal == nullptr; ++i) {
        uint32_t f0 = 0;
        if (GuestTryLoadU32(base, beg + i * 20, &f0) && f0 == 15) {
          signal = "FE push-state id 15";
        }
      }
    }
  }
  if (signal == nullptr) {
    // Shader heartbeat: the editor's own "_nis" pixel shaders were set
    // within the last 0.5 s (see IsCasEditorPs): covers editor entry
    // points that use a different FE screen id (the startup new-game flow
    // stayed NATIVE on the FE detection alone).
    const int64_t last_ns = g_cas_ps_last_ns.load(std::memory_order_relaxed);
    if (last_ns >= 0) {
      const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
      if (now_ns - last_ns < 500'000'000) {
        signal = "CAS _nis shader heartbeat";
      }
    }
  }
  const bool cas_active = signal != nullptr;
  if (cas_active != s_in_cas_editor) {
    s_in_cas_editor = cas_active;
    if (cas_active) {
      REXLOG_INFO(
          "native-scene: create-a-skater editor - yielding to emulated "
          "output ({}; editor CAC shading + live composite passes render "
          "exactly there)",
          signal);
    } else {
      REXLOG_INFO(
          "native-scene: create-a-skater editor closed - native output "
          "resumes");
    }
  }
  return cas_active;
}

// Frees retired buffers / recycles retired SRV slots whose last
// referencing submission completed, and services the debug-dialog
// texture/mesh cache flushes.
void ReleaseRetiredAndFlushCaches(const NativeGuestOutputRenderContext& context) {
  auto* command_processor = context.d3d12.command_processor;
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
    for (auto& [key, t] : g_r.tex_store) {
      RetireGuestTexture(t, submission);
    }
    g_r.tex_store.clear();
    g_r.tex_routes.clear();
    g_r.words_sticky.clear();
    g_r.tex_sticky.clear();
    g_r.tex_pending_first.clear();
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
}

  // ---- Dynamic-shadow atlas pass ----
  // Renders the frame's dynamic casters (skinned characters + rigid
  // non-identity-world props: exactly the game's caster list) into the
  // three cascade tiles with the captured light rows, then applies the
  // game's coverage blur + depth dilation. Runs before the main pass so the
  // scene shader can sample the finished atlas.
bool RenderShadowAtlas(const NativeGuestOutputRenderContext& context,
                       const FrameScene& scene, uint32_t bone_region,
                       int32_t debug_mode, uint32_t* out_draws) {
  auto& list = context.d3d12.command_processor->GetDeferredCommandList();
  const float* sh = scene.shadow_rows;
  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  if (REXCVAR_GET(skate3_native_render_scene_shadows) && scene.shadow_valid &&
      g_r.shadow_raw != nullptr && g_r.pso_shadow_caster != nullptr &&
      g_r.pso_shadow_blur != nullptr && debug_mode == 0) {
    struct Caster {
      const DrawItem* item;
      uint32_t bone_offset;
      bool bones;
    };
    std::vector<Caster> casters;
    for (const DrawItem& item : scene.items) {
      if (item.transparent || item.unlit || item.cloth_quads) {
        continue;
      }
      const bool skinned = item.skinned && !item.bones.empty();
      // The game's CSM casts only DYNAMIC content: characters/vehicles
      // (skinned), Ropa cloth, and dynamicobject props (the truck's caster
      // draws, F10-verified). Static world scenery has its shadows BAKED
      // into the lightmaps, and that includes world-PLACED instances
      // (streetlights, trees, rails), which carry real world transforms.
      // The old identity-matrix test let every placed prop into the caster
      // pass, painting phantom streetlight-head/tree silhouettes onto the
      // plaza glass towers (a "reflection-like" soft dark blob high on the
      // facade, geometrically impossible as a real shadow: sun at 47 deg,
      // lamp 8 m tall, blob at 96 m; the emulated frame has no such
      // shadow).
      if (!skinned && item.dynobj == 0 && !item.ropa && item.char_family == 0) {
        continue;
      }
      // Entities the game is holding invisible (spawn settle / distance
      // fade, see CharFadeAlpha) must not paint a shadow either; the
      // emulated frame shows neither the NPC nor a blob under it.
      if (item.char_family != 0 && CharFadeAlpha(item) < 0.05f &&
          REXCVAR_GET(skate3_native_render_scene_entity_fade)) {
        continue;
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
  *out_draws = shadow_draws;
  return shadow_ready;
}

  // Selection-outline mask (see kOutlineShaderSource): re-render the frame's
  // selected items into the small R8 target while the scene pass state is
  // still bound. The edge composite runs after the resolve, on the
  // single-sample output.
bool RenderOutlineMask(const NativeGuestOutputRenderContext& context,
                       const FrameScene& scene, const D3D12_VIEWPORT& viewport,
                       const D3D12_RECT& scissor, bool msaa_on,
                       D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv,
                       D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool use_depth) {
  auto& list = context.d3d12.command_processor->GetDeferredCommandList();
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
  return outline_ready;
}

// Selection-outline composite: additive stencil-edge-detect over the
// resolved output (before the popup blur, like the game's postfx order).
void RenderOutlineComposite(const NativeGuestOutputRenderContext& context,
                            const FrameScene& scene,
                            D3D12_CPU_DESCRIPTOR_HANDLE output_rtv,
                            const D3D12_VIEWPORT& viewport,
                            const D3D12_RECT& scissor) {
  auto& list = context.d3d12.command_processor->GetDeferredCommandList();
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

// 600-frame perf + telemetry log lines (verbatim from the former tail of
// RenderScene).
void LogFrameStats(const FrameScene& scene, uint64_t frames, uint32_t drawn,
                   uint32_t drawn_2d, uint32_t drawn_spline, bool shadow_ready,
                   uint32_t shadow_draws) {
  if (frames % 600 == 0) {
    // CPU-side perf snapshot for this 600-frame window. guest_fps is derived
    // from the guest frame interval; capture/build run on the guest render
    // thread (they extend guest frame time directly), render/items/shadow on
    // the command processor thread, decode inline on the render thread
    // (count = decodes this window; max = the worst single decode).
    const double guest_dt_ms = g_pw_guest_dt.AvgMs();
    REXLOG_INFO(
        "native-scene perf: guest_fps={:.0f} guest_dt_max={:.1f}ms "
        "capture={:.2f}/{:.2f}ms build={:.2f}/{:.2f}ms v3={:.2f}/{:.2f}ms | render={:.2f}/{:.2f}ms "
        "items={:.2f}/{:.2f}ms shadow={:.2f}/{:.2f}ms "
        "decode[mesh n={} avg={:.2f} max={:.2f}ms tex n={} avg={:.2f} max={:.2f}ms] "
        "commit={:.2f}/{:.2f}ms itemcache[hit={} build={}] cam[chg={} rep={} maxstreak={}]",
        guest_dt_ms > 0.0 ? 1000.0 / guest_dt_ms : 0.0, g_pw_guest_dt.MaxMs(),
        g_pw_capture.AvgMs(), g_pw_capture.MaxMs(), g_pw_build.AvgMs(),
        g_pw_build.MaxMs(), g_pw_v3.AvgMs(), g_pw_v3.MaxMs(),
        g_pw_render.AvgMs(), g_pw_render.MaxMs(),
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
    for (PerfWindow* w : {&g_pw_guest_dt, &g_pw_capture, &g_pw_build, &g_pw_v3,
                          &g_pw_render, &g_pw_items, &g_pw_shadow,
                          &g_pw_mesh_decode, &g_pw_tex_decode, &g_pw_commit}) {
      w->Reset();
    }
  }
  if (frames % 600 == 0) {
    uint32_t lw_ctxs = 0, lw_ents = 0;
    skate3::native_lw::QueryLwStats(&lw_ctxs, &lw_ents);
    REXLOG_INFO(
        "native-scene: frame {} items={} draws={} draws_2d={} drawn_2d={} "
        "splines[{}/{}] "
        "2d[other={} dropped={} askip={} astale={} textures={}] cached_meshes={} textures={} "
        "vs_uploads={} palettes={} palette_base_plus1={} ropa[rigid={} stale={} rescued={} flip={} mismatch={} relax={} hold={} caster={} incoh={} stretch={} blend={} blendmiss={}] dyn_gap={} skinned={} skinned_skipped={} foreign_bank={} "
        "rigid[pending={} dropped={} worldprops={}] "
        "rej[dyn={} range={} chain={} geom={} draws={} bbox={}] "
        "rr[decode_fail={} no_bones={} mesh_deferred={} tex_deferred={}] "
        "store[n={} routes={} evict={}] "
        "heal[vfail={} dfail={} demote={}] serve[sticky={} skipnew={} adstale={} adnone={}] "
        "shadow[valid={} ready={} draws={}] char[attempt={} valid={} drawn={} reused={} "
        "bones_rescued={}] dynobj[valid={} drawn={}] "
        "lw[ctxs={} ents={} stamp={} fade0={} resc={} fill={} pal={} rows={}] "
        "refl[pair={} flat={} gate={:#x}]",
        frames, scene.items.size(), drawn, g_draws_2d.load(), drawn_2d,
        drawn_spline, g_draws_spline.load(),
        g_draws_2d_other.load(), g_draws_2d_dropped.load(),
        g_2d_async_skip.load(), g_2d_async_stale.load(), g_r.tex_store.size(),
        g_r.meshes.size(), g_r.tex_store.size(),
        g_vs_uploads.load(), g_palette_snapshots.load(), g_palette_base_plus1.load(),
        g_ropa_rigid.load(), g_ropa_stale.load(), g_ropa_rescued.load(),
        g_ropa_flip.load(), g_ropa_mismatch.load(), g_ropa_relaxed.load(),
        g_ropa_hold.load(), g_ropa_caster.load(), g_pub_incoherent.load(),
        g_stretch_veto.load(),
        g_ropa_blend_drawn.load(), g_ropa_blend_miss.load(), g_dyn_gap.load(),
        g_skinned_items.load(),
        g_skinned_skipped.load(), g_capture_foreign_bank.load(),
        g_rigid_pending.load(), g_rigid_dropped.load(),
        g_world_props.load(),
        g_rej_no_dynstate.load(), g_rej_dyn_range.load(),
        g_rej_chain.load(), g_rej_geom.load(), g_rej_draws.load(), g_rej_bbox.load(),
        g_rr_decode_fail.load(), g_rr_no_bones.load(), g_rr_mesh_deferred.load(),
        g_rr_tex_deferred.load(), g_r.tex_store.size(), g_r.tex_routes.size(),
        g_store_evicted.load(), g_heal_verify_fail.load(),
        g_heal_decode_fail.load(), g_demote_hold.load(),
        g_tex_sticky_served.load(), g_skip_new.load(), g_ad_stale_served.load(),
        g_ad_placeholder.load(), scene.shadow_valid,
        shadow_ready, shadow_draws,
        g_char_attempts.load(), g_char_valid.load(), g_char_drawn.load(),
        g_char_rows_reused.load(), g_bones_rescued.load(), scene.dynobj_valid,
        g_dynobj_drawn.load(), lw_ctxs, lw_ents, g_lw_stamped.load(),
        g_lw_fade0.load(), g_lw_ctx_rescued.load(), g_lw_gap_filled.load(),
        g_lw_pal_sub.load(), g_lw_rows_served.load(),
        g_refl_pair.load(), g_refl_flat.load(), g_refl_gate.load());
  }
}

bool RenderScene(const NativeGuestOutputRenderContext& context, void* /*user_data*/) {
  if (!SceneEnabled() || context.backend != NativeGuestOutputBackend::kD3D12) {
    return false;
  }
  // While the game reports menus / loading (presence context 0x8001 == 0),
  // yield to the emulated output, EXCEPT the in-game pause menu (world
  // still publishing perspective scenes), which stays native when
  // skate3_native_render_scene_pause_native is on: the pause UI rides the
  // same captured-APT 2D replay as the gameplay HUD, and the backdrop is the
  // ordinary native world. Loading screens and the boot frontend still
  // render emulated (complete and correct there).
  // The photo-grab readback window runs before any yield decision so it
  // updates every frame in every mode (the grab must work whether the photo
  // flow renders yielded-emulated or native).
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (base != nullptr) {
    UpdatePhotoGrabWindow(base);
  }
  if (YieldForMenus(context)) {
    return false;
  }
  if (base == nullptr) {
    return false;
  }
  if (YieldForPhotoEditor(base)) {
    return false;
  }
  if (YieldForCasEditor(base)) {
    return false;
  }

  const auto render_t0 = PerfClock::now();
  const auto perf_ns_since = [](PerfClock::time_point t0) {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t0)
            .count());
  };

  bool loading_native = g_loading_native_frame;
  // Render the takeover-gate window (warmup armed, no fresh substantial
  // scene yet) as a native loading frame instead of yielding, in two cases:
  // (a) g_loading_hold: the frames right after a native loading screen
  //     (the presence context flips to gameplay before the first fresh
  //     scene, and the emulated output was suppressed all through the load,
  //     so yielding would flash its stale pre-load content);
  // (b) boot_native: the whole startup flow (intro videos, boot frontend)
  //     runs with the gate armed and no scene published, and should render
  //     natively as 2D-over-black rather than fall back to emulated.
  if (!loading_native &&
      (g_loading_hold || REXCVAR_GET(skate3_native_render_scene_boot_native)) &&
      REXCVAR_GET(skate3_native_render_scene_warmup_budget_ms) > 0 &&
      g_warmup_armed.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_scene_mutex);
    // A fresh CAS-editor scene qualifies at ANY size: the startup-flow
    // editor is skater-only (~10 items, below warmup_min_items) but is a
    // complete, deliberate scene; holding it rendered the 3D view black
    // behind the live editor menu. It renders WITHOUT disarming the gate
    // (see the takeover gate below); real gameplay still needs min_items.
    const bool fresh =
        g_scene && g_scene->generation >= g_warmup_fresh_generation;
    const bool ready =
        fresh && (g_scene->items.size() >=
                      size_t(REXCVAR_GET(skate3_native_render_scene_warmup_min_items)) ||
                  (!g_scene->items.empty() && CasEditorActive(base)));
    loading_native = !ready;
  }
  g_loading_hold = loading_native;
  std::shared_ptr<const FrameScene> scene_ptr;
  if (loading_native) {
    // Native loading screen: there is no current world scene (g_scene holds
    // the PREVIOUS map's stale one); render an empty scene, i.e. a black
    // backdrop, and let the 2D overlay tail replay the game's live loading
    // UI (Publish2dDraws publishes every guest frame, loads included).
    // Value-initialized: zero items, no shadow/blur/outline, generation 0.
    static const std::shared_ptr<const FrameScene> s_loading_scene =
        std::make_shared<const FrameScene>();
    scene_ptr = s_loading_scene;
  } else {
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

  // FMV routing: prefer the native path; video quads in the 2D replay are
  // SUBSTITUTED with the ps_yuv2d combine, matched by their own captured
  // slot-0 fetch (== that video's Y plane; through ps_main a movie quad
  // renders as an opaque black cover, intro, or slow greyscale luma,
  // camera-page previews). Order-faithful: backdrop fills land under the
  // video like the emulated frame; multiple simultaneous videos each match
  // their own plane set. Only when this path is unavailable does a live
  // movie heartbeat yield to the emulated output.
  MoviePlanes movies[kMaxMovies];
  {
    std::lock_guard<std::mutex> lock(g_movie_mutex);
    std::memcpy(movies, g_movies, sizeof(movies));
  }
  const int64_t movie_now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerfClock::now().time_since_epoch())
          .count();
  bool movie_fresh = false;
  for (const MoviePlanes& m : movies) {
    movie_fresh |= m.ns >= 0 && movie_now_ns - m.ns < 500'000'000;
  }
  // Movie SESSION start (publish-freshness OFF->ON edge = a video began):
  // the triple substitution only serves plane content DECODED DURING THIS
  // SESSION (GuestTexture::last_change_frame >= this stamp). The APT plane
  // copies keep their addresses across videos, so at video N+1's start
  // both the store AND guest memory can still hold video N's last frame;
  // serving either flashed the previous video for a few frames at every
  // boundary. Until the new video's first frame lands (content change ->
  // inline re-decode), the quad holds black, which is what a starting
  // video looks like.
  static uint64_t s_movie_session_frame = 0;
  {
    static bool s_movie_fresh_prev = false;
    if (movie_fresh && !s_movie_fresh_prev) {
      s_movie_session_frame = g_frames_rendered.load(std::memory_order_relaxed);
    }
    s_movie_fresh_prev = movie_fresh;
  }
  const bool movie_sub = movie_fresh &&
                         REXCVAR_GET(skate3_native_render_scene_fmv_native) &&
                         g_r.pso_yuv2d != nullptr;
  if (!movie_sub && YieldForMovie()) {
    return false;
  }

  auto* command_processor = context.d3d12.command_processor;
  auto& list = command_processor->GetDeferredCommandList();

  ReleaseRetiredAndFlushCaches(context);

  // Reset this frame's bone ring region (shared by the shadow casters and
  // the main pass: the shadow pass allocates first, the main pass appends).
  const uint64_t frame_number = g_frames_rendered.load(std::memory_order_relaxed);
  const uint32_t bone_region =
      uint32_t(frame_number % RendererState::kBoneRegions) *
      RendererState::kBoneRegionSize;
  g_r.bone_ring_offset = 0;
  g_r.ropa_ring_offset = 0;

  {
    const std::string tm(REXCVAR_GET(skate3_native_render_scene_trace_mesh));
    const uint32_t parsed =
        tm.empty() ? 0u : uint32_t(std::strtoul(tm.c_str(), nullptr, 16));
    if (parsed != g_trace_mesh_addr) {
      g_trace_mesh_addr = parsed;
      g_trace_keys.clear();
      g_trace_sig.clear();
      REXLOG_INFO("tex-trace: mesh={:08X} {}", parsed,
                  parsed ? "TRACING" : "off");
    }
  }

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
  // The takeover gates judge REAL scenes only; a native loading frame
  // renders its empty scene deliberately (the gates re-run as usual once
  // the presence context flips back to gameplay).
  if (warmup_ms > 0 && !loading_native &&
      REXCVAR_GET(skate3_native_render_scene_debug) == 0) {
    if (g_warmup_armed.load(std::memory_order_relaxed)) {
      const bool small_or_stale =
          scene.generation < g_warmup_fresh_generation ||
          scene.items.size() <
              size_t(REXCVAR_GET(skate3_native_render_scene_warmup_min_items));
      // A fresh CAS-editor scene renders despite being under min_items (the
      // startup-flow editor is skater-only, ~10 items), but does NOT count
      // as the gameplay takeover; the gate stays armed for the real load
      // that follows the editor.
      const bool editor_scene = scene.generation >= g_warmup_fresh_generation &&
                                !scene.items.empty() && CasEditorActive(base);
      if (small_or_stale && !editor_scene) {
        // Stale or fade-in scene: yield (brief, a few frames). Keep
        // committing worker results meanwhile; every pre-takeover frame
        // counts on map changes.
        PrewarmCommit(context, frame_number);
        return false;
      }
      if (!small_or_stale) {
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
  // serve the draw path's steady-state misses (EnqueueMeshMiss/
  // EnqueueWordsMiss), so make sure they exist even on a session that never
  // showed a loading screen with the pipeline up.
  EnsurePrewarmWorkers();
  // Native loading frames take the loading-screen commit budget; the heavy
  // decode lifting behind the load is unchanged from the yielded path.
  PrewarmCommit(context, frame_number, /*loading=*/loading_native);
  // Content-store LRU: superseded words states (old mip levels, pre-demote
  // detail sets, one-shot UI art) age out once nothing routes to them.
  EvictTexStore(frame_number, command_processor->GetCurrentSubmission());

  bool shadow_ready = false;
  uint32_t shadow_draws = 0;
  const auto shadow_t0 = PerfClock::now();
  const float* sh = scene.shadow_rows;
  const int32_t debug_mode = REXCVAR_GET(skate3_native_render_scene_debug);
  shadow_ready =
      RenderShadowAtlas(context, scene, bone_region, debug_mode, &shadow_draws);
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
  // Loading frames clear to black (the game's loading UI composes over
  // black); real scenes keep the sky-ish debug clear that shows through
  // undecoded holes.
  const FLOAT clear_color[4] = {loading_native ? 0.0f : 0.25f,
                                loading_native ? 0.0f : 0.35f,
                                loading_native ? 0.0f : 0.55f, 1.0f};
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
  // F7 scene-composition ring (RequestSceneRingDump): one compact signature
  // per scene item per frame, ~900 frames deep. A 1-2 frame artifact (the
  // dam-bank blue flash) is uncapturable by F10/F11; the ring lets a
  // keypress seconds later name exactly which item appeared / vanished /
  // changed textures on the artifact frame. Entries are recorded at
  // classification below; draw_item stamps the issued-draw count, so an
  // item that early-returned (deferred mesh, skip-new, fade skip) shows
  // drawn=0 that frame.
  SceneRingFrame* ring_frame = nullptr;
  std::unordered_map<const DrawItem*, uint32_t> ring_map;
  if (REXCVAR_GET(skate3_native_render_scene_ring) && debug_mode == 0) {
    g_scene_ring.emplace_back();
    while (g_scene_ring.size() > kSceneRingFrames) {
      g_scene_ring.pop_front();
    }
    ring_frame = &g_scene_ring.back();
    ring_frame->frame = frame_number;
    std::memcpy(ring_frame->cam, scene.cam_pos, sizeof(ring_frame->cam));
    ring_frame->fog[0] = scene.fog_ramp[0];
    ring_frame->fog[1] = scene.fog_ramp[1];
    ring_frame->fog[2] = scene.fog_ramp[2];
    ring_frame->fog[3] = scene.fog_color[0];
    ring_frame->fog[4] = scene.fog_color[1];
    ring_frame->fog[5] = scene.fog_color[2];
    std::memcpy(ring_frame->family_rows, scene.family_rows,
                sizeof(ring_frame->family_rows));
    ring_frame->sky_height = scene.sky_height;
    ring_frame->shadow_valid = scene.shadow_valid;
    ring_frame->items.reserve(scene.items.size());
    ring_map.reserve(scene.items.size());
  }
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
    auto it = g_r.tex_store.find(key);
    if (it != g_r.tex_store.end()) {
      it->second.last_used_frame = frame_number;
    }
    if (it != g_r.tex_store.end() && it->second.valid &&
        frame_number >= it->second.recheck_frame &&
        REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
      // 2-frame cadence in menu/editor contexts, like the item-draw poll
      // (in-place CAS composite rewrites during edits).
      it->second.recheck_frame =
          frame_number +
          (g_in_menus_frame.load(std::memory_order_relaxed) ? 2 : 16);
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      const bool fp_new = fp != 0 && fp != it->second.payload_fp;
      if (it->second.recheck_count < 3) {
        ++it->second.recheck_count;
      }
      if (it->second.incomplete || fp_new) {
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
  // `site` identifies the consuming slot (mesh << 1 | slot) for the sticky
  // fallback: streaming rebinds NEW mip words as you approach the art (the
  // cache key changes wholesale), and dropping to the placeholder / channel
  // diffuse for the worker round trip was the visible poster/decal flash;
  // the site's previous art keeps serving until the new decode lands.
  // Returns null only when nothing ever decoded for this site.
  const auto resolve_fetch_words = [&](const uint32_t words[6], uint64_t site,
                                       bool retained) -> const GuestTexture* {
    const uint64_t fkey = FetchWordsKey(words);
    auto fit = find_words_texture(fkey);
    if (fit == g_r.tex_store.end()) {
      if (!retained) {
        EnqueueWordsMiss(fkey, words);
      }
    } else if (fit->second.valid) {
      g_r.words_sticky[site] = fkey;
      return &fit->second;
    } else if (!retained && frame_number >= fit->second.retry_after_frame) {
      // Failed words decode (payload was mid-stream at first sight): keep
      // retrying; without this the entry negative-cached until the words
      // changed again.
      EnqueueWordsMiss(fkey, words);
      fit->second.retry_after_frame = frame_number + 30;
    }
    const auto pit = g_r.words_sticky.find(site);
    if (pit != g_r.words_sticky.end() && pit->second != fkey) {
      const auto old = g_r.tex_store.find(pit->second);
      if (old != g_r.tex_store.end() && old->second.valid) {
        old->second.last_used_frame = frame_number;
        g_ad_stale_served.fetch_add(1, std::memory_order_relaxed);
        return &old->second;
      }
    }
    // Nothing decoded for this site yet: the caller falls back to the
    // channel diffuse (the baked placeholder poster). If the "decal flash
    // and replace" report survives the dense-probe fix, a climbing none=
    // with a capped log here names this path as the flasher.
    g_ad_placeholder.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_ad_logs{0};
    if (!retained && s_ad_logs.fetch_add(1) < 24) {
      REXLOG_INFO(
          "native-scene: ad/decal words site={:X} fkey={:016X} -> placeholder "
          "(no decode yet) words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
          site, fkey, words[0], words[1], words[2], words[3], words[4],
          words[5]);
    }
    return nullptr;
  };

  const auto draw_item = [&](const DrawItem& item) {
    // NO per-frame inline decodes here. Static content (world geometry,
    // props) loads/heals on the decode workers via the miss queue; a
    // texture decode averages ~10 ms and panning surfaces dozens of new
    // payloads in one frame; inline decode WAS the panning lag spike.
    // Dynamic payloads (skinned, CPU-rewritten every frame) are kept
    // fresh by the dyn decode jobs (guest-thread snapshot -> worker), one
    // frame behind the sim; only their FIRST sight decodes inline. The
    // cloth-quads particle path (gated off by default) still re-decodes
    // inline on change; it has no job route.
    // EXCEPTION: ROPA garments decode INLINE (skate3_native_render_scene_
    // ropa_inline): the worker route put the GPU-resident cloth shape 1-2
    // frames behind the body, visible as jelly/clip-through-torso during
    // direction changes (the shape only pauses when motion is steady). A
    // garment decode is sub-millisecond (a few hundred verts; it was
    // lumped in with the expensive texture decodes in the perf overhaul),
    // and the game's ping-pong double buffer makes the LIVE read tear-safe
    // (the sim writes the other half).
    const bool ropa_inline =
        item.ropa && REXCVAR_GET(skate3_native_render_scene_ropa_inline);
    const bool dynamic_payload = item.skinned || item.cloth_quads || item.ropa;
    auto it = g_r.meshes.find(item.mesh);
    if (item.retained &&
        (it == g_r.meshes.end() || it->second.fingerprint != item.fingerprint)) {
      // Retained off-screen items (edge-of-view guard band) outlive their
      // guest-side lifetime guarantees; the arena may have streamed out or
      // been reused since capture. Draw only the exact cached decode; no
      // guest reads, no heals, no miss enqueues from here.
      return;
    }
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint &&
        REXCVAR_GET(skate3_native_render_scene_mesh_revalidate)) {
      if (item.cloth_quads || ropa_inline) {
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
      if (!item.cloth_quads && !ropa_inline) {
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
    // reflective_trans glass draws in the blended sub-pass whenever its
    // exact branch is live (same gate as the sub-pass routing below); the
    // opaque cull-PSO reset must not fire there.
    const bool refl_trans_pass =
        item.env_family == 13 && debug_mode == 0 && scene.shadow_valid;
    // Character items mid-fade (spawn settle / distance) draw in the blended
    // sub-pass too (same gate as the routing below), same exemption.
    const bool char_fade_pass =
        (item.char_family == 1 || item.char_family == 2 ||
         item.char_family == 3 || item.char_family == 6) &&
        debug_mode == 0 && CharFadeAlpha(item) < 0.999f &&
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    if (use_depth && !item.transparent && !item.water && !hair_pass &&
        !refl_trans_pass && !char_fade_pass && g_r.pso_cullback != nullptr) {
      const float* w = item.world;
      const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                         w[1] * (w[4] * w[10] - w[6] * w[8]) +
                         w[2] * (w[4] * w[9] - w[5] * w[8]);
      // Game-parity backface culling: every world environment material's
      // XML sets CULLMODE=FRONT (== our CULL_FRONT; the banner work
      // calibrated game-kept faces as our D3D12 BACK faces). CULL_NONE
      // showed interior/away faces the game never renders, e.g. the
      // building wall's inside face stacking behind the translucent canopy
      // glass. Trees/alphatest (fams 7/9/10: leaf/fence cards read from
      // both sides) and mirrored instances (flipped winding) stay
      // uncull(ed), matching the two-sided-sheet rules.
      const bool cull_family =
          REXCVAR_GET(skate3_native_render_scene_backface_cull) &&
          item.env_family != 0 && item.env_family != 7 &&
          item.env_family != 9 && item.env_family != 10 &&
          item.env_family != 13;
      ID3D12PipelineState* want =
          ((buffers.two_sided_sheet || cull_family) && det3 >= 0.0f)
              ? g_r.pso_cullback
              : g_r.pso;
      if (want != scene_pso_bound) {
        list.D3DSetPipelineState(want);
        scene_pso_bound = want;
      }
    }

    // Resolve guest textures (white fallback) through the words-keyed
    // content store: object -> stable live words -> store entry
    // (console identity semantics; a
    // retargeted or reused object is just a different key, never a stale
    // serve). The object addresses were readable on the game thread this
    // frame; NO VirtualQuery here; it takes the process VAD lock, which
    // the guest streaming threads hammer, and ~2 calls per item stalled
    // the whole renderer to 3 fps.
    // Set by resolve_texture_raw when it returns the white fallback while a
    // decode is in flight (first-sight miss or a still-failing heal); the
    // sticky wrapper below then serves the item's last-good texture instead.
    bool tex_pending = false;
    const auto resolve_texture_raw = [&](uint32_t tex_ptr) -> const GuestTexture* {
      if (tex_ptr == 0) {
        return &g_r.white;
      }
      const bool trm = g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr;
      auto rit = g_r.tex_routes.find(tex_ptr);
      if (!item.retained) {
        // Route refresh: seqlock-stable read of the live fetch words (a
        // mid-rewrite mixed snapshot must never become a key; it would
        // decode a coherent image of the WRONG memory, the pool-page
        // collage class). An unstable read keeps the previous route for a
        // frame. Retained items skip all live reads: the object may be
        // gone; their route is frozen at retention.
        uint32_t live[6];
        if (ReadStableTexWords(base, tex_ptr, live)) {
          const bool demoted = (live[1] & 0xFFFFF000u) == 0u;
          if (rit == g_r.tex_routes.end()) {
            rit = g_r.tex_routes.emplace(tex_ptr, RendererState::TexRoute{})
                      .first;
            std::memcpy(rit->second.words, live, sizeof(live));
            rit->second.key = FetchWordsKey(live);
            rit->second.demoted = demoted;
          } else if (demoted) {
            // Mip-0 demoted (base address cleared; the old pool range is
            // already reused): hold the pre-demote route; its decode
            // carries the full chain, strictly better, and suspend its
            // payload polls (the probes would read the reused pool and
            // heal in foreign bytes). A re-promote publishes fresh words
            // and re-routes.
            if (!rit->second.demoted) {
              rit->second.demoted = true;
              g_demote_hold.fetch_add(1, std::memory_order_relaxed);
              if (trm) {
                REXLOG_INFO("tex-trace: f{} obj={:08X} DEMOTED (route held, "
                            "polls suspended)",
                            frame_number, tex_ptr);
              }
            }
          } else if (std::memcmp(live, rit->second.words, sizeof(live)) != 0) {
            if (trm) {
              REXLOG_INFO(
                  "tex-trace: f{} obj={:08X} REROUTE old=[{:08X} {:08X} "
                  "{:08X} {:08X} {:08X} {:08X}] new=[{:08X} {:08X} {:08X} "
                  "{:08X} {:08X} {:08X}]",
                  frame_number, tex_ptr, rit->second.words[0],
                  rit->second.words[1], rit->second.words[2],
                  rit->second.words[3], rit->second.words[4],
                  rit->second.words[5], live[0], live[1], live[2], live[3],
                  live[4], live[5]);
            }
            std::memcpy(rit->second.words, live, sizeof(live));
            rit->second.key = FetchWordsKey(live);
            rit->second.demoted = false;
          } else {
            rit->second.demoted = false;
          }
        } else if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} words UNSTABLE (mid-rewrite)",
                      frame_number, tex_ptr);
        }
      }
      if (rit == g_r.tex_routes.end()) {
        // Unreadable/unstable object with no prior route: nothing safe to
        // serve or decode yet; next frame's read settles it.
        tex_pending = true;
        return &g_r.white;
      }
      const RendererState::TexRoute& route = rit->second;
      auto sit = g_r.tex_store.find(route.key);
      if (sit == g_r.tex_store.end()) {
        if (item.retained || route.demoted) {
          // Retained: no enqueues, no live reads. Demote-held with nothing
          // cached: the pre-demote pool may already be reused; a decode
          // would commit foreign bytes under a good key. White/sticky until
          // a re-promote publishes live words.
          tex_pending = true;
          return &g_r.white;
        }
        // Decode on the workers; white/sticky for the 1-3 frames that takes
        // (inline decode measured ~10 ms avg / ~70 ms max, the panning
        // lag spikes).
        if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} MISS key={:016X} enqueued",
                      frame_number, tex_ptr, route.key);
        }
        EnqueueWordsMiss(route.key, route.words);
        g_rr_tex_deferred.fetch_add(1, std::memory_order_relaxed);
        tex_pending = true;
        return &g_r.white;
      }
      GuestTexture& e = sit->second;
      e.last_used_frame = frame_number;
      if (!e.valid) {
        // Failed decode: retry on its backoff clock (the payload usually
        // lands within a few frames of first sight; the commit stamps the
        // real backoff).
        if (!item.retained && !route.demoted &&
            frame_number >= e.retry_after_frame) {
          e.retry_after_frame = frame_number + 120;
          EnqueueWordsMiss(route.key, route.words);
        }
        if (trm) {
          REXLOG_INFO("tex-trace: f{} obj={:08X} INVALID entry key={:016X} "
                      "(retry at f{})",
                      frame_number, tex_ptr, route.key, e.retry_after_frame);
        }
        tex_pending = true;
        return &g_r.white;
      }
      // Payload revalidation, the one irreducible heuristic: the game
      // streams content IN PLACE at addresses the words already point to
      // (event-ad rotation, mip-pool fills, composed lightmap pages) with
      // no CPU-visible event. Escalating cadence (2/4/8 then 16): a decode
      // taken while the payload was still streaming reads back garbage;
      // fresh entries re-verify fast, then settle to the cheap steady
      // cadence. An incomplete decode (truncated tiled-mip copy) and a
      // near_black verdict re-decode regardless of the mip-0 fingerprint;
      // the commit's same-content dedup absorbs the no-ops. Suspended while
      // the route is demote-held (the probes would read the reused pool).
      if (!item.retained && !route.demoted &&
          frame_number >= e.recheck_frame &&
          REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
        // Menu/editor contexts poll STEADY entries on a 2-frame cadence
        // (mirrors the 2D resolver): the CAS editor recomposes the skater's
        // skin/garment textures IN PLACE progressively over ~a second on
        // every edit, and the 16-frame steady cadence made each composite
        // step land ~100+ ms late, desynchronized across the pieces, the
        // "broken for 1-2 s after changing skin color" state. Gameplay
        // keeps the cheap steady cadence.
        e.recheck_frame =
            frame_number +
            (e.recheck_count < 3
                 ? (2ull << e.recheck_count)
                 : (g_in_menus_frame.load(std::memory_order_relaxed) ? 2ull
                                                                     : 16ull));
        const uint64_t fp = SampleProbeFingerprint(base, e);
        const bool fp_new = fp != 0 && fp != e.payload_fp;
        if (trm) {
          REXLOG_INFO(
              "tex-trace: f{} obj={:08X} poll key={:016X} fp={:016X} "
              "cached={:016X} new={} inc={} nb={} cnt={}",
              frame_number, tex_ptr, route.key, fp, e.payload_fp,
              fp_new ? 1 : 0, e.incomplete ? 1 : 0, e.near_black ? 1 : 0,
              e.recheck_count);
        }
        if (e.recheck_count < 3) {
          ++e.recheck_count;
        }
        if (fp_new || e.incomplete || (e.near_black && e.nb_redecodes < 3)) {
          // Re-decode churn diagnostic: repeated in-place payload changes
          // on one key = streaming oscillation, visible as texture flicker
          // on the affected meshes.
          static std::atomic<uint32_t> s_redecode_logs{0};
          if (s_redecode_logs.fetch_add(1) < 256) {
            REXLOG_INFO(
                "native-scene: texture re-decode key={:016X} reason={}{}{} "
                "words=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                route.key, fp_new ? "fp" : "", e.incomplete ? "inc" : "",
                e.near_black ? "nb" : "", e.fetch_words[0], e.fetch_words[1],
                e.fetch_words[2], e.fetch_words[3], e.fetch_words[4],
                e.fetch_words[5]);
          }
          if (trm) {
            REXLOG_INFO("tex-trace: f{} obj={:08X} ENQUEUE heal key={:016X} "
                        "reason={}{}{}",
                        frame_number, tex_ptr, route.key, fp_new ? "fp" : "",
                        e.incomplete ? "inc" : "", e.near_black ? "nb" : "");
          }
          EnqueueWordsMiss(route.key, e.fetch_words);
        }
      }
      return &e;
    };
    // Sticky serving: streaming rotates content onto NEW texture objects (a
    // mip promote is usually a fresh object: 287 first-sight objects vs 73
    // in-place rebinds in one 45 s traversal), so a plain
    // cache miss white-flashed content that was on screen with the previous
    // mip one frame earlier. While the new object's decode is in flight,
    // serve the last texture this item slot successfully resolved, the
    // same visual as the console's own mip transition. slot: 0 diffuse,
    // 1 lightmap, 2 macro, 3 normal/ripple, 4 decal art, 5 hair, 6 spec.
    const auto resolve_texture = [&](uint32_t tex_ptr,
                                     uint32_t slot) -> const GuestTexture* {
      tex_pending = false;
      const GuestTexture* t = resolve_texture_raw(tex_ptr);
      // Near-uniform-black decodes on the WHITE-NEUTRAL slots (1 lightmap,
      // 2 macro) serve the white fallback until a heal lands real content.
      // Lightmap: a real-but-black page binds with tint.r > 0 and the CSM
      // min-clamp renders the surface BLACK (the door 59810af's shader gate
      // cannot see). Macro: since 59810af the weathering multiplies OVER
      // the composited decal art, so a mid-stream dark macro decode turns
      // the paint into the black-square flash; white is the macro's
      // authored neutral (materials without weathering bind default_white).
      // Slots with legitimately dark content (diffuse, decal art, spec)
      // are untouched. Applied in the wrapper so retained items get the
      // same protection.
      const bool trm = g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr;
      if ((slot == 1 || slot == 2) && t != &g_r.white && t->valid &&
          t->near_black) {
        if (trm) {
          REXLOG_INFO("tex-trace: f{} slot={} obj={:08X} NEAR-BLACK -> white",
                      frame_number, slot, tex_ptr);
        }
        static std::atomic<uint32_t> s_nb_logs{0};
        if (s_nb_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
          REXLOG_INFO(
              "native-scene: near-black decode obj={:08X} slot={} served as "
              "white fallback (mid-compose/stream content)",
              tex_ptr, slot);
        }
        return &g_r.white;
      }
      if (item.retained || tex_ptr == 0) {
        return t;
      }
      const uint64_t skey = (uint64_t(item.mesh) << 3) | slot;
      if (t != &g_r.white) {
        RendererState::TexStickySite& site = g_r.tex_sticky[skey];
        // Material-detail downgrade hold: the game's streaming demotes a
        // nearby mesh to its UN (undetailed) material for a fraction of a
        // second and back, the visible flash to completely different/
        // blurry art. Under words identity a demote is just a smaller-area
        // key; the site's last-adopted decode is still resident in the
        // store, so keep serving it while the downgrade is young. A
        // persistent downgrade (a real demote as the player leaves) adopts
        // after the hold window.
        const int32_t hold = REXCVAR_GET(skate3_native_render_scene_detail_hold);
        const uint64_t cur_key = FetchWordsKey(t->fetch_words);
        const uint64_t cur_area = FetchWordsArea(t->fetch_words);
        if (hold > 0 && site.area > cur_area && site.words_key != 0) {
          const auto hit = g_r.tex_store.find(site.words_key);
          const GuestTexture* held =
              hit != g_r.tex_store.end() && hit->second.valid ? &hit->second
                                                              : nullptr;
          if (held != nullptr) {
            // The held entry may no longer be polled by any live route;
            // re-probe its payload on its own recheck clock so a reused
            // pool page abandons the hold instead of serving foreign
            // bytes.
            hit->second.last_used_frame = frame_number;
            if (frame_number >= hit->second.recheck_frame) {
              hit->second.recheck_frame = frame_number + 16;
              const uint64_t fp = SampleProbeFingerprint(base, hit->second);
              if (fp == 0 || fp != hit->second.payload_fp) {
                held = nullptr;
              }
            }
          }
          if (held != nullptr) {
            if (site.downgrade_since == 0) {
              site.downgrade_since = frame_number;
              static std::atomic<uint32_t> s_hold_logs{0};
              if (s_hold_logs.fetch_add(1, std::memory_order_relaxed) < 24) {
                REXLOG_INFO(
                    "native-scene: detail-downgrade HOLD mesh={:08X} slot={} "
                    "held={:016X} bound={:016X} area {} -> {}",
                    item.mesh, slot, site.words_key, cur_key, site.area,
                    cur_area);
              }
            }
            if (frame_number - site.downgrade_since < uint64_t(hold)) {
              if (trm && ((frame_number - site.downgrade_since) & 63u) == 0) {
                REXLOG_INFO(
                    "tex-trace: f{} slot={} DETAIL-HOLD serving fp={:016X} "
                    "(bound area {} < {})",
                    frame_number, slot, held->payload_fp, cur_area,
                    site.area);
              }
              return held;
            }
          }
        }
        if (trm && site.words_key != 0 && site.words_key != cur_key) {
          REXLOG_INFO(
              "tex-trace: f{} slot={} ADOPT key {:016X}(area {}) -> "
              "{:016X}(area {})",
              frame_number, slot, site.words_key, site.area, cur_key,
              cur_area);
        }
        site.words_key = cur_key;
        site.area = cur_area;
        site.downgrade_since = 0;
        if (!g_r.tex_pending_first.empty()) {
          g_r.tex_pending_first.erase(tex_ptr);
        }
        return t;
      }
      if (tex_pending) {
        // The current binding's decode is in flight: serve the site's
        // previous art from the store, the console's own mip-transition
        // look instead of a white flash.
        const auto sit = g_r.tex_sticky.find(skey);
        if (sit != g_r.tex_sticky.end() && sit->second.words_key != 0) {
          const auto old = g_r.tex_store.find(sit->second.words_key);
          if (old != g_r.tex_store.end() && old->second.valid) {
            old->second.last_used_frame = frame_number;
            g_tex_sticky_served.fetch_add(1, std::memory_order_relaxed);
            if (trm) {
              REXLOG_INFO("tex-trace: f{} slot={} STICKY serving key={:016X} "
                          "(pending {:08X})",
                          frame_number, slot, sit->second.words_key, tex_ptr);
            }
            return &old->second;
          }
        }
      }
      if (trm && t == &g_r.white) {
        REXLOG_INFO("tex-trace: f{} slot={} serving WHITE (req {:08X})",
                    frame_number, slot, tex_ptr);
      }
      return t;
    };
    // Streamed-artwork diffuse override (see DrawItem::diffuse_fetch): the
    // real art exists only as draw-time fetch words; resolve those through
    // the words-keyed cache (shared with the 2D pass; the art has no guest
    // object to key on).
    const GuestTexture* diffuse =
        item.diffuse_fetch[1] != 0
            ? resolve_fetch_words(item.diffuse_fetch, uint64_t(item.mesh) << 1,
                                  item.retained)
            : nullptr;
    if (diffuse == nullptr) {
      diffuse = resolve_texture(item.diffuse_tex, 0);
      if (diffuse == &g_r.white && tex_pending && item.diffuse_tex != 0 &&
          !item.retained) {
        // Brand-new content, first decode in flight, and no previous mip
        // cached to serve: draw NOTHING for a short window instead of a
        // white flash: the emulated pop-in semantics (the game shows
        // streamed content only once its data is ready). Bounded so a
        // permanently failing texture still falls back to visible white.
        const auto [pit, fresh] =
            g_r.tex_pending_first.try_emplace(item.diffuse_tex, frame_number);
        if (frame_number - pit->second < 20) {
          g_skip_new.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    }
    const GuestTexture* lightmap =
        item.lightmap_tex != 0 && REXCVAR_GET(skate3_native_render_scene_lightmaps)
            ? resolve_texture(item.lightmap_tex, 1)
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
            ? resolve_texture(item.macro_tex, 2)
            : &g_r.white;
    if ((item.water || item.char_family >= 6) && item.water_normal != 0) {
      // Water rides its ripple normal map in the macro slot (water never
      // carries a macro overlay; overlay.z stays 0 below so the macro
      // composite path never runs). Vehicles do the same with their DXN
      // panel normal map; without it the hinged panels' vertex normals
      // face away from the sun and shade as a dark ambient-blue patch that
      // stops at the door seams (the exact PS with a FLAT map reproduces
      // that artifact; with the real map it matches the emulated car).
      macro_tex = resolve_texture(item.water_normal, 3);
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
         (item.env_family >= 5 && item.env_family <= 6) ||
         item.env_family == 13) &&
        item.water_env != 0) {
      auto cit = g_r.cube_textures.find(item.water_env);
      if (cit == g_r.cube_textures.end()) {
        if (!item.retained) {
          EnqueueCubeMiss(item.water_env);
        }
      } else if (cit->second.valid) {
        cube_tex = &cit->second;
      }
    }
    const GuestTexture* decal_tex = item.decal && item.decal_art != 0 &&
                                            REXCVAR_GET(skate3_native_render_scene_decals)
                                        ? resolve_texture(item.decal_art, 4)
                                        : &g_r.white;
    // Streamed-artwork decal override (see DrawItem::decal_fetch): ad frames
    // covered by an environment.decal section get the current event-ad art
    // bound over the decal channel at draw time.
    if (item.decal && item.decal_fetch[1] != 0 &&
        REXCVAR_GET(skate3_native_render_scene_decals)) {
      const GuestTexture* ad = resolve_fetch_words(
          item.decal_fetch, (uint64_t(item.mesh) << 1) | 1, item.retained);
      if (ad != nullptr) {
        decal_tex = ad;
      }
    }
    if (item.char_family >= 4 && item.char_family <= 5 &&
        item.hair_alpha_tex != 0) {
      // Hair strand coverage rides the (otherwise unused) decal slot; the
      // PS hair branch samples it at the raw second texcoord. The white
      // fallback keeps failed decodes opaque rather than invisible.
      decal_tex = resolve_texture(item.hair_alpha_tex, 5);
    }
    // F7 ring: stamp the SERVED content fingerprints (see SceneRingItem);
    // pointer identity cannot see an in-place content swap.
    if (ring_frame != nullptr) {
      const auto rit = ring_map.find(&item);
      if (rit != ring_map.end()) {
        SceneRingItem& ri = ring_frame->items[rit->second];
        const auto fp_of = [](const GuestTexture* t) -> uint64_t {
          return t != nullptr && t->valid ? t->payload_fp : 0;
        };
        ri.fp_diffuse = fp_of(diffuse);
        ri.fp_lightmap = fp_of(lightmap);
        ri.fp_macro = fp_of(macro_tex);
        ri.fp_decal = fp_of(decal_tex);
      }
    }
    if (g_trace_mesh_addr != 0 && item.mesh == g_trace_mesh_addr) {
      // Traced-mesh per-frame summary: SERVED state (post-resolve), logged
      // when anything changes. The mesh's texture objects also register for
      // the worker-commit trace.
      const auto fp_of = [](const GuestTexture* t) -> uint64_t {
        return t != nullptr && t->valid ? t->payload_fp : 0;
      };
      for (const GuestTexture* t :
           {diffuse, lightmap, macro_tex, decal_tex}) {
        if (t != nullptr && t != &g_r.white) {
          g_trace_keys.insert(FetchWordsKey(t->fetch_words));
        }
      }
      uint64_t sig = uint64_t(item.diffuse_tex) ^
                     (uint64_t(item.decal_art) << 16) ^
                     (uint64_t(item.lightmap_tex) << 32) ^
                     (uint64_t(item.macro_tex) << 48);
      sig ^= fp_of(diffuse) * 3 ^ fp_of(decal_tex) * 5 ^ fp_of(lightmap) * 7 ^
             fp_of(macro_tex) * 11;
      sig ^= item.retained ? 1 : 0;
      uint64_t& last = g_trace_sig[item.ctx];
      if (sig != last) {
        last = sig;
        REXLOG_INFO(
            "tex-trace: f{} SERVED ctx={:08X} retained={} "
            "dif={:08X}/{:016X} dec={:08X}/{:016X} lm={:08X}/{:016X} "
            "mac={:08X}/{:016X}",
            frame_number, item.ctx, item.retained ? 1 : 0, item.diffuse_tex,
            fp_of(diffuse), item.decal_art, fp_of(decal_tex),
            item.lightmap_tex, fp_of(lightmap), item.macro_tex,
            fp_of(macro_tex));
      }
    }
    // Exact env families without decal art bind the material's spec/ecc/
    // refmask map (or the animated.tree noise tint) in the free decal slot;
    // overlay.w == 3 tells the shader the masks are live.
    bool spec_bound = false;
    if (item.env_family != 0 && !item.decal && item.env_family != 10 &&
        item.spec_tex != 0) {
      const GuestTexture* spec = resolve_texture(item.spec_tex, 6);
      if (spec != &g_r.white) {
        decal_tex = spec;
        spec_bound = true;
      }
    }
    // Sky dome: the material's `specular` channel is the 1D radial sun
    // gradient (512x16), bound in the free decal slot for the exact sky
    // branch. Until it decodes the dome falls back to the plain fullbright
    // panorama (sunless for the 1-3 frames in flight).
    bool sky_sun_bound = false;
    if (item.unlit && item.spec_tex != 0) {
      const GuestTexture* sun = resolve_texture(item.spec_tex, 6);
      if (sun != &g_r.white) {
        decal_tex = sun;
        sky_sun_bound = true;
      }
    }
    const bool is_decal =
        item.char_family < 4 && item.decal && decal_tex != &g_r.white;
    // Fam 5/6 masks+normal descriptor pair (t4/t5): the reflective PS
    // perturbs its reflection/spec with the material's normal map (shader
    // overlay.w == 4 branch: the fix for the giant flat-normal cube smear
    // on glass facades). Both views are re-created into the pair's two
    // consecutive heap slots once per frame, so prewarm/revalidation
    // texture swaps can never leave a stale descriptor. Until the normal
    // map decodes, overlay.w stays 3 (flat-normal reflection, the old
    // behavior).
    uint32_t pair_base = 0;
    bool normal_paired = false;
    if (item.env_family >= 5 && item.env_family <= 6) {
      uint32_t gate = 0;
      if (!spec_bound) gate |= 1;
      if (item.water_normal == 0) gate |= 2;
      if (spec_bound && (!decal_tex->valid || decal_tex->srv_mips == 0)) gate |= 4;
      if (gate == 0) {
        const GuestTexture* nrm = resolve_texture(item.water_normal, 3);
        if (nrm == &g_r.white) {
          gate |= 8;
        } else {
          if (!nrm->valid) gate |= 16;
          if (nrm->srv_mips == 0) gate |= 32;
        }
        if (gate == 0) {
          const uint64_t pkey =
              (uint64_t(item.spec_tex) << 32) | item.water_normal;
          auto pit = g_r.mat_pairs.find(pkey);
          if (pit == g_r.mat_pairs.end() && g_r.srv_next + 2 <= 8192) {
            pit = g_r.mat_pairs.emplace(pkey, std::make_pair(g_r.srv_next, ~0ull))
                      .first;
            g_r.srv_next += 2;
          }
          if (pit == g_r.mat_pairs.end()) {
            gate |= 64;
          } else {
            if (pit->second.second != frame_number) {
              pit->second.second = frame_number;
              const auto make_view = [&](const GuestTexture* t, uint32_t slot_idx) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = t->srv_format;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = t->srv_mapping;
                srv.Texture2D.MipLevels = t->srv_mips;
                D3D12_CPU_DESCRIPTOR_HANDLE h =
                    g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += size_t(slot_idx) * g_r.srv_size;
                g_r.device->CreateShaderResourceView(t->texture, &srv, h);
              };
              make_view(decal_tex, pit->second.first);
              make_view(nrm, pit->second.first + 1);
            }
            pair_base = pit->second.first;
            normal_paired = true;
          }
        }
      }
      if (normal_paired) {
        g_refl_pair.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_refl_flat.fetch_add(1, std::memory_order_relaxed);
        g_refl_gate.store(gate, std::memory_order_relaxed);
      }
    }
    constants[44] = item.macro_scale;
    constants[45] = item.macro_opacity;
    constants[46] = macro_tex != &g_r.white ? 1.0f : 0.0f;
    // overlay.w: 1 = single-placement decal (art clamps), 2 = tileable
    // decal (art wraps; clamping a many-period uv range stretched the
    // border texels into the giant cliff-face streaks), 3 = spec masks
    // bound (exact env families).
    constants[47] = is_decal ? (item.decal_tileable ? 2.0f : 1.0f)
                             : (spec_bound ? (normal_paired ? 4.0f : 3.0f)
                                           : 0.0f);
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
    } else if ((item.env_family >= 5 && item.env_family <= 6) ||
               item.env_family == 13) {
      // misc.y = cube LOD bias for the reflective families: the guest's
      // cube fetch computes its gradient LOD at the game's own 1152x640
      // render; at 4K our reflection-vector gradients are ~3.4x smaller
      // per pixel, so mips alone leave baked cube detail (the streetlight
      // heads) visible that the console blurs away.
      // misc.z/misc.w = the F12 reflection-isolation controls (see the
      // refl_mode cvar; both spare on opaque items; the fog packing only
      // uses these slots on transparent/water).
      constants[49] =
          log2f(std::max(1.0f, float(context.guest_output_height) / 640.0f));
      constants[50] = float(REXCVAR_GET(skate3_native_render_scene_refl_mode));
      constants[51] = float(REXCVAR_GET(skate3_native_render_scene_refl_lod));
      // misc.x (spare on opaque fam 5/6): both constant normal-tilt trims,
      // fixed-point packed (each mapped to 0..999 around 500; float-exact).
      // Only when the EXACT branch will run (same gate as cam_pos.w = -fam)
      // - the legacy fallback reads misc.x as the transparent/water flag.
      // Fam 13 has no normal map (its PS reflects off the vertex normal),
      // so its misc.x stays 0.
      if (debug_mode == 0 && scene.shadow_valid && item.env_family != 13) {
        double bx = REXCVAR_GET(skate3_native_render_scene_refl_bias_x);
        double by = REXCVAR_GET(skate3_native_render_scene_refl_bias_y);
        if (REXCVAR_GET(skate3_native_render_scene_refl_bias_auto) &&
            item.detail_tex != 0) {
          // Derive the fold from the material's own detail texture: its
          // first BC1 block decoded with hardware bit replication, texels
          // averaged, folded as 2*d - 1. Cached per texture object; the
          // packed-tile quirk of <=16px textures is benign here (neighbor
          // packed mips of a constant texture hold the same constant).
          // Implausible results (non-DXT1 / unreadable / |fold| > 0.1)
          // keep the cvar values.
          static std::unordered_map<uint32_t, std::pair<float, float>> fold_cache;
          auto fit = fold_cache.find(item.detail_tex);
          if (fit == fold_cache.end()) {
            std::pair<float, float> fold{float(bx), float(by)};
            uint32_t raw[6];
            if (GuestTryCopy(raw, base + item.detail_tex + 7 * 4, sizeof(raw))) {
              const uint32_t w0 = SwapU32(raw[0]);
              const uint32_t w1 = SwapU32(raw[1]);
              uint8_t block[8];
              if ((w0 & 3) == 2 && (w1 & 0x3F) == 0x12 &&
                  GuestTryCopy(block,
                               base + (0xA0000000u | ((w1 >> 12) << 12)), 8)) {
                uint8_t le[8];
                for (int k = 0; k < 8; k += 2) {  // k_8in16 guest endianness
                  le[k] = block[k + 1];
                  le[k + 1] = block[k];
                }
                uint8_t px[16][4];
                DecodeBc1Block(le, px);
                float ax = 0.0f, ay = 0.0f;
                for (int k = 0; k < 16; ++k) {
                  ax += px[k][0];
                  ay += px[k][1];
                }
                const float fx = (ax / 16.0f) * (2.0f / 255.0f) - 1.0f;
                const float fy = (ay / 16.0f) * (2.0f / 255.0f) - 1.0f;
                if (std::fabs(fx) < 0.1f && std::fabs(fy) < 0.1f) {
                  fold = {fx, fy};
                }
                REXLOG_INFO(
                    "native-scene: detail fold tex={:08X} = ({:+.6f}, {:+.6f})",
                    item.detail_tex, fx, fy);
              }
            }
            fit = fold_cache.emplace(item.detail_tex, fold).first;
          }
          bx = fit->second.first;
          by = fit->second.second;
        }
        const auto pack_trim = [](double v) {
          int i = int(std::lround(v * 1000.0)) + 500;
          return std::clamp(i, 0, 999);
        };
        constants[48] = float(pack_trim(bx) + 1000 * pack_trim(by));
      }
    }
    // cam_pos.w = -40 selects the exact sky branch: the game computes the
    // sun glow inside the dome shader (see the PS sky branch). Needs the
    // frame's captured sky sun rows AND the 1D gradient bound above; falls
    // back to the legacy fullbright dome otherwise. The sky item never uses
    // mat_tint/overlay/misc, so those root constants carry the sun rows.
    if (debug_mode == 0 && item.unlit && sky_sun_bound && scene.sky_sun_valid) {
      constants[39] = -40.0f;
      constants[40] = scene.sky_sun[0];  // mat_tint.xyz = sun direction
      constants[41] = scene.sky_sun[1];
      constants[42] = scene.sky_sun[2];
      constants[43] = scene.sky_height;  // mat_tint.w = dome viewpos Y
      constants[44] = scene.sky_sun[3];  // overlay.x = sun angular scale
      constants[45] = scene.sky_sun[4];  // overlay.y = pre-tone multiplier
      constants[46] = 0.0f;
      constants[47] = 0.0f;
      constants[49] = scene.sky_sun[5];  // misc.y = scene exposure
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
    decal_gpu.ptr +=
        size_t(normal_paired ? pair_base : decal_tex->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 5, decal_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE cube_gpu = heap_start;
    cube_gpu.ptr += size_t(cube_tex->srv_slot) * g_r.srv_size;
    context.d3d12.set_graphics_root_descriptor_table(
        context.d3d12.command_processor_user_data, 8, cube_gpu);
    // ROPA shape blend (see RendererState::ropa_shapes): combine the shape
    // generations with the kernel weights InterpolateDynamicItems computed
    // (the SAME 8-tap boxcar / pair-lerp the body bones and garment world
    // took this frame) into the per-frame ropa upload ring and draw from
    // it; the stepped cloth shape against the interpolated body was the
    // tee jelly/clip-through, and a filter-MISMATCHED blend (plain lerp vs
    // boxcar body) kept a period-scaled residue of it. Positions/normals
    // (floats 0..6) blend; packed attributes (blend words, uvs: floats
    // 7..13) copy from the newest generation present. Generations missing
    // from the ring (evicted / decode in flight) renormalize over what IS
    // present when at least half the kernel's weight survives.
    D3D12_VERTEX_BUFFER_VIEW item_vbv = buffers.vb_view;
    if (item.ropa && item.shape_count > 0 &&
        REXCVAR_GET(skate3_native_render_scene_ropa_blend)) {
      const std::vector<float>* gv[DrawItem::kShapeGens] = {};
      float gw[DrawItem::kShapeGens] = {};
      int ng = 0;
      float total = 0.0f;
      uint64_t newest_seq = 0;
      const std::vector<float>* newest = nullptr;
      // Generations must match the RESIDENT decode's extent exactly; the
      // index buffer references that many vertices. After a re-stream/
      // outfit swap the ring briefly holds stale-size generations; binding
      // one against the current IB reads past the bound VB and collapses
      // triangles (a momentary partial-invisible blink). Stale sizes drop
      // out here; if too much kernel weight is stale, the raw resident VB
      // draws instead (always self-consistent).
      const size_t want_floats =
          size_t(buffers.vb_view.SizeInBytes) / sizeof(float);
      const auto rit = g_r.ropa_shapes.find(item.mesh);
      if (rit != g_r.ropa_shapes.end()) {
        for (int k = 0; k < item.shape_count; ++k) {
          for (const RendererState::RopaGen& g : rit->second) {
            if (g.seq != item.shape_seq[k]) {
              continue;
            }
            if (g.verts.size() != want_floats) {
              break;  // stale-size generation (re-stream/outfit swap)
            }
            gv[ng] = &g.verts;
            gw[ng] = item.shape_w[k];
            total += item.shape_w[k];
            ++ng;
            if (g.seq >= newest_seq) {
              newest_seq = g.seq;
              newest = &g.verts;
            }
            break;
          }
        }
      }
      const uint32_t region =
          uint32_t(frame_number % RendererState::kBoneRegions) *
          RendererState::kRopaRegionSize;
      const uint32_t bytes = uint32_t(want_floats * sizeof(float));
      if (ng > 0 && total >= 0.5f && newest != nullptr &&
          buffers.vb_view.StrideInBytes == 56 &&
          g_r.ropa_ring_offset + bytes <= RendererState::kRopaRegionSize) {
        float* dst = reinterpret_cast<float*>(g_r.ropa_ring_cpu + region +
                                              g_r.ropa_ring_offset);
        const float inv = 1.0f / total;
        for (size_t v = 0; v + 14 <= want_floats; v += 14) {
          float blend7[7] = {};
          for (int k = 0; k < ng; ++k) {
            const float* src = gv[k]->data() + v;
            const float wk = gw[k] * inv;
            for (int f = 0; f < 7; ++f) {
              blend7[f] += src[f] * wk;
            }
          }
          // One store per float: dst is write-combined upload memory.
          std::memcpy(dst + v, blend7, sizeof(blend7));
          std::memcpy(dst + v + 7, newest->data() + v + 7, 7 * sizeof(float));
        }
        item_vbv.BufferLocation = g_r.ropa_ring->GetGPUVirtualAddress() +
                                  region + g_r.ropa_ring_offset;
        item_vbv.SizeInBytes = bytes;
        item_vbv.StrideInBytes = buffers.vb_view.StrideInBytes;
        g_r.ropa_ring_offset += (bytes + 255u) & ~255u;
        g_ropa_blend_drawn.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_ropa_blend_miss.fetch_add(1, std::memory_order_relaxed);
      }
    }
    list.D3DIASetVertexBuffers(0, 1, &item_vbv);
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
      if (ring_frame != nullptr) {
        const auto rit = ring_map.find(&item);
        if (rit != ring_map.end()) {
          ++ring_frame->items[rit->second].drawn;
        }
      }
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
  // Mid-fade entity items (see CharFadeAlpha / pso_fade): blended but with
  // z-write ON, drawn at the HEAD of the blended sub-pass; they behave like
  // main-pass objects whose glass/hair still composites over them, and depth
  // writes stop their own overlapping pieces (skin under clothes, far-side
  // doors/wheels through the body shell) from double-blending into an x-ray.
  const auto char_fade_zwrite = [&](const DrawItem& it) {
    return debug_mode == 0 && it.char_rows[14 * 4 + 1] > 0.0f &&
           (it.char_family == 1 || it.char_family == 2 ||
            it.char_family == 3 || it.char_family == 6) &&
           REXCVAR_GET(skate3_native_render_scene_entity_fade) &&
           CharFadeAlpha(it) < 0.999f;
  };
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
    if (ring_frame != nullptr) {
      SceneRingItem ri{};
      ri.ctx = item.ctx;
      ri.mesh = item.mesh;
      ri.diffuse = item.diffuse_tex;
      ri.lightmap = item.lightmap_tex;
      ri.vb_obj = item.vb_obj;
      ri.spec = item.spec_tex;
      ri.macro = item.macro_tex;
      ri.decal_art = item.decal_art;
      ri.wnormal = item.water_normal;
      uint32_t idx_total = 0;
      for (const DrawEntry& e : item.draws) {
        idx_total += e.index_count;
      }
      ri.indices = idx_total;
      ri.env_family = item.env_family;
      ri.char_family = item.char_family;
      ri.flags = uint8_t((item.transparent ? 1u : 0u) | (item.water ? 2u : 0u) |
                         (item.skinned ? 4u : 0u) | (item.retained ? 8u : 0u) |
                         (item.pending ? 16u : 0u) |
                         (item.caster_bank ? 32u : 0u) |
                         (item.decal ? 64u : 0u));
      ring_map.emplace(&item, uint32_t(ring_frame->items.size()));
      ring_frame->items.push_back(ri);
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
    // Per-entity spawn/distance fade (CharFadeAlpha): the game submits
    // LivingWorld entity draws at alpha 0 through the whole spawn settle
    // (NPCs drop ~1 m to the ground before fading in) and ramps alpha with
    // distance; skip invisible items entirely, blend mid-fade ones.
    // LW-mapped items (lw_alpha >= 0) do not need a validated lighting
    // capture to honor the fade: the entity alpha is authoritative even
    // when the capture chain failed; a spawn-settling NPC is invisible
    // regardless of whether its rows validated this frame.
    const bool entity_fade = debug_mode == 0 && item.char_family != 0 &&
                             (char_capture_ok || item.lw_alpha >= 0.0f) &&
                             REXCVAR_GET(skate3_native_render_scene_entity_fade);
    const float fade_a = entity_fade ? CharFadeAlpha(item) : 1.0f;
    // One-frame fade-blink guard: a mesh that rendered ~opaque last frame
    // cannot legitimately sit at alpha 0 this frame; the game's spawn/
    // distance fades ramp over ~0.5 s. An opaque->0 step means a garbage/
    // foreign constant row served the alpha this capture (the clone-shared
    // char-rows suspect; observed as part of the tee flickering invisible
    // for a moment). Repair: draw the item OPAQUE this frame (skip the fade
    // skip + the mid-fade blend routing) and log it. The last-alpha map
    // updates from the RAW value, so a persisting alpha 0 (real despawn /
    // spawn settle) only gets one repaired frame and then skips normally.
    // LW-mapped items bypass the blink repair entirely (read AND write):
    // the entity alpha cannot blink; the repair exists for garbage/foreign
    // CAPTURED rows, and its mesh key is clone-shared, which force-drew
    // spawn-settling clones OPAQUE every frame their twin was visible (the
    // "NPC drops out of the sky with no fade" sighting).
    bool fade_blink = false;
    if (item.char_family != 0 && debug_mode == 0 && item.lw_alpha < 0.0f) {
      static std::unordered_map<uint32_t, uint8_t> s_fade_opaque;  // render thread
      uint8_t& was_opaque = s_fade_opaque[item.mesh];
      if (entity_fade && fade_a <= 0.004f && was_opaque) {
        fade_blink = true;
        static std::atomic<uint64_t> s_blinks{0};
        const uint64_t n = s_blinks.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: fade BLINK repaired mesh={:08X} fam={} ropa={} "
              "src={} rows13=({:.3f},{:.3f},{:.3f},{:.3f}) "
              "rows14=({:.3f},{:.3f}) (n={})",
              item.mesh, item.char_family, item.ropa ? 1 : 0, item.dbg_src,
              item.char_rows[13 * 4 + 0], item.char_rows[13 * 4 + 1],
              item.char_rows[13 * 4 + 2], item.char_rows[13 * 4 + 3],
              item.char_rows[14 * 4 + 0], item.char_rows[14 * 4 + 1], n);
        }
      }
      was_opaque = (!entity_fade || fade_a > 0.9f) ? 1 : 0;
      if (s_fade_opaque.size() > 1024) {
        s_fade_opaque.clear();
      }
    }
    if (entity_fade && fade_a <= 0.004f && !fade_blink) {
      if (item.lw_alpha >= 0.0f) {
        g_lw_fade0.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    const bool char_fade_blend = char_fade_zwrite(item) && !fade_blink;
    // environment.reflective_trans (fam 13): blended glass canopies, joins
    // the sorted alpha sub-pass whenever its exact branch is live (same
    // shadow_valid gate as cam_pos.w = -fam; the legacy fallback renders it
    // opaque exactly as before classification).
    const bool refl_trans_blend =
        item.env_family == 13 && scene.shadow_valid;
    const auto stamp_route = [&](uint8_t route) {
      if (ring_frame != nullptr) {
        const auto rit = ring_map.find(&item);
        if (rit != ring_map.end()) {
          ring_frame->items[rit->second].route = route;
        }
      }
    };
    if ((item.transparent || item.water || hair_blend || glass_blend ||
         refl_trans_blend || char_fade_blend) &&
        debug_mode == 0) {
      if (REXCVAR_GET(skate3_native_render_scene_transparents)) {
        transparent_items.push_back(&item);
        stamp_route(2);
      }
      continue;
    }
    opaque_items.emplace_back(view_dist2(item), &item);
    stamp_route(1);
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
                       // Fading entities (z-write blend) draw before every
                       // z-write-off blend so hair/glass composite over them.
                       const bool fa = char_fade_zwrite(*a);
                       const bool fb = char_fade_zwrite(*b);
                       if (fa != fb) {
                         return fa;
                       }
                       return view_dist2(*a) > view_dist2(*b);
                     });
    list.D3DSetPipelineState(use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
    ID3D12PipelineState* blend_bound = use_depth ? g_r.pso_transparent : g_r.pso_nodepth;
    for (const DrawItem* item : transparent_items) {
      // Mid-fade entity pieces: alpha blend with z-write ON (see pso_fade).
      if (use_depth && g_r.pso_fade != nullptr && char_fade_zwrite(*item)) {
        if (blend_bound != g_r.pso_fade) {
          list.D3DSetPipelineState(g_r.pso_fade);
          blend_bound = g_r.pso_fade;
        }
        draw_item(*item);
        continue;
      }
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
        list.D3DSetPipelineState(blend_bound);
        continue;
      }
      // reflective_trans glass culls like the game (its material family's
      // XML: CULLMODE=FRONT): the canopy panels are double-glazed pane
      // PAIRS a few cm apart; uncull(ed) we composited BOTH panes (an
      // extra a^2 blend layer the emulated frame doesn't have).
      // pso_hair_b IS the transparent state with CULL_FRONT.
      if (item->env_family == 13 && use_depth && g_r.pso_hair_b != nullptr &&
          REXCVAR_GET(skate3_native_render_scene_backface_cull)) {
        const float* w = item->world;
        const float det3 = w[0] * (w[5] * w[10] - w[6] * w[9]) -
                           w[1] * (w[4] * w[10] - w[6] * w[8]) +
                           w[2] * (w[4] * w[9] - w[5] * w[8]);
        ID3D12PipelineState* want =
            det3 >= 0.0f ? g_r.pso_hair_b
                         : (use_depth ? g_r.pso_transparent : g_r.pso_nodepth);
        if (want != blend_bound) {
          list.D3DSetPipelineState(want);
          blend_bound = want;
        }
        draw_item(*item);
        continue;
      }
      if (blend_bound != (use_depth ? g_r.pso_transparent : g_r.pso_nodepth)) {
        blend_bound = use_depth ? g_r.pso_transparent : g_r.pso_nodepth;
        list.D3DSetPipelineState(blend_bound);
      }
      draw_item(*item);
    }
  }
  g_pw_items.Add(perf_ns_since(items_t0));

  // Guest-texture resolver shared by the spline pass (pre-resolve, in the
  // scene pass) and the HUD pass (post-resolve); both allocate strip/quad
  // vertices from the same per-frame ui_ring region.
  // Per-frame inline budget for HOT content re-decodes (actively-animating
  // elements + video planes): inline keeps their content latency at ONE
  // frame; overflow degrades to the async heal for the rest of the frame.
  int64_t hot_inline_budget_ns = 4'000'000;
  // force_inline: the dormant FMV bracket-fallback resolve stays on the
  // inline decode (fires at most once per plane set).
  // video: the FMV triple resolves; content changes always decode inline
  // (frame-exact playback), and the caller additionally gates serving on
  // last_change_frame >= the movie session start (see the triple loop).
  const auto resolve_2d_texture = [&](const uint32_t fetch[6],
                                      bool force_inline = false,
                                      bool video = false) -> const GuestTexture* {
    if ((fetch[0] & 0x3u) != 2 || fetch[1] == 0) {
      return &g_r.white;
    }
    const uint64_t key = FetchWordsKey(fetch);
    // Routing (see the 2d_async_px cvar): inline 2D decodes stall the frame
    // AND the guest, and the cost is NOT proportional to texel count (8888
    // APT tiles pay a per-PIXEL tiled-offset loop + 2 CreateCommittedResource
    // calls; 64x64 tiles measured 3-19 ms with the regen attempt on top)
    // - so FIRST SIGHTINGS and COLD content changes decode on
    // the words-miss workers. HOT content (changed within the last ~60
    // frames: video planes, actively-animating menu/HUD elements) re-decodes
    // INLINE under a small per-frame budget instead; the async round trip
    // (probe cadence + worker + commit ~ 4-5 frames) capped animating UI at
    // a ~30 Hz content rate, which read as "menus feel like 15-30 fps while
    // the counter says 140".
    const int32_t async_px = REXCVAR_GET(skate3_native_render_scene_2d_async_px);
    const uint32_t px_w = (fetch[2] & 0x1FFFu) + 1;
    const uint32_t px_h = ((fetch[2] >> 13) & 0x1FFFu) + 1;
    const bool async_ui = !force_inline && async_px > 0 &&
                          uint64_t(px_w) * px_h > uint64_t(async_px);
    // PLAIN store lookup on purpose: find_words_texture is the 3D poster
    // revalidator; it probes, RE-ARMS recheck_frame and enqueues its own
    // ui=false heal, which made this resolver's liveness block dead code
    // (recheck always freshly re-armed before it ran; astale=0 across whole
    // sessions) and put every 2D content update on that 4-5
    // frame async round trip.
    auto it = g_r.tex_store.find(key);
    bool inline_redecode = false;
    if (it != g_r.tex_store.end()) {
      it->second.last_used_frame = frame_number;
    }
    if (it != g_r.tex_store.end() && it->second.valid && !it->second.incomplete &&
        frame_number >= it->second.recheck_frame &&
        REXCVAR_GET(skate3_native_render_scene_tex_revalidate)) {
      // Content liveness for CPU-rewritten UI art: video frames and APT
      // re-rasterized tiles rewrite the same payload with the fetch words
      // unchanged; without the probe the words-keyed cache would freeze
      // them on their first decoded content.
      const uint64_t fp = SampleProbeFingerprint(base, it->second);
      if (fp != 0 && fp != it->second.payload_fp) {
        const bool hot =
            frame_number - it->second.last_change_frame <= 60;
        inline_redecode =
            video || !async_ui || (hot && hot_inline_budget_ns > 0);
        if (inline_redecode) {
          RetireGuestTexture(it->second,
                             command_processor->GetCurrentSubmission());
          g_r.tex_store.erase(it);
          it = g_r.tex_store.end();  // falls into the inline decode below
        } else {
          // Cold change (poster rotation class): heal on the workers,
          // serve the stale decode meanwhile.
          EnqueueWordsMiss(key, fetch, /*ui=*/true);
          it->second.recheck_frame = frame_number + 2;
          g_2d_async_stale.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        // Menu screens probe on a 2-frame cadence: the skater-portrait
        // boxes are resolved IN PLACE (words unchanged) up to hundreds of
        // ms after the quad first draws (asset streaming first), and a
        // 16-frame recheck left the pre-resolve memory content, the
        // loading-poster flash, on screen for its full window.
        it->second.recheck_frame =
            frame_number + (g_in_menus_frame.load(std::memory_order_relaxed) ? 2 : 16);
      }
    }
    if (it == g_r.tex_store.end()) {
      if (async_ui && !video && !inline_redecode) {
        // First sighting: decode on the workers and skip the quad for the
        // 1-3 frames that takes (imperceptible pop-in at the render rate,
        // vs a whole-frame render+guest stall inline).
        EnqueueWordsMiss(key, fetch, /*ui=*/true);
        g_2d_async_skip.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }
      // Small HUD/spline art decodes inline (sub-ms; async would pop
      // HUD elements for no gain).
      const auto hud_t0 = PerfClock::now();
      GuestTexture gt;
      EnsureGuestTextureFromWords(context, base, fetch, gt);
      const uint64_t decode_ns = perf_ns_since(hud_t0);
      hot_inline_budget_ns -= int64_t(decode_ns);
      g_pw_tex_decode.Add(decode_ns);
      // Attribution for residual render-thread stalls: anything still
      // decoding inline for >3 ms should either move over the async
      // threshold or explain itself here.
      if (decode_ns > 3'000'000) {
        static std::atomic<uint32_t> s_slow{0};
        const uint32_t n = s_slow.fetch_add(1, std::memory_order_relaxed);
        if (n < 24 || (n & 255u) == 0) {
          REXLOG_INFO(
              "native-scene: SLOW inline 2D decode {:.1f}ms {}x{} "
              "fetch=[{:08X} {:08X} {:08X}] forced={} (n={})",
              double(decode_ns) / 1e6, px_w, px_h, fetch[0], fetch[1],
              fetch[2], force_inline ? 1 : 0, n);
        }
      }
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
      gt.last_used_frame = frame_number;
      gt.last_change_frame = frame_number;
      it = g_r.tex_store.emplace(key, gt).first;
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
      const GuestTexture* spline_tex = resolve_2d_texture(s.fetch);
      if (spline_tex == nullptr) {
        continue;  // big-art decode in flight on the workers; skip a frame
      }
      const uint32_t srv_slot = spline_tex->srv_slot;
      list.D3DSetPipelineState(s.pass == 1 ? g_r.pso_spline_darken
                                           : g_r.pso_spline_default);
      // Root constants: the scene's (smoothed) view_proj rows; the verts
      // are world-space, then i_intensity as staged (c149).
      float spline_consts[20];
      std::memcpy(spline_consts, scene.view_proj, sizeof(float) * 16);
      std::memcpy(spline_consts + 16, s.consts + 149 * 4, sizeof(float) * 4);
      list.D3DSetGraphicsRoot32BitConstants(0, 20, spline_consts, 0);
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

  const bool outline_ready =
      RenderOutlineMask(context, scene, viewport, scissor, msaa_on, scene_rtv,
                        dsv, use_depth);

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
    RenderOutlineComposite(context, scene, output_rtv, viewport, scissor);
  }

  // ---- Photo-editor postfx chain (photo_fx.hlsl: exact ucode ports) ----
  // While the photo-mission photo editor is up and every pass's live
  // constants were captured this frame, apply the game's own chain over the
  // resolved native frame: depth pack -> visualfx (grade/vignette/CoC) ->
  // DOF downsample -> tap9dofMotionBlur -> tap9dof -> uber -> fisheye.
  if (scene.photo_fx.valid &&
      REXCVAR_GET(skate3_native_render_scene_photo_native) &&
      EnsurePhotoFxPipeline(context)) {
    ID3D12Device* device = context.d3d12.device;
    const auto pfx_to_srv = [&](ID3D12Resource* r) {
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data, r,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    const auto pfx_to_rt = [&](ID3D12Resource* r) {
      context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data, r,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET);
    };
    const auto pfx_flush = [&] {
      context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);
    };
    // Output-sized targets (visualfx out, uber out, packed depth).
    bool pfx_ok = true;
    if (g_r.pfx_width != context.guest_output_width ||
        g_r.pfx_height != context.guest_output_height || g_r.pfx_full[0] == nullptr) {
      ID3D12Resource** res[3] = {&g_r.pfx_full[0], &g_r.pfx_full[1], &g_r.pfx_depth};
      const uint32_t rtv_slots[3] = {8, 9, 13};
      const uint32_t srv_slots[3] = {g_r.pfx_srv[0], g_r.pfx_srv[1], g_r.pfx_srv[5]};
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = context.guest_output_width;
      desc.Height = context.guest_output_height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_CLEAR_VALUE clear{};
      clear.Format = desc.Format;
      for (int i = 0; i < 3 && pfx_ok; ++i) {
        if (*res[i] != nullptr) {
          g_r.retired.emplace_back(*res[i], command_processor->GetCurrentSubmission());
          *res[i] = nullptr;
        }
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   &clear, IID_PPV_ARGS(res[i])))) {
          pfx_ok = false;
          break;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(rtv_slots[i]) * g_r.rtv_size;
        device->CreateRenderTargetView(*res[i], nullptr, rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
        slot.ptr += size_t(srv_slots[i]) * g_r.srv_size;
        device->CreateShaderResourceView(*res[i], &srv, slot);
      }
      if (pfx_ok) {
        g_r.pfx_width = context.guest_output_width;
        g_r.pfx_height = context.guest_output_height;
      }
    }
    if (pfx_ok) {
      // Identity grade-LUT upload (once).
      if (!g_r.pfx_lut_uploaded) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_r.pfx_lut;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_r.pfx_lut_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = 32;
        src.PlacedFootprint.Footprint.Height = 32;
        src.PlacedFootprint.Footprint.Depth = 32;
        src.PlacedFootprint.Footprint.RowPitch = 256;
        list.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        context.d3d12.push_transition_barrier(
            context.d3d12.command_processor_user_data, g_r.pfx_lut,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_r.pfx_lut_uploaded = true;
      }
      // Native depth SRV (re-created every photo frame; the depth resource
      // is rebuilt on output resize).
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (g_r.msaa > 1) {
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
          sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          sd.Texture2D.MipLevels = 1;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
        slot.ptr += size_t(g_r.pfx_srv[7]) * g_r.srv_size;
        device->CreateShaderResourceView(g_r.depth, &sd, slot);
      }
      // Guest-output SRV (the finished native frame = the chain's scene
      // input), re-pointed like the blur block does.
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = context.d3d12.guest_output_format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE slot = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
        slot.ptr += size_t(g_r.output_srv_slot) * g_r.srv_size;
        device->CreateShaderResourceView(context.d3d12.guest_output_resource, &srv, slot);
      }
      // The two static input textures (fetch words captured at the game's
      // own uber/fisheye flushes): the 512x2 vignette gradient + the grain.
      const GuestTexture* vig =
          resolve_2d_texture(scene.photo_fx.vignette_fetch, /*force_inline=*/true);
      const GuestTexture* grain =
          resolve_2d_texture(scene.photo_fx.grain_fetch, /*force_inline=*/true);
      const uint32_t vig_slot =
          (vig != nullptr && vig->valid) ? vig->srv_slot : g_r.white.srv_slot;
      const uint32_t grain_slot =
          (grain != nullptr && grain->valid) ? grain->srv_slot : g_r.white.srv_slot;

      // Baked literal rows c250..c255 per pass (the shader asset footers the
      // game loads via PM4 LOAD_ALU_CONSTANT to PS rows 252+; values read
      // from capture).
      static constexpr float kPfxLiterals[kPfxPassCount][6][4] = {
          // visualfx
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {-0.200000003f, 1.0f, 1.41412354f, 2.0f},
           {1.51991853e-05f, 0.99609381f, 0.00389099144f, 0.00392156886f},
           {0.200000003f, 0.300000012f, 0.5f, 1.0f}},
          // dof downsample
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0.000868055562f, 0.00156250002f, 0.25f, 0.00100000005f}},
          // tap9dofMotionBlur
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {1.0f, 1.5f, 9.99999975e-05f, 0.00392156886f},
           {1.51991853e-05f, 0.99609381f, 0.00389099144f, 0.0f}},
          // tap9dof
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0.100000001f, 0, 0, 0}},
          // uber
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {2.0f, 0.5f, -1.0f, 0.0f},
           {0.015625f, 0.984375f, 0.96875f, -0.96875f},
           {1.0f, 0, 0, 0}},
          // fisheye
          {{0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {0, 0, 0, 0},
           {-1.0f, 0.5f, 0, 0},
           {-0.5f, -0.888888896f, 1.0f, 1.77777779f}},
      };
      const uint32_t cb_slot_base = uint32_t(frame_number % 4) * 8;
      uint32_t cb_slot_next = 0;
      const auto fill_cb = [&](int pass) -> D3D12_GPU_VIRTUAL_ADDRESS {
        const uint32_t slot = cb_slot_base + (cb_slot_next++);
        uint8_t* dst = g_r.pfx_cb_ptr + size_t(slot) * 4096;
        std::memset(dst, 0, 4096);
        float* rows = reinterpret_cast<float*>(dst);
        if (pass >= 0) {
          std::memcpy(rows, scene.photo_fx.ps[pass], sizeof(scene.photo_fx.ps[pass]));
          std::memcpy(rows + 240 * 4, scene.photo_fx.vs[pass],
                      sizeof(scene.photo_fx.vs[pass]));
          std::memcpy(rows + 250 * 4, kPfxLiterals[pass], sizeof(kPfxLiterals[pass]));
        }
        rows[248 * 4 + 0] = float(context.guest_output_width);
        rows[248 * 4 + 1] = float(context.guest_output_height);
        return g_r.pfx_cb->GetGPUVirtualAddress() + size_t(slot) * 4096;
      };
      const D3D12_GPU_DESCRIPTOR_HANDLE pfx_heap_start =
          g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
      const auto pfx_bind = [&](uint32_t param, uint32_t slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = pfx_heap_start;
        h.ptr += size_t(slot) * g_r.srv_size;
        context.d3d12.set_graphics_root_descriptor_table(
            context.d3d12.command_processor_user_data, param, h);
      };
      // Root parameter map: 1=t0 2=t1 3=t2 4=t3 5=t4 6=t6 7=t7 8=t5.
      const auto pfx_bind_all = [&](uint32_t t0, uint32_t t1, uint32_t t2,
                                    uint32_t t3, uint32_t t4, uint32_t t6,
                                    uint32_t t7, uint32_t t5) {
        pfx_bind(1, t0);
        pfx_bind(2, t1);
        pfx_bind(3, t2);
        pfx_bind(4, t3);
        pfx_bind(5, t4);
        pfx_bind(6, t6);
        pfx_bind(7, t7);
        pfx_bind(8, t5);
      };
      const uint32_t W = g_r.white.srv_slot;
      D3D12_VIEWPORT half_vp{0.0f, 0.0f, float(RendererState::kPfxHalfW),
                             float(RendererState::kPfxHalfH), 0.0f, 1.0f};
      D3D12_RECT half_sc{0, 0, LONG(RendererState::kPfxHalfW),
                         LONG(RendererState::kPfxHalfH)};
      D3D12_VIEWPORT quarter_vp{0.0f, 0.0f, float(RendererState::kPfxQuarterW),
                                float(RendererState::kPfxQuarterH), 0.0f, 1.0f};
      D3D12_RECT quarter_sc{0, 0, LONG(RendererState::kPfxQuarterW),
                            LONG(RendererState::kPfxQuarterH)};
      const auto set_rtv = [&](uint32_t rtv_slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(rtv_slot) * g_r.rtv_size;
        list.D3DOMSetRenderTargets(1, &rtv, FALSE, nullptr);
      };
      list.D3DSetGraphicsRootSignature(g_r.pfx_root_sig);
      list.D3DIASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      const int32_t accum_mode =
          REXCVAR_GET(skate3_native_render_scene_photo_native_accum);

      // 1) Depth pack: native depth (sample 0) -> the console D24-as-8888
      //    layout at output res.
      context.d3d12.push_transition_barrier(
          context.d3d12.command_processor_user_data, g_r.depth,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      pfx_flush();
      set_rtv(13);
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      list.D3DSetPipelineState(g_r.pfx_pso[0]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(-1));
      pfx_bind_all(W, W, W, W, g_r.pfx_srv[6], W, W, g_r.pfx_srv[7]);
      list.D3DDrawInstanced(3, 1, 0, 0);
      context.d3d12.push_transition_barrier(
          context.d3d12.command_processor_user_data, g_r.depth,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      pfx_to_srv(g_r.pfx_depth);
      // Scene input -> SRV for the rest of the chain.
      context.d3d12.push_transition_barrier(
          context.d3d12.command_processor_user_data, context.d3d12.guest_output_resource,
          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      pfx_flush();

      // 2) Accumulation input (visualfx t3). Mode 0 = black, 1 = the scene
      //    downsampled (this frame, the editor scene is frozen).
      if (accum_mode == 0) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(12) * g_r.rtv_size;
        const float black[4] = {0, 0, 0, 0};
        list.D3DClearRenderTargetView(rtv, black, 0, nullptr);
      } else if (accum_mode == 1) {
        set_rtv(12);
        list.RSSetViewport(quarter_vp);
        list.RSSetScissorRect(quarter_sc);
        list.D3DSetPipelineState(g_r.pfx_pso[7]);
        list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(-1));
        pfx_bind_all(g_r.output_srv_slot, W, W, W, g_r.pfx_srv[6], W, W, W);
        list.D3DDrawInstanced(3, 1, 0, 0);
      }
      pfx_to_srv(g_r.pfx_quarter);
      pfx_flush();

      // 3) visualfx (full res): scene + depth + accumulation -> pfx_full[0].
      set_rtv(8);
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      list.D3DSetPipelineState(g_r.pfx_pso[1]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(kPfxVisualFx));
      pfx_bind_all(g_r.output_srv_slot, g_r.pfx_srv[5], W, g_r.pfx_srv[4],
                   g_r.pfx_srv[6], W, W, W);
      list.D3DDrawInstanced(3, 1, 0, 0);
      pfx_to_srv(g_r.pfx_full[0]);
      pfx_flush();

      // 4) DOF downsample: pfx_full[0] -> pfx_half[0].
      set_rtv(10);
      list.RSSetViewport(half_vp);
      list.RSSetScissorRect(half_sc);
      list.D3DSetPipelineState(g_r.pfx_pso[2]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(kPfxDofDown));
      pfx_bind_all(g_r.pfx_srv[0], W, W, W, g_r.pfx_srv[6], W, W, W);
      list.D3DDrawInstanced(3, 1, 0, 0);
      pfx_to_srv(g_r.pfx_half[0]);
      pfx_flush();

      // 5) tap9dofMotionBlur: pfx_half[0] + depth -> pfx_half[1].
      set_rtv(11);
      list.D3DSetPipelineState(g_r.pfx_pso[3]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(kPfxDofMB));
      pfx_bind_all(g_r.pfx_srv[2], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6], W, W, W);
      list.D3DDrawInstanced(3, 1, 0, 0);
      pfx_to_srv(g_r.pfx_half[1]);
      pfx_to_rt(g_r.pfx_half[0]);
      pfx_flush();

      // 6) tap9dof: pfx_half[1] -> pfx_half[0].
      set_rtv(10);
      list.D3DSetPipelineState(g_r.pfx_pso[4]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(kPfxDof));
      pfx_bind_all(g_r.pfx_srv[3], W, W, W, g_r.pfx_srv[6], W, W, W);
      list.D3DDrawInstanced(3, 1, 0, 0);
      pfx_to_srv(g_r.pfx_half[0]);
      pfx_flush();

      // 7) uber (full res): graded sharp + depth + LUT + grain + blurred
      //    half -> pfx_full[1].
      set_rtv(9);
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      list.D3DSetPipelineState(g_r.pfx_pso[5]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(kPfxUber));
      pfx_bind_all(g_r.pfx_srv[0], g_r.pfx_srv[5], W, W, g_r.pfx_srv[6],
                   grain_slot, g_r.pfx_srv[2], W);
      list.D3DDrawInstanced(3, 1, 0, 0);
      pfx_to_srv(g_r.pfx_full[1]);
      // The finished chain replaces the output: back to RT for fisheye.
      context.d3d12.push_transition_barrier(
          context.d3d12.command_processor_user_data, context.d3d12.guest_output_resource,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      pfx_flush();

      // 8) fisheye (to screen): lens warp + vignette gradient + tint.
      list.D3DOMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
      list.D3DSetPipelineState(g_r.pfx_pso[6]);
      list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(kPfxFisheye));
      pfx_bind_all(g_r.pfx_srv[1], W, vig_slot, W, g_r.pfx_srv[6], W, W, W);
      list.D3DDrawInstanced(3, 1, 0, 0);

      // 9) Accumulation mode 2: next frame's visualfx t3 = the finished
      //    frame downsampled (pfx_full[1] is still in SRV state here).
      if (accum_mode == 2) {
        pfx_to_rt(g_r.pfx_quarter);
        pfx_flush();
        set_rtv(12);
        list.RSSetViewport(quarter_vp);
        list.RSSetScissorRect(quarter_sc);
        list.D3DSetPipelineState(g_r.pfx_pso[7]);
        list.D3DSetGraphicsRootConstantBufferView(0, fill_cb(-1));
        pfx_bind_all(g_r.pfx_srv[1], W, W, W, g_r.pfx_srv[6], W, W, W);
        list.D3DDrawInstanced(3, 1, 0, 0);
      }

      // Restore steady states (all pfx color targets idle as RENDER_TARGET)
      // + the main pass's root bindings for the 2D overlay.
      pfx_to_rt(g_r.pfx_full[0]);
      pfx_to_rt(g_r.pfx_full[1]);
      pfx_to_rt(g_r.pfx_half[0]);
      pfx_to_rt(g_r.pfx_half[1]);
      if (accum_mode != 2) {
        pfx_to_rt(g_r.pfx_quarter);
      }
      pfx_to_rt(g_r.pfx_depth);
      pfx_flush();
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      list.D3DSetGraphicsRootSignature(g_r.root_signature);
      if (g_r.shadow_cb != nullptr) {
        const uint32_t cb_offset =
            uint32_t(frame_number % RendererState::kShadowCbRegions) * 256u;
        list.D3DSetGraphicsRootConstantBufferView(
            6, g_r.shadow_cb->GetGPUVirtualAddress() + cb_offset);
        list.D3DSetGraphicsRootConstantBufferView(
            9, g_r.bone_ring->GetGPUVirtualAddress() + bone_region);
      }
      static bool s_pfx_first = true;
      if (s_pfx_first) {
        s_pfx_first = false;
        REXLOG_INFO(
            "native-scene: photo editor postfx chain LIVE (native; accum mode {}, "
            "vignette {}, grain {})",
            accum_mode, vig_slot == W ? "WHITE-fallback" : "resolved",
            grain_slot == W ? "WHITE-fallback" : "resolved");
      }
    }
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
    const float h_consts[8] = {1.0f,
                               0.0f,
                               scene.ui_blur,
                               0.0f,
                               scene.ui_blur_color[0],
                               scene.ui_blur_color[1],
                               scene.ui_blur_color[2],
                               1.0f};
    list.D3DSetGraphicsRoot32BitConstants(0, 8, h_consts, 0);
    srv_table(g_r.blur_srv[0]);
    list.D3DDrawInstanced(3, 1, 0, 0);
    // V: blur_tex[1] -> blur_tex[0].
    to_srv(g_r.blur_tex[1]);
    to_rt(g_r.blur_tex[0]);
    flush();
    list.D3DOMSetRenderTargets(1, &blur_rtv0, FALSE, nullptr);
    const float v_consts[8] = {0.0f,
                               1.0f,
                               scene.ui_blur,
                               0.0f,
                               scene.ui_blur_color[0],
                               scene.ui_blur_color[1],
                               scene.ui_blur_color[2],
                               1.0f};
    list.D3DSetGraphicsRoot32BitConstants(0, 8, v_consts, 0);
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
      // yuv != nullptr redirects this draw to the FMV combine: pso_yuv2d
      // with the three movie plane slots on tables 1/2/5 (t0/t1/t4), the
      // quad's own geometry/transform (so windowed movies place exactly).
      const auto emit_draw = [&](const Draw2d& d, const uint32_t* yuv = nullptr) {
        const uint32_t bytes = uint32_t(d.verts.size());
        if (bytes == 0 || d.stride != 28) {
          return;
        }
        if (ui_offset + bytes > RendererState::kUiRegionSize) {
          g_draws_2d_dropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::memcpy(g_r.ui_ring_cpu + ui_region + ui_offset, d.verts.data(), bytes);
        uint32_t srv_slot;
        if (yuv != nullptr) {
          srv_slot = yuv[0];
        } else {
          const GuestTexture* t = resolve_2d_texture(d.fetch);
          if (t == nullptr) {
            // Big-art decode in flight on the workers (large-art async
            // routing); skip the quad; it lands 1-3 frames later.
            return;
          }
          srv_slot = t->srv_slot;
        }
        float constants[40];
        std::memcpy(constants, d.consts, sizeof(d.consts));
        // 2D ortho draws have no translation row in the projection (c3 ==
        // (0,0,0,1)); perspective view-proj rows do. Half-pixel applies to
        // the former only.
        const bool ortho = d.consts[12] == 0.0f && d.consts[13] == 0.0f &&
                           d.consts[14] == 0.0f && d.consts[15] == 1.0f;
        constants[36] = ortho ? 1.0f : 0.0f;
        // m[9].y: sharp-magnification amount for APT cached-bitmap tiles
        // (see the cvar; the shader gates on actual fetch magnification).
        constants[37] = float(std::clamp(
            REXCVAR_GET(skate3_native_render_scene_2d_sharp), 0.0, 2.0));
        // m[9].zw: the D3D9 half-pixel shift in NDC, in OUTPUT-pixel units
        // (half a 720p pixel shifted fullscreen art 1-2 native px up-left
        // at 2x+ scales), and deliberately 7/16 px instead of exactly
        // 1/2: an exact half puts the bottom/right edge of an
        // edge-to-edge quad precisely THROUGH the last row/column's pixel
        // centers, and the top-left fill rule then drops that row/column
        // - the residual 1px see-through sliver on loading screens. The
        // 1/16 px underhang is far below visible sampling misalignment.
        constants[38] = viewport.Width > 0.0f ? 0.875f / viewport.Width : 0.0f;
        constants[39] = viewport.Height > 0.0f ? 0.875f / viewport.Height : 0.0f;
        list.D3DSetGraphicsRoot32BitConstants(0, 40, constants, 0);
        const auto srv_table_at = [&](uint32_t param, uint32_t slot) {
          D3D12_GPU_DESCRIPTOR_HANDLE h =
              g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
          h.ptr += size_t(slot) * g_r.srv_size;
          context.d3d12.set_graphics_root_descriptor_table(
              context.d3d12.command_processor_user_data, param, h);
        };
        srv_table_at(1, srv_slot);
        if (yuv != nullptr) {
          list.D3DSetPipelineState(g_r.pso_yuv2d);
          srv_table_at(2, yuv[1]);
          srv_table_at(5, yuv[2]);
        }
        D3D12_VERTEX_BUFFER_VIEW vbv{
            g_r.ui_ring->GetGPUVirtualAddress() + ui_region + ui_offset, bytes, d.stride};
        list.D3DIASetVertexBuffers(0, 1, &vbv);
        list.D3DIASetPrimitiveTopology(d.prim == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                   : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.D3DDrawInstanced(d.count, 1, 0, 0);
        if (yuv != nullptr) {
          list.D3DSetPipelineState(g_r.pso_2d);
        }
        ui_offset += bytes;
        ++drawn_2d;
      };

      list.D3DOMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
      list.RSSetViewport(viewport);
      list.RSSetScissorRect(scissor);
      // Native FMV substitution, self-contained: a video quad's console
      // shader binds Y at the DRAW's fetch slot 0 and the U/V planes at
      // slots 1/2: three valid distinct textures with the chroma at
      // exactly half the luma dimensions is a video draw, regardless of
      // which UI path drew it (the camera-page previews are plain APT
      // elements; matching against the VideoRenderer-published planes was
      // refuted twice; the UI paths sample APT-side plane COPIES, e.g.
      // 0x1F6xxxxx vs the published 0xA59xxxxx). Resolved
      // triples are cached per frame by the Y address, and their store
      // entries are forced to a per-frame liveness probe (a 30 fps video
      // rewriting its planes every few RENDERED frames would otherwise
      // settle the probe to 16-frame sampling, the "sluggish video").
      const auto yuv_triple = [](const Draw2d& d) -> bool {
        const uint32_t* s0 = d.fetch;
        const uint32_t* s1 = d.fetch + 6;
        const uint32_t* s2 = d.fetch + 12;
        if ((s0[0] & 3u) != 2 || (s1[0] & 3u) != 2 || (s2[0] & 3u) != 2 ||
            s0[1] == 0 || s1[1] == 0 || s2[1] == 0 || s1[1] == s0[1] ||
            s2[1] == s0[1] || s1[1] == s2[1]) {
          return false;
        }
        const uint32_t w0 = (s0[2] & 0x1FFFu) + 1, h0 = ((s0[2] >> 13) & 0x1FFFu) + 1;
        const uint32_t w1 = (s1[2] & 0x1FFFu) + 1, h1 = ((s1[2] >> 13) & 0x1FFFu) + 1;
        const uint32_t w2 = (s2[2] & 0x1FFFu) + 1, h2 = ((s2[2] >> 13) & 0x1FFFu) + 1;
        const auto half = [](uint32_t full, uint32_t c) {
          return c == full / 2 || c == (full + 1) / 2;
        };
        return w0 >= 32 && h0 >= 32 && half(w0, w1) && half(h0, h1) &&
               w2 == w1 && h2 == h1;
      };
      struct TripleCacheEntry {
        uint32_t y_addr;
        bool ok;
        uint32_t slots[3];
      };
      TripleCacheEntry triple_cache[kMaxMovies];
      int triple_count = 0;
      // Backup: the freshest VideoRenderer-published plane set, for a
      // bracketed movie quad without a readable triple (the boot intro
      // rendered through this before the triple detection existed).
      // Resolved lazily; the capped log tracks whether it is still ever
      // needed; if it stays silent across sessions, this path and the
      // OnMovieFrame publish machinery behind it can be retired.
      uint32_t fallback_slots[3] = {};
      int fallback_state = 0;  // 0 = unresolved, 1 = ok, -1 = unavailable
      const auto fallback_yuv = [&]() -> const uint32_t* {
        if (fallback_state == 0) {
          fallback_state = -1;
          const MoviePlanes* best = nullptr;
          for (const MoviePlanes& m : movies) {
            if (m.ns >= 0 && movie_now_ns - m.ns < 500'000'000 &&
                (best == nullptr || m.ns > best->ns)) {
              best = &m;
            }
          }
          if (best != nullptr) {
            bool ok = true;
            for (int p = 0; p < 3 && ok; ++p) {
              auto hot = g_r.tex_store.find(FetchWordsKey(best->words[p]));
              if (hot != g_r.tex_store.end()) {
                hot->second.recheck_frame = 0;
              }
              const GuestTexture* t =
                  resolve_2d_texture(best->words[p], /*force_inline=*/true);
              ok = t != &g_r.white && t->texture != nullptr;
              fallback_slots[p] = t->srv_slot;
            }
            if (ok) {
              fallback_state = 1;
            }
          }
        }
        return fallback_state == 1 ? fallback_slots : nullptr;
      };
      bool movie_drawn = false;
      for (const Draw2d& d : scene_2d) {
        const uint32_t* yuv = nullptr;
        // Video-quad detection runs regardless of movie_sub: a quad whose
        // own fetch slots form a YUV triple must NEVER draw through ps_main
        // - that renders the raw Y plane (greyscale luma, or the PREVIOUS
        // video's frame while the plane copies are still stale), the
        // video-boundary flash class. If the planes can't resolve yet
        // (async decode in flight, substitution off) the quad is SKIPPED;
        // black under a starting video is what the real thing looks like.
        bool video_quad = false;
        if (yuv_triple(d)) {
          video_quad = true;
          if (movie_sub) {
            TripleCacheEntry* e = nullptr;
            for (int t = 0; t < triple_count && e == nullptr; ++t) {
              if (triple_cache[t].y_addr == d.fetch[1]) {
                e = &triple_cache[t];
              }
            }
            if (e == nullptr && triple_count < kMaxMovies) {
              e = &triple_cache[triple_count++];
              e->y_addr = d.fetch[1];
              e->ok = true;
              bool decode_failed = false;
              for (int p = 0; p < 3 && e->ok; ++p) {
                const uint32_t* w = d.fetch + p * 6;
                // Plain find (find_words_texture would probe + enqueue its
                // own ui=false heal first): force the per-frame probe, the
                // resolve inline-decodes any content change (video=true).
                auto hot = g_r.tex_store.find(FetchWordsKey(w));
                if (hot != g_r.tex_store.end()) {
                  hot->second.recheck_frame = 0;  // content-hot: per-frame probe
                }
                const GuestTexture* t =
                    resolve_2d_texture(w, /*force_inline=*/false, /*video=*/true);
                // Session gate: only content decoded during THIS movie
                // session serves; at a video boundary both the store and
                // guest memory can still hold the PREVIOUS video's last
                // frame (the plane copies keep their addresses); the quad
                // holds black until the new video's first frame lands.
                e->ok = t != nullptr && t != &g_r.white && t->texture != nullptr &&
                        t->last_change_frame >= s_movie_session_frame;
                decode_failed |= t == &g_r.white;
                e->slots[p] = e->ok ? t->srv_slot : 0;
              }
              if (decode_failed) {
                static std::atomic<uint32_t> s_triple_failed{0};
                if (s_triple_failed.fetch_add(1, std::memory_order_relaxed) < 8) {
                  REXLOG_INFO(
                      "native-scene: FMV plane triple resolve FAILED "
                      "(y={:08X})",
                      d.fetch[1]);
                }
              }
            }
            if (e != nullptr && e->ok) {
              yuv = e->slots;
            }
          }
        }
        if (yuv == nullptr && movie_sub && (d.flags & 0x2u) != 0 &&
            d.src_stride == 24) {
          // Bracketed movie quad without a readable triple: through ps_main
          // it draws its c8 opaque-black cover, also a video quad.
          video_quad = true;
          yuv = fallback_yuv();
          if (yuv != nullptr) {
            static std::atomic<uint32_t> s_fb_logged{0};
            if (s_fb_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
              REXLOG_INFO(
                  "native-scene: FMV bracket fallback served a quad (no "
                  "readable YUV triple on it)");
            }
          }
        }
        if (video_quad && yuv == nullptr) {
          static std::atomic<uint32_t> s_vskip{0};
          const uint32_t n = s_vskip.fetch_add(1, std::memory_order_relaxed);
          if (n < 8 || (n & 511u) == 0) {
            REXLOG_INFO(
                "native-scene: video quad skipped (planes not ready, "
                "y={:08X}, movie_sub={}) (n={})",
                d.fetch[1], movie_sub ? 1 : 0, n);
          }
          continue;
        }
        emit_draw(d, yuv);
        movie_drawn |= yuv != nullptr;
      }
      if (movie_drawn) {
        g_movie_native_last_ns.store(movie_now_ns, std::memory_order_relaxed);
      }
      if (movie_drawn) {
        static std::atomic<bool> s_movie_logged{false};
        if (!s_movie_logged.exchange(true, std::memory_order_relaxed)) {
          REXLOG_INFO(
              "native-scene: FMV rendering NATIVELY (movie-quad YUV "
              "substitution)");
        }
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
  MaybeDumpSceneRing();
  LogFrameStats(scene, frames, drawn, drawn_2d, drawn_spline, shadow_ready,
                shadow_draws);
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
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12
