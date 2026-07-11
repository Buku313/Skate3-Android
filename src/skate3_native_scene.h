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
  // Streamed-artwork diffuse override (event posters): the Massive ad
  // system rebinds the diffuse fetch slot at draw time with the current
  // event-ad art: either over a 16x16 min-mip placeholder channel (the
  // ace-of-spades "Big Event" grid) or REPLACING a full-size default
  // poster (the letter-writing frames showing the MONDO "THE NEW VIDEO"
  // ad). When the item's own main-pass draw (validated by slot 3 == this
  // item's lightmap) bound a different texture at fetch slot 4, these six
  // draw-time fetch words resolve the diffuse instead (words-keyed texture
  // cache). All zero = no override.
  uint32_t diffuse_fetch[6] = {};
  // Same mechanism for environment.decal: that family samples its decal
  // overlay art at fetch slot 4 (diffuse rides slot 6), and ad frames
  // covered by a decal section get the event art bound there at draw time.
  uint32_t decal_fetch[6] = {};
  // "macrooverlay" material channel (environment shaders): a large-scale
  // grime/crack overlay multiplied over the diffuse at uv *
  // macroOverlayUVScale with macroOverlayOpacity: the ground/wall
  // weathering that reads as "extra grit".
  uint32_t macro_tex;     // guest renderengine::Texture address (0 = none)
  float macro_scale;      // macroOverlayUVScale channel constant
  float macro_opacity;    // macroOverlayOpacity channel constant
  uint16_t pos_offset;
  uint16_t uv_offset;
  uint16_t uv2_offset;
  uint16_t bw_offset;  // blend weights (u8x4), valid when skinned
  uint16_t bi_offset;  // blend indices (u8x4), valid when skinned
  uint16_t normal_offset;  // usage-3 vertex normal (0 fmt = none)
  // usage-6 tangent / usage-7 binormal (k_10_11_11): NPC character meshes
  // (default_cloth / default_hair / livingworld peds / skateboard) carry NO
  // usage-3 normal; the game's VS derives it as cross(tangent, binormal)
  // (verified numerically against the emulated VS outputs, dot 0.9999).
  // tb_fmt = 16 when both are present; DecodeMesh then synthesizes the
  // normal, replacing the faceted ddx/ddy face-normal fallback.
  uint16_t tangent_offset = 0;
  uint16_t binormal_offset = 0;
  uint8_t tb_fmt = 0;
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
  // Character-family exact shading (defaultcharacter.fx and friends).
  // Classified from AttribulatorMaterialName: 1 = character.default_*
  // (defaultcharacter PS layout), 2 = CAC player pieces (skin / face / cloth
  // / leather / shift / cloth_ropa: cacstamp layout), 3 =
  // character.livingworld_* pedestrians (stamp recolor), 4 = character.hair
  // (CAC), 5 = character.default_hair (NPC hair), 6 =
  // character.livingworld_vehicles (vehicle.fx: colorize_red/blue paint
  // recolor + phong spec + environment-cube reflection), 7 =
  // character.livingworld_vehicles_glass (reflection-only, alpha-blended
  // windows). 0 = not a character.
  // char_rows = the canonical per-draw lighting block captured from the
  // PIXEL constant bank at palette-capture time (CaptureCharLighting):
  // 15 float4 rows consumed by the scene PS character branch (cbuffer CH,
  // b2). rows[14].y = family, > 0 only when the capture validated; items
  // without it fall back to the legacy empirical character shading.
  uint8_t char_family = 0;
  float char_rows[60] = {};
  // Hair strand coverage: the hair mesh's "alpha" channel texture, sampled
  // at the SECOND texcoord (raw float2); hair renders alpha-blended in the
  // sorted sub-pass (the opaque path is the blocky-helmet look).
  uint32_t hair_alpha_tex = 0;
  // AttribulatorMaterialName starts "environment.decal": wall/ground
  // sections whose pixel shader composites a `decal` art texture (graffiti,
  // painted branding, grime) over the base diffuse INSIDE the shader,
  // lerp(base, decal.rgb, decal.a), and draws OPAQUE (disassembled from
  // decalenvironment_defaultPS; these meshes ARE the surface there, so
  // rendering them alpha-blended punches holes in walls). Without the
  // composite the paint is missing (the no-graffiti bug).
  bool decal;
  // environment.decal_tileable: the decal art TILES across the surface
  // (rock/cliff faces) and must sample with WRAP; single-placement decal
  // art samples with CLAMP (its transparent border keeps the area outside
  // the placement clear).
  bool decal_tileable;
  uint32_t decal_art;  // `decal` channel texture (0 = none)
  // AttribulatorMaterialName starts "environment.transparent": mist/cloud
  // sheets, glass, fences, vines. Alpha-BLENDED in a sub-pass after all
  // opaque items (back-to-front, depth test on, z-write off); the opaque
  // pass's alpha-test turned the soft mist gradients into solid hard-edged
  // white cloud blobs.
  bool transparent;
  // AttribulatorMaterialName starts "water.": canal/ocean surfaces
  // (flowingwater.fx family). Drawn in the transparent sub-pass with a
  // dedicated shading branch (ripple normal taps + fresnel reflection);
  // `water_normal` is the material's `normal` channel texture, bound in the
  // macro slot (water never carries a macro overlay).
  bool water;
  uint32_t water_normal;
  uint32_t water_env;  // `environment` channel cube map (reflection term)
  // character.cloth_ropa (Ropa cloth-simulated garments, e.g. player tees):
  // the VS variant branches on a flag row kept in front of the bone palette
  // - sim active means the dynamic VB already holds deformed root-local
  // positions and the draw is RIGID (one affine at c188/c191), not skinned.
  // Detected via the AttribulatorMaterialName channel; consumed by
  // CaptureSkinnedState.
  bool ropa;
  // Exact world-material shading family, classified from
  // AttribulatorMaterialName (models verified against the game's own pixel
  // shaders):
  // 0 = none (legacy empirical shading), 1 = environment.default
  // (baseenvironment), 2 = environmentsimple.default (defaultenvironment),
  // 3 = environment.decal, 4 = environment.decal_tileable(_simple),
  // 5 = environment.reflective, 6 = environment.reflective_simple,
  // 7 = environmentsimple.alphatest, 8 = environmentsimple.diffuse,
  // 9 = tree.default, 10 = animated.tree, 11 = proxyworld.default,
  // 12 = incandescent.default.
  uint8_t env_family = 0;
  // dynamicobject.fx family (movable props: dispensers, dumpsters, benches,
  // cans, ...). 0 = not a dynamic object, 1 = dynamicobject.default,
  // 2 = dynamicobject.alphatest. These are rigid model-space props with per-
  // frame lighting rows (sun / ambient / exposure) captured from their PS
  // bank, NOT the frame-global world-material rows; hence a separate family
  // and its own scene PS branch (model verified exact against the game's
  // own pixel shader).
  uint8_t dynobj = 0;
  // "specular" channel texture (spec mask / eccentricity / reflection mask)
  // for env families; the "noise" tint texture for animated.tree. Bound in
  // the decal slot (t4) on families that carry no decal art.
  uint32_t spec_tex = 0;
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
  // Park-editor / object-mover selection: the game re-draws the selected
  // object after the sky (twice, stencil-marking it) and a postfx stencil
  // edge-detect adds the blue outline. Items matched to those re-draws by
  // (ib_obj, vb_obj, world translation) render into the native outline mask
  // (see kOutlineShaderSource).
  bool selected = false;
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
  // Global distance-fog parameter rows, captured once per frame from the
  // first main-pass draw's VS constant bank (main-pass layout: c4 = camera,
  // the validation key; c5 = fog ramp scale/bias/exponent, c6 = linear-space
  // fog color rgb + transmittance scale in w; identical across every
  // main-pass draw of a frame). Consumed by environment.transparent items.
  float fog_ramp[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  float fog_color[4] = {};
  // Dynamic-shadow (CSM) receiver constants, captured once per frame from a
  // world-material draw's PIXEL constant bank: raw rows c0..c8
  // (c0/c3/c4 = light-space X/Y/depth rows, c1/c2 = cascade 1/2 scale+offset,
  // c5.x = depth bias, c8 = shadow color + floor; c6 = sun dir, c7 = camera,
  // captured for validation only).
  bool shadow_valid = false;
  // Extended to c0..c11: c10.x = scene exposure (2.5), c11.y = the material
  // multiplier (1.0), consumed by the exact world-shading tone chain.
  float shadow_rows[48] = {};
  // Frame-global rows of the OTHER world shader families, captured
  // opportunistically from their PS banks (debug-path classified):
  // [0..1] = tree PS c0.xy (lightmap scale 0.3435 / floor 0.02),
  // [2] = tree PS c4.y (tint multiplier), [3] = proxyworld PS c3.y (0.45).
  // Defaults from the day capture cover frames before the first hit.
  float family_rows[4] = {0.3435f, 0.02f, 1.0f, 0.45f};
  // dynamicobject.fx frame-global lighting rows, captured from a
  // dynamicobject/alphatestdynamicobject PS bank (debug-path classified):
  // [0..2] sun direction (PS c9), [3] scene exposure (c13.x),
  // [4..6] flat ambient rgb (c15.xyz), [7] bounce scale (c15.w),
  // [8] material multiplier (c14.y), [9] static world-shadow floor (c8.w).
  // Consumed by the exact dynamicobject scene PS branch.
  bool dynobj_valid = false;
  float dynobj_rows[10] = {};
  // Sky-dome viewpos: sky.* meshes are camera-relative (sky.fx VS adds
  // g_vViewPos), but the game pins the sky viewpos Y at a fixed level
  // elevation (165.0 in every capture) so the skyline doesn't bob with the
  // skater. Captured per frame from the sky draw's VS bank; the default
  // covers frames before the first capture.
  float sky_height = 165.0f;
  // Sky sun rows, captured together with sky_height from the sky draw's
  // PIXEL bank (sky_defaultPS layout): [0..2] = g_vLightDir (unit vector
  // toward the sun), [3] = sun angular scale (m_params[0].x),
  // [4] = sky pre-tone multiplier (m_params[0].y), [5] = scene exposure
  // (g_envattributes[2].x). Consumed by the exact sky scene-PS branch;
  // the emulated frame's big sun glow is computed IN the sky dome shader
  // from a 1D radial gradient (the sky material's `specular` channel),
  // not by a separate draw.
  bool sky_sun_valid = false;
  float sky_sun[6] = {};
  // Fullscreen UI background blur: 0 = off; otherwise the kernel scale of
  // the game's blur_hBlur/vBlur + basictex pass chain (PS c0.x, 8 in every
  // capture), detected per frame from the blur_hBlurPS draw. The native
  // replica runs after the MSAA resolve, before the 2D overlay draws.
  float ui_blur = 0.0f;
  // Selection-outline color (postfx_edgedetectstencilPS c0; the park-editor
  // blue in every capture), refreshed from the guest edge-detect draw when
  // it runs. Consumed by the outline composite when any item is selected.
  float outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
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

// Queue one mesh for the prewarm decode workers (guest thread; deduped per
// load).
void OnMeshRegistered(uint8_t* base, uint32_t mesh);

// Called (post-call) from the pegasus::tRModelData::Fixup hook: the
// rw-arena LOAD-time pointer resolve, fired per model while its arena
// streams in. The EARLY prewarm source: it spreads mesh/texture decoding
// across the load's whole disk-streaming phase instead of the
// final-seconds activation burst. (The `Unfix` atoms are the save/size
// path and never run at load.)
void OnModelFixup(uint8_t* base, uint32_t model);

// Called (post-call) from the WorldPresentation::AddRenderInstance hook:
// the world registry add that fires per placed instance during the load's
// final ACTIVATION phase. Walks tInstance -> tRModelData -> mesh table
// (self-validating offsets) and queues every optimesh-form mesh: the
// completeness backstop behind OnModelFixup.
void OnAddRenderInstance(uint8_t* base, uint32_t instance);

// Called from the D3D::SetPending_AluConstants hook (guest render thread,
// invoked from inside the Draw* functions). Records the location of the
// device's positional 256-register VS constant shadow bank.
void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device);

