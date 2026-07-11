// Shared sample-vertex decode + skin + spread helpers (see the header).
// Extracted verbatim from the five open-coded copies in
// skate3_native_scene.cpp, behavior-preserving.

#include "native/skate3_native_guest_read.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "generated/skate3_init.h"

namespace skate3::native_scene {

float GuestHalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1F;
  const uint32_t man = h & 0x3FF;
  if (exp == 0) return std::bit_cast<float>(sign);  // denorms ~0
  if (exp == 31) return std::bit_cast<float>(sign | 0x7F800000u);
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (man << 13));
}

float BindDiag(const DrawItem& item) {
  float d2 = 0.0f;
  for (int a = 0; a < 3; ++a) {
    const float d = item.bbox_max[a] - item.bbox_min[a];
    d2 += d * d;
  }
  return std::sqrt(d2);
}

namespace {

bool PosFinite(const float p[3]) {
  return p[0] > -1e7f && p[0] < 1e7f && p[1] > -1e7f && p[1] < 1e7f &&
         p[2] > -1e7f && p[2] < 1e7f;
}

}  // namespace

bool ReadSkinVertGuest(uint8_t* base, const DrawItem& item, uint32_t vtx,
                       SkinSampleVert* out) {
  const uint32_t v = item.vb_addr + vtx * item.stride;
  const uint32_t pa = v + item.pos_offset;
  float* p = out->p;
  switch (item.pos_fmt) {
    case 57:
      for (int a = 0; a < 3; ++a) {
        const uint32_t u = REX_LOAD_U32(pa + a * 4);
        std::memcpy(&p[a], &u, 4);
      }
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
      return false;
  }
  out->pos_finite = PosFinite(p);
  if (item.bw_offset != 0 && item.bi_offset != 0) {
    // u8x4 attributes are big-endian per 32-bit word: component k is byte
    // (24 - 8k) of the host-order load.
    const uint32_t bw = REX_LOAD_U32(v + item.bw_offset);
    const uint32_t bi = REX_LOAD_U32(v + item.bi_offset);
    for (int k = 0; k < 4; ++k) {
      out->w[k] = uint8_t((bw >> (24 - 8 * k)) & 0xFF);
      out->bone[k] = uint8_t((bi >> (24 - 8 * k)) & 0xFF);
    }
  } else {
    std::memset(out->w, 0, sizeof(out->w));
    std::memset(out->bone, 0, sizeof(out->bone));
  }
  return true;
}

bool ReadSkinSamplesGuest(uint8_t* base, const DrawItem& item, uint32_t n,
                          SkinSampleVert* out) {
  if (item.stride == 0) {
    return false;
  }
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2 || n < 2) {
    return false;
  }
  for (uint32_t s = 0; s < n; ++s) {
    if (!ReadSkinVertGuest(base, item, s * (count - 1) / (n - 1), &out[s])) {
      return false;
    }
  }
  return true;
}

int ReadSkinSamplesRaw(const uint8_t* vb, size_t vb_size, const DrawItem& item,
                       uint32_t n, SkinSampleVert* out) {
  if (item.stride == 0) {
    return -1;
  }
  const uint32_t count = item.vb_bytes / item.stride;
  if (count < 2 || n < 2) {
    return -1;
  }
  for (uint32_t s = 0; s < n; ++s) {
    const uint32_t v = (s * (count - 1) / (n - 1)) * item.stride;
    if (uint64_t(v) + item.stride > vb_size) {
      return int(s);  // short read: caller treats as nothing-to-judge
    }
    const uint8_t* vp = vb + v;
    SkinSampleVert& sv = out[s];
    float* p = sv.p;
    // Guest payload copied raw = big-endian attributes.
    switch (item.pos_fmt) {
      case 57:
        for (int a = 0; a < 3; ++a) {
          const uint8_t* b = vp + item.pos_offset + a * 4;
          const uint32_t w = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 |
                             uint32_t(b[2]) << 8 | b[3];
          p[a] = std::bit_cast<float>(w);
        }
        break;
      case 32:
        for (int a = 0; a < 3; ++a) {
          const uint8_t* b = vp + item.pos_offset + a * 2;
          p[a] = GuestHalfToFloat(uint16_t(uint16_t(b[0]) << 8 | b[1]));
        }
        break;
      case 26: {
        constexpr float kScale = 2.0f / 32767.0f;
        for (int a = 0; a < 3; ++a) {
          const uint8_t* b = vp + item.pos_offset + a * 2;
          p[a] = int16_t(uint16_t(b[0]) << 8 | b[1]) * kScale + (a == 1 ? 0.8f : 0.0f);
        }
        break;
      }
      default:
        return -1;
    }
    sv.pos_finite = PosFinite(p);
    if (item.bw_offset != 0 && item.bi_offset != 0) {
      // Component k = guest byte k (matches the live path's
      // byte-(24-8k)-of-host-load convention).
      const uint8_t* bw = vp + item.bw_offset;
      const uint8_t* bi = vp + item.bi_offset;
      for (int k = 0; k < 4; ++k) {
        sv.w[k] = bw[k];
        sv.bone[k] = bi[k];
      }
    } else {
      std::memset(sv.w, 0, sizeof(sv.w));
      std::memset(sv.bone, 0, sizeof(sv.bone));
    }
  }
  return int(n);
}

