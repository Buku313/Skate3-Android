#include "skate3_native_debug_dialog.h"

#include <imgui.h>

#include <rex/cvar.h>

#include "skate3_native_scene.h"

// Hook-layer master (boot-time), shown read-only.
REXCVAR_DECLARE(bool, skate3_native_render);
// Hot-reload feature gates (skate3_native_scene.cpp).
REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_lightmaps);
REXCVAR_DECLARE(bool, skate3_native_render_scene_macro);
REXCVAR_DECLARE(bool, skate3_native_render_scene_decals);
REXCVAR_DECLARE(bool, skate3_native_render_scene_transparents);
REXCVAR_DECLARE(bool, skate3_native_render_scene_shadows);
REXCVAR_DECLARE(bool, skate3_native_render_scene_backface_cull);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_quadlists);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_retain_offscreen);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_mesh_revalidate);
REXCVAR_DECLARE(bool, skate3_native_render_scene_tex_mips);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_debug);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_refl_mode);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_lod);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_x);
REXCVAR_DECLARE(double, skate3_native_render_scene_refl_bias_y);
REXCVAR_DECLARE(bool, skate3_native_render_scene_refl_bias_auto);
// Smoothness / pacing.
REXCVAR_DECLARE(bool, skate3_native_render_scene_smooth_camera);
REXCVAR_DECLARE(double, skate3_native_render_scene_smooth_camera_filter_ms);
REXCVAR_DECLARE(bool, skate3_native_render_scene_sort_opaque);
REXCVAR_DECLARE(double, skate3_guest_fps_cap);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_synthetic_pan);
REXCVAR_DECLARE(double, skate3_native_render_scene_synthetic_pan_rate);
REXCVAR_DECLARE(double, skate3_native_render_scene_synthetic_pan_amp);
// SDK: emulated-draw suppression while the native output is active.
REXCVAR_DECLARE(bool, native_render_suppress_emulated_draws);