// Called (per guest frame) from the Sk8::Challenge::PhotoReplayController::
// Update hook while a photo-mission's photo editor is up. Heartbeat for the
// photo-editor yield: the editor's depth-of-field / saturation / brightness
// / contrast effects are the game's own postfx chain, which only the
// emulated path executes.
void OnPhotoReplayUpdate();

// 2D/APT phase bracket (the HUD and every other 2D element is a Flash SWF
// converted to EA APT, rendered through FE::AptRenderingIntegration). Guest
// draws issued while inside a bracket are 2D draws. bit: 0 =
// FrontEndManager::Render2D, 1 = AptMovieIntegration::Render, 2 =
// AptRenderingIntegration::DrawRenderingUnit, 3 =
// AptRenderingIntegration::UpdateRenderToTexture (in gameplay the whole HUD
// renders inside bit 3 into a screen-sized overlay texture at true screen
// coordinates; replaying those draws directly is the composite), 4 =
// Sk8::Render::cFont::DrawstringLocal (glyph text: trick names/scores;
// text can flush outside the APT brackets), 5 =
// Sk8::Render::SimpleDraw::DrawParameters::Draw (immediate-mode quads:
// chase arrows, in-world markers/beams).
void On2dPhase(uint32_t bit, bool enter);

// Current guest shader objects (D3DDevice_SetPixelShader/SetVertexShader),
// recorded per draw so offline analysis can group the 2D draw stream by
// shader variant.
void OnSetShader(bool pixel, uint32_t obj);

