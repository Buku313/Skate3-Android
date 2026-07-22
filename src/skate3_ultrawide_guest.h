#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>

// Guest-side ultrawide support: the native renderer draws true widescreen
// frames (wide guest output + Hor+ projection), but the game's own CPU
// culling still tests against its 16:9 view frustum and would drop side
// content before it ever reaches the renderer. The frustum patch scope below
// is injected into the recompiled cull routine (see
// cmake/ApplySkate3CodegenPatches.cmake) and widens the cull planes in guest
// memory to match the wide view.

inline bool Skate3UltrawideHasExpandedTargetAspect() {
  if (!rex::cvar::Query<bool>("skate3_ultrawide") ||
      !rex::graphics::IsNativeGuestOutputActive()) {
    return false;
  }

  return rex::cvar::Query<double>("skate3_ultrawide_target_aspect") >
         (16.0 / 9.0) + 0.01;
}

inline bool Skate3UltrawideShouldWidenGameCullFrustum(u8* base, uint32_t cull_block) {
  // Every cull block reaching the vectorized cull job gets its left/right
  // planes widened: DoFrustumCulling reads them unconditionally, including
  // blocks with no extra cull volumes (the word at +5260); gating on that
  // count left 16:9 culling holes at the frame edges in the areas whose
  // blocks carry none. Over-inclusion is safe (a few extra submitted
  // meshes); the plane sanity checks below still veto implausible data.
  (void)base;
  return cull_block != 0 && Skate3UltrawideHasExpandedTargetAspect() &&
         rex::cvar::Query<bool>("skate3_ultrawide_widen_game_frustum");
}

struct Skate3UltrawideGameFrustumPatchScope {
  Skate3UltrawideGameFrustumPatchScope(PPCContext& ctx_, u8* base_, uint32_t cull_block)
      : ctx(ctx_), base(base_) {
    if (!Skate3UltrawideShouldWidenGameCullFrustum(base, cull_block)) {
      return;
    }

    const double target_aspect = std::clamp(
        rex::cvar::Query<double>("skate3_ultrawide_target_aspect"), 16.0 / 9.0, 8.0);
    side_scale = float((16.0 / 9.0) / target_aspect);
    if (!(side_scale > 0.0f && side_scale < 0.999f)) {
      return;
    }

    block = cull_block;
    PlaneBits current_left_bits = LoadPlaneBits(kLeftPlaneOffset);
    PlaneBits current_right_bits = LoadPlaneBits(kRightPlaneOffset);

    WidenedPlaneCache& cache = CacheForBlock();
    if (cache.valid && cache.side_scale == side_scale &&
        current_left_bits == cache.widened[0] && current_right_bits == cache.widened[1]) {
      return;
    }

    cache.original[0] = current_left_bits;
    cache.original[1] = current_right_bits;

    Plane left = LoadPlane(kLeftPlaneOffset);
    Plane right = LoadPlane(kRightPlaneOffset);
    Plane widened_left{};
    Plane widened_right{};
    for (size_t i = 0; i < left.size(); ++i) {
      const float center = (left[i] + right[i]) * 0.5f;
      const float side = (left[i] - right[i]) * 0.5f * side_scale;
      widened_left[i] = center + side;
      widened_right[i] = center - side;
    }

    if (!NormalizePlane(widened_left) || !NormalizePlane(widened_right)) {
      return;
    }

    StorePlane(kLeftPlaneOffset, widened_left);
    StorePlane(kRightPlaneOffset, widened_right);
    cache.widened[0] = LoadPlaneBits(kLeftPlaneOffset);
    cache.widened[1] = LoadPlaneBits(kRightPlaneOffset);
    cache.side_scale = side_scale;
    cache.valid = true;
    active = true;
  }

  ~Skate3UltrawideGameFrustumPatchScope() = default;

  Skate3UltrawideGameFrustumPatchScope(const Skate3UltrawideGameFrustumPatchScope&) = delete;
  Skate3UltrawideGameFrustumPatchScope& operator=(const Skate3UltrawideGameFrustumPatchScope&) =
      delete;

 private:
  using Plane = std::array<float, 4>;
  using PlaneBits = std::array<uint32_t, 4>;

  struct WidenedPlaneCache {
    uint32_t block = 0;
    std::array<PlaneBits, 2> original{};
    std::array<PlaneBits, 2> widened{};
    float side_scale = 1.0f;
    bool valid = false;
  };

  static constexpr uint32_t kLeftPlaneOffset = 5152;
  static constexpr uint32_t kRightPlaneOffset = 5168;

  float LoadFloat(uint32_t address) const {
    PPCRegister value{};
    value.u32 = REX_LOAD_U32(address);
    return value.f32;
  }

  void StoreFloat(uint32_t address, float number) const {
    PPCRegister value{};
    value.f32 = number;
    REX_STORE_U32(address, value.u32);
  }

  Plane LoadPlane(uint32_t offset) const {
    Plane plane{};
    for (uint32_t i = 0; i < plane.size(); ++i) {
      plane[i] = LoadFloat(block + offset + i * 4);
    }
    return plane;
  }

  void StorePlane(uint32_t offset, const Plane& plane) const {
    for (uint32_t i = 0; i < plane.size(); ++i) {
      StoreFloat(block + offset + i * 4, plane[i]);
    }
  }

  PlaneBits LoadPlaneBits(uint32_t offset) const {
    PlaneBits plane{};
    for (uint32_t i = 0; i < plane.size(); ++i) {
      plane[i] = REX_LOAD_U32(block + offset + i * 4);
    }
    return plane;
  }

  WidenedPlaneCache& CacheForBlock() {
    static thread_local std::array<WidenedPlaneCache, 16> cache_entries{};
    WidenedPlaneCache* empty = nullptr;
    for (WidenedPlaneCache& entry : cache_entries) {
      if (entry.valid && entry.block == block) {
        return entry;
      }
      if (!entry.valid && !empty) {
        empty = &entry;
      }
    }

    WidenedPlaneCache& entry = empty ? *empty : cache_entries[block % cache_entries.size()];
    entry = {};
    entry.block = block;
    return entry;
  }

  static bool NormalizePlane(Plane& plane) {
    const float length =
        std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (!(length > 0.00001f) || !std::isfinite(length)) {
      return false;
    }

    const float inv_length = 1.0f / length;
    for (float& value : plane) {
      value *= inv_length;
      if (!std::isfinite(value)) {
        return false;
      }
    }
    return true;
  }

  uint32_t block = 0;
  PPCContext& ctx;
  u8* base = nullptr;
  float side_scale = 1.0f;
  bool active = false;
};
