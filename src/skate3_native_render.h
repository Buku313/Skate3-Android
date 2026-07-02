#pragma once

// Skate 3 native renderer (data-driven scene renderer).
//
// The guest-function hooks in skate3_native_render.cpp are link-time
// weak-symbol overrides and are always present; everything they do is gated
// behind the skate3_native_render cvar. Install() only announces state and
// validates configuration. The scene capture/build and rendering live in
// skate3_native_scene.cpp.

namespace skate3::native_render {

void Install();

}  // namespace skate3::native_render