// D3D::SetPending_RenderStates(device, mask, bank, ptr): the render-state
// shadow bank (blend/depth state for the 2D pass lives here).
void OnRenderStateUpload(uint64_t mask, uint32_t bank, uint32_t ptr);

// D3DDevice_SetViewport / SetRenderState_ScissorTestEnable-adjacent scissor
// tracking, recorded per draw (render-to-texture APT passes show up as
// non-fullscreen viewports).
void OnSetViewport(uint8_t* base, uint32_t viewport_ptr);
void OnSetScissor(uint8_t* base, uint32_t rect_ptr);

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

// Runtime renderer switch (bound to F5): flips skate3_native_render_scene
// live. Capture hooks, scene build, guest-output replacement and
// emulated-draw suppression all re-check the cvar every frame, so the swap
// between the native renderer and the emulated GPU is seamless. Returns the
// new state; refuses (returns false) when the skate3_native_render hook
// layer was not enabled at boot.
bool ToggleSceneEnabled();

// Debug-dialog cache flushes (F12 native-render debug menu): retire every
// cached GPU texture / mesh decode so hot-toggled decode settings (mip
// chains, revalidation, ...) rebuild immediately.
void FlushTextureCache();
void FlushMeshCache();

// Synthetic camera pan probe (judder isolation, hotkey P): cycles
// skate3_native_render_scene_synthetic_pan 0 -> 1 -> 2 -> 3 -> 0 and returns
// the new mode. 1 = time-based constant-rate pan injected at scene build,
// 2 = fixed angle step per published frame, 3 = synthetic pose samples fed
// through the camera smoother (reconstruction error logged numerically).
int CycleSyntheticPan();

// Camera-signal recorder (judder diagnosis): for `seconds`, records every
// distinct guest camera pose (1 kHz sampler timestamps) plus the per-frame
// raw and smoothed headings, then writes logs/cam_signal_<ts>.csv. Pan the
// camera with the stick at a steady rate while it runs; offline analysis
// shows whether the game's own pose sequence is irregular at the source.
void RecordCameraSignal(double seconds);

// Bone-signal recorder (wheel-guard diagnosis): for `seconds`, records every
// raw entity pose (ring pushes: bone palettes / rigid worlds, timestamps)
// and each frame's interpolated output, then writes
// logs/bone_signal_<ts>.bin. Skate at a steady speed while it runs;
// offline analysis replays candidate guard strategies against the real
// board data.
void RecordBoneSignal(double seconds);

}  // namespace skate3::native_scene
