#pragma once

#include "skate3_native_scene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace skate3::penguin_mod {

// Deliberately outside the Xbox guest-address range. The native renderer
// recognizes this as a host-owned mesh and never tries to read it from guest
// memory.
inline constexpr uint32_t kMeshKey = 0xFFF0BEEFu;

struct Asset {
  // Native scene vertex layout: 14 floats / 56 bytes per vertex.
  std::vector<float> vertices;
  std::vector<uint16_t> indices;
  std::vector<uint8_t> rgba;
  uint32_t texture_width = 0;
  uint32_t texture_height = 0;
  // Guest mesh whose full palette became the canonical bone-number space.
  // ApplyToFrame uses this exact item as its live pose anchor.
  uint32_t anchor_mesh = 0;
  float bbox_min[3] = {};
  float bbox_max[3] = {};
  // Two sole samples transferred from the original skater. Their original
  // CAC weights let ApplyToFrame find the live midpoint between both feet;
  // the mascot scale is applied around that point so it stays on the board.
  struct Anchor {
    float p[3] = {};
    uint32_t weights = 0;
    uint32_t indices = 0;
  } sole[2];
  std::string source_dir;
  std::string error;
  bool loaded = false;
};

// Loads the OBJ and diffuse atlas on first use. The returned storage lives for
// the process lifetime and is safe for the game and render threads to share.
const Asset &GetAsset();

// Called by the ordinary guest-mesh decoder while the original CAC is still
// visible. Once enough pieces have arrived, their bind-pose vertices and real
// bone weights are used to auto-rig a stable copy of the penguin asset.
void ObserveSkaterMesh(const native_scene::DrawItem &item,
                       const float *vertices, uint32_t vertex_count);
void BeginSkaterPaletteFrame();
void ObserveSkaterPalette(const native_scene::DrawItem &item);

// Null during the brief calibration window; stable for the process lifetime
// after the player rig has been transferred.
const Asset *GetRiggedAsset();

// Replaces the CAC player pieces with one host-owned penguin draw item while
// preserving the live player bone palette, world state, and lighting.
void ApplyToFrame(native_scene::FrameScene &scene);

} // namespace skate3::penguin_mod