uint32_t SkinPointHostRows(const SkinSampleVert& sv, const float* rows,
                           size_t rows_floats, float q[3]) {
  uint32_t total = 0;
  q[0] = q[1] = q[2] = 0.0f;
  for (int k = 0; k < 4; ++k) {
    const uint32_t w = sv.w[k];
    if (w == 0) continue;
    const uint32_t r0 = 3u * sv.bone[k];
    if ((r0 + 3) * 4 > rows_floats) continue;
    total += w;
    for (int a = 0; a < 3; ++a) {
      const float* row = rows + (r0 + uint32_t(a)) * 4;
      q[a] += float(w) * (row[0] * sv.p[0] + row[1] * sv.p[1] + row[2] * sv.p[2] +
                          row[3]);
    }
  }
  if (total != 0) {
    for (int a = 0; a < 3; ++a) q[a] /= float(total);
  }
  return total;
}

uint32_t SkinPointBankRows(uint8_t* base, uint32_t bank, uint32_t pb,
                           const SkinSampleVert& sv, float q[3], bool* rows_sane) {
  uint32_t total = 0;
  q[0] = q[1] = q[2] = 0.0f;
  for (int k = 0; k < 4; ++k) {
    const uint32_t w = sv.w[k];
    if (w == 0) continue;
    const uint32_t r0 = pb + 3 * uint32_t(sv.bone[k]);
    if (r0 + 3 > 256) continue;
    total += w;
    for (int a = 0; a < 3; ++a) {
      float row[4];
      for (int i = 0; i < 4; ++i) {
        row[i] = std::bit_cast<float>(REX_LOAD_U32(bank + ((r0 + a) * 4 + i) * 4));
      }
      // A weighted bone's rotation row must be rotation-shaped (norm near
      // the entity scale). The vehicle-flick captures carried WORLD
      // POSITIONS in the rotation slots (norm ~140); those can still
      // project on-screen and pass the geometric gate.
      const float rn = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
      if (rn < 0.04f || rn > 25.0f) {
        *rows_sane = false;
      }
      q[a] += float(w) * (row[0] * sv.p[0] + row[1] * sv.p[1] + row[2] * sv.p[2] +
                          row[3]);
    }
  }
  if (total != 0) {
    for (int a = 0; a < 3; ++a) q[a] /= float(total);
  }
  return total;
}

int SkinnedSpreadHostRows(const SkinSampleVert* sv, uint32_t n, const float* rows,
                          size_t rows_floats, int min_n, bool garbage_fails,
                          float* out_spread, int* out_n) {
  float qmin[3] = {1e9f, 1e9f, 1e9f};
  float qmax[3] = {-1e9f, -1e9f, -1e9f};
  int weighted = 0;
  for (uint32_t s = 0; s < n; ++s) {
    if (!sv[s].pos_finite) {
      if (garbage_fails) {
        return -1;  // NaN/garbage positions mid-sim-write
      }
      continue;
    }
    float q[3];
    if (SkinPointHostRows(sv[s], rows, rows_floats, q) == 0) {
      continue;
    }
    ++weighted;
    for (int a = 0; a < 3; ++a) {
      qmin[a] = std::min(qmin[a], q[a]);
      qmax[a] = std::max(qmax[a], q[a]);
    }
  }
  if (out_n) {
    *out_n = weighted;
  }
  if (weighted < min_n) {
    return 0;  // nothing to judge
  }
  const float dx = qmax[0] - qmin[0];
  const float dy = qmax[1] - qmin[1];
  const float dz = qmax[2] - qmin[2];
  *out_spread = std::sqrt(dx * dx + dy * dy + dz * dz);
  return 1;
}

}  // namespace skate3::native_scene