namespace skate3 {
namespace {

bool CvarCheckbox(const char* label, bool value, const char* help = nullptr) {
  bool v = value;
  ImGui::Checkbox(label, &v);
  if (help != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", help);
  }
  return v;
}

}  // namespace

void NativeDebugDialog::Show() {
  visible_ = true;
  SetDrawActive(true);
}

void NativeDebugDialog::Hide() {
  if (!visible_) {
    return;
  }
  visible_ = false;
  SetDrawActive(false);
}

void NativeDebugDialog::Toggle() {
  if (visible_) {
    Hide();
  } else {
    Show();
  }
}

void NativeDebugDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  if (!visible_) {
    return;
  }
  bool open = visible_;
  ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Native Render Debug (F12)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) {
      Hide();
    }
    return;
  }

  if (!REXCVAR_GET(skate3_native_render)) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                       "skate3_native_render hook layer is OFF (boot-time)");
  }

  ImGui::SeparatorText("Renderer");
  {
    const bool v = CvarCheckbox("Native scene renderer (F5)",
                                REXCVAR_GET(skate3_native_render_scene),
                                "Full native/emulated switch, same as F5");
    if (v != REXCVAR_GET(skate3_native_render_scene)) {
      skate3::native_scene::ToggleSceneEnabled();
    }
  }
  REXCVAR_SET(native_render_suppress_emulated_draws,
              CvarCheckbox("Suppress emulated draws",
                           REXCVAR_GET(native_render_suppress_emulated_draws),
                           "Skip emulated GPU work for framebuffer-sized passes while "
                           "native output is active (perf). Small-surface passes "
                           "(lightmap page composition) always run."));
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_debug);
    const char* kModes[] = {"0: normal", "1: clear only", "2: solid colors",
                            "3: first 20 items", "4: no depth"};
    if (ImGui::Combo("debug mode", &mode, kModes, 5)) {
      REXCVAR_SET(skate3_native_render_scene_debug, mode);
    }
  }

  ImGui::SeparatorText("World shading");
  REXCVAR_SET(skate3_native_render_scene_lightmaps,
              CvarCheckbox("Lightmaps", REXCVAR_GET(skate3_native_render_scene_lightmaps),
                           "Baked lighting atlas sample x2 on world materials"));
  REXCVAR_SET(skate3_native_render_scene_macro,
              CvarCheckbox("Macro overlay", REXCVAR_GET(skate3_native_render_scene_macro),
                           "Large-scale grime/crack multiply (ground/wall weathering)"));
  REXCVAR_SET(skate3_native_render_scene_decals,
              CvarCheckbox("Decal art composite",
                           REXCVAR_GET(skate3_native_render_scene_decals),
                           "Graffiti/paint art lerped over environment.decal sections"));
  REXCVAR_SET(skate3_native_render_scene_transparents,
              CvarCheckbox("Transparent sub-pass",
                           REXCVAR_GET(skate3_native_render_scene_transparents),
                           "environment.transparent items (mist sheets, glass, fences)"));
  REXCVAR_SET(skate3_native_render_scene_shadows,
              CvarCheckbox("Dynamic shadows",
                           REXCVAR_GET(skate3_native_render_scene_shadows),
                           "Native CSM: skater/NPC/prop shadows onto the world"));
  REXCVAR_SET(skate3_native_render_scene_backface_cull,
              CvarCheckbox("Backface cull (game parity)",
                           REXCVAR_GET(skate3_native_render_scene_backface_cull),
                           "World env materials cull FRONT like the game's material "
                           "XMLs; off = legacy cull-none (shows interior faces)"));

  ImGui::SeparatorText("Reflective glass isolation (env fam 5/6/13)");
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_refl_mode);
    const char* kReflModes[] = {"0: normal",
                                "1: cube reflection OFF",
                                "2: cube at absolute LOD (slider)",
                                "3: flat normal (no normal map)",
                                "4: visualize cube sample only",
                                "5: body only (no spec, no cube)",
                                "6: normal-map LOD bias (slider)",
                                "7: visualize lightmap sample",
                                "8: visualize lightmap UV (frac x16)",
                                "9: lightmap resolve status (blue = missing)"};
    if (ImGui::Combo("refl mode", &mode, kReflModes, 10)) {
      REXCVAR_SET(skate3_native_render_scene_refl_mode, mode);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Live isolation of the glass-facade reflection term. Flip F5 to "
          "compare against the emulated frame at each setting:\n"
          "1 removes the cube term; if the artifact survives, it is NOT "
          "the reflection.\n2 + slider finds which mip level (if any) "
          "matches the emulated look.\n3 tests the normal-map perturbation."
          "\n4 shows exactly what the reflection vector samples.\n5 leaves "
          "only diffuse x lightmap.");
    }
    float lod = float(REXCVAR_GET(skate3_native_render_scene_refl_lod));
    if (ImGui::SliderFloat("refl LOD / extra bias", &lod, -4.0f, 12.0f, "%.1f")) {
      REXCVAR_SET(skate3_native_render_scene_refl_lod, double(lod));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Mode 2: the absolute cube mip level (0 = sharpest; ~9 = the "
          "face average).\nOther modes: extra LOD bias added to the "
          "automatic 640p-parity bias.");
    }
    REXCVAR_SET(skate3_native_render_scene_refl_bias_auto,
                CvarCheckbox("auto normal tilt (derive from material)",
                             REXCVAR_GET(skate3_native_render_scene_refl_bias_auto),
                             "Compute the constant tilt from each material's own "
                             "detail texture (hardware-exact BC1 decode), the "
                             "principled source of the hand-tuned reference "
                             "values. Uncheck to drive the sliders below "
                             "instead."));
    float bx = float(REXCVAR_GET(skate3_native_render_scene_refl_bias_x));
    if (ImGui::SliderFloat("normal tilt X (horizontal)", &bx, -0.2f, 0.2f, "%.3f")) {
      REXCVAR_SET(skate3_native_render_scene_refl_bias_x, double(bx));
    }
    float by = float(REXCVAR_GET(skate3_native_render_scene_refl_bias_y));
    if (ImGui::SliderFloat("normal tilt Y (vertical)", &by, -0.2f, 0.2f, "%.3f")) {
      REXCVAR_SET(skate3_native_render_scene_refl_bias_y, double(by));
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::SetTooltip(
          "Constant normal tilt on the glass, applied in EVERY mode; "
          "rotates all reflections. Drag until the reflected content sits "
          "where the emulated frame puts it (F5 to compare). Defaults "
          "(0.028, 0.012) = the exact detail-map constant. 0.01 here is "
          "roughly 1 degree of reflection rotation.");
    }
  }

  ImGui::SeparatorText("Scene items");
  REXCVAR_SET(skate3_native_render_scene_world_items,
              CvarCheckbox("World items", REXCVAR_GET(skate3_native_render_scene_world_items),
                           "Static geometry from the world sort lists"));
  REXCVAR_SET(skate3_native_render_scene_dynamic_items,
              CvarCheckbox("Dynamic items",
                           REXCVAR_GET(skate3_native_render_scene_dynamic_items),
                           "Characters, movable props, cloth (RenderMesh/world-path "
                           "captures)"));
  REXCVAR_SET(skate3_native_render_scene_quadlists,
              CvarCheckbox("Quad-list particles",
                           REXCVAR_GET(skate3_native_render_scene_quadlists),
                           "Non-indexed quad-list captures (particle systems; off by "
                           "default: render as white squares without sprite textures)"));
  REXCVAR_SET(
      skate3_native_render_scene_retain_offscreen,
      CvarCheckbox("Retain off-screen statics",
                   REXCVAR_GET(skate3_native_render_scene_retain_offscreen),
                   "Keep recently seen statics drawn while the game view-culls "
                   "them: the smoothed render camera trails the guest pose, so "
                   "without this world geometry visibly tears down right at the "
                   "screen edges during pans/traversal"));
  ImGui::SeparatorText("Overlays");
  REXCVAR_SET(skate3_native_render_scene_2d,
              CvarCheckbox("2D / HUD replay", REXCVAR_GET(skate3_native_render_scene_2d),
                           "APT/Flash HUD + glyph text + SimpleDraw icons"));
  REXCVAR_SET(skate3_native_render_scene_splines,
              CvarCheckbox("Neon splines", REXCVAR_GET(skate3_native_render_scene_splines),
                           "Waypoint arrows / marker beams"));

  ImGui::SeparatorText("Smoothness / pacing");
  REXCVAR_SET(skate3_native_render_scene_smooth_camera,
              CvarCheckbox("Smooth camera + entity poses",
                           REXCVAR_GET(skate3_native_render_scene_smooth_camera),
                           "The guest updates its camera/entities on its own ~200 Hz "
                           "sim tick; raw poses judder at high render rates. Re-times "
                           "them on the host clock (1 kHz camera sampler + pose "
                           "interpolation, a few ms of camera latency)."));
  {
    float w = float(REXCVAR_GET(skate3_native_render_scene_smooth_camera_filter_ms));
    if (ImGui::SliderFloat("camera filter (ms, 0 = off)", &w, 0.0f, 100.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_smooth_camera_filter_ms, double(w));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Boxcar average over the camera pose signal. The game's camera "
          "advances in 60 Hz-quantized lumps at high fps (the measured cause "
          "of stick-pan stutter); 50 ms = three 60 Hz periods cancels it "
          "exactly for ~25 ms extra camera latency. 0 reverts to raw "
          "interpolation.");
    }
  }
  REXCVAR_SET(skate3_native_render_scene_sort_opaque,
              CvarCheckbox("Front-to-back opaque sort",
                           REXCVAR_GET(skate3_native_render_scene_sort_opaque),
                           "Early-z rejects occluded pixels before the material shading"));
  {
    int cap = int(REXCVAR_GET(skate3_guest_fps_cap));
    if (ImGui::InputInt("Guest fps cap (0 = off)", &cap, 10, 30)) {
      if (cap < 0) cap = 0;
      if (cap > 1000) cap = 1000;
      REXCVAR_SET(skate3_guest_fps_cap, double(cap));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Pace guest frames on an even beat. Set a few fps below the display "
          "refresh (with G-Sync/VRR this is what makes motion read as smooth; "
          "uncapped, the guest's irregular frame times drive the refresh "
          "directly).");
    }
  }
  {
    int mode = REXCVAR_GET(skate3_native_render_scene_synthetic_pan);
    const char* kPanModes[] = {"0: off", "1: time-based (build clock)",
                               "2: fixed step per frame", "3: through smoother"};
    if (ImGui::Combo("Synthetic pan (P)", &mode, kPanModes, 4)) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan, mode);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Judder isolation probe: replaces the camera with a host-generated "
          "constant-rate 360-degree pan. AFTER ENGAGING, SWEEP THE REAL CAMERA "
          "AROUND ONCE WITH THE STICK; the game only submits what its own "
          "frustum sees; the sweep fills a static-world union so the full "
          "circle is populated.\n1 smooth = camera path is fine; 1 judders = "
          "frame pacing / present / display, not the camera.\n2 = constant "
          "angle per FRAME (smooth only if displayed frames are evenly spaced "
          ", the complement of 1).\n3 = feeds synthetic ~200 Hz samples "
          "through the camera smoother and logs reconstruction error "
          "(synthetic-pan: lines).");
    }
    float rate = float(REXCVAR_GET(skate3_native_render_scene_synthetic_pan_rate));
    if (ImGui::SliderFloat("pan rate (deg/s)", &rate, 5.0f, 720.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan_rate, double(rate));
    }
    float amp = float(REXCVAR_GET(skate3_native_render_scene_synthetic_pan_amp));
    if (ImGui::SliderFloat("pan amplitude (0 = full spin)", &amp, 0.0f, 60.0f, "%.0f")) {
      REXCVAR_SET(skate3_native_render_scene_synthetic_pan_amp, double(amp));
    }
    if (ImGui::Button("Record camera signal (8 s)")) {
      skate3::native_scene::RecordCameraSignal(8.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Record bone signal (6 s)")) {
      skate3::native_scene::RecordBoneSignal(6.0);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Click, then skate at a steady speed for 6 seconds. Records every "
          "raw entity pose (bone palettes with timestamps) + the rendered "
          "interpolation output to logs\\bone_signal_<ts>.bin, so wheel-vs-"
          "deck lag/orbit can be measured and candidate fixes replayed "
          "offline against real data.");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Click, then IMMEDIATELY pan the camera with the right stick at a "
          "steady rate for 8 seconds (synthetic pan should be OFF). Records "
          "every distinct guest camera pose with 1 kHz-sampler timestamps + "
          "the smoothed output, to logs\\cam_signal_<ts>.csv; offline "
          "analysis shows whether the game's own camera signal is jerky at "
          "the source.");
    }
  }

  ImGui::SeparatorText("Caches (lightmap-era plumbing)");
  REXCVAR_SET(
      skate3_native_render_scene_tex_revalidate,
      CvarCheckbox("Texture payload revalidation",
                   REXCVAR_GET(skate3_native_render_scene_tex_revalidate),
                   "Re-fingerprint cached texture payloads every 16 frames, re-decode "
                   "on change (added to heal late-composed lightmap pages)"));
  REXCVAR_SET(skate3_native_render_scene_mesh_revalidate,
              CvarCheckbox("Mesh payload revalidation",
                           REXCVAR_GET(skate3_native_render_scene_mesh_revalidate),
                           "Re-decode cached meshes when the guest payload fingerprint "
                           "changes (streaming arena reuse / CPU-animated buffers)"));
  REXCVAR_SET(skate3_native_render_scene_tex_mips,
              CvarCheckbox("Texture mip chains",
                           REXCVAR_GET(skate3_native_render_scene_tex_mips),
                           "Upload guest mip chains (off = mip 0 only). Flush the "
                           "texture cache after toggling."));
  if (ImGui::Button("Flush texture cache")) {
    skate3::native_scene::FlushTextureCache();
  }
  ImGui::SameLine();
  if (ImGui::Button("Flush mesh cache")) {
    skate3::native_scene::FlushMeshCache();
  }
  ImGui::TextDisabled("MSAA + shadow tile size are restart-only (skate3.toml).");

  ImGui::End();
  if (!open) {
    Hide();
  }
}

}  // namespace skate3
