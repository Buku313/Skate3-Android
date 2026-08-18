#include "skate3_penguin_mod.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/rexglue-sdk/thirdparty/tracy/profiler/src/stb_image.h"

#if defined(__ANDROID__) || defined(ANDROID)
constexpr const char* kDefaultPenguinDir =
    "/storage/emulated/0/skate3/mods/penguin";
#else
constexpr const char* kDefaultPenguinDir = "mods/penguin";
#endif

REXCVAR_DEFINE_BOOL(skate3_penguin_mod, false, "Skate 3",
                    "Replace the playable create-a-skater model with the "
                    "Seiyu Paradise Penguin Mod when its assets are installed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_penguin_mod_dir, kDefaultPenguinDir, "Skate 3",
                      "Directory containing base.obj and texture_diffuse.png "
                      "for the Seiyu Paradise Penguin Mod")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(skate3_penguin_scale, 0.62, "Skate 3",
                      "Seiyu size relative to the original skater; "
                      "scaled around the live midpoint between both feet")
    .range(0.35, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_penguin_board_offset, 0.0, "Skate 3",
                      "Vertical penguin foot offset in metres; positive lifts "
                      "both feet above the board")
    .range(-0.25, 0.25)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::penguin_mod {
namespace {

struct ObjIndex {
  int v = 0;
  int vt = 0;
  int vn = 0;

  bool operator==(const ObjIndex&) const = default;
};

struct ObjIndexHash {
  size_t operator()(const ObjIndex& i) const {
    uint64_t h = uint32_t(i.v);
    h = (h * 0x9E3779B185EBCA87ull) ^ uint32_t(i.vt);
    h = (h * 0x9E3779B185EBCA87ull) ^ uint32_t(i.vn);
    return size_t(h ^ (h >> 32));
  }
};

struct ObservedVertex {
  float p[3];
  uint32_t weights;
  uint32_t indices;
};

struct ObservedMesh {
  std::vector<ObservedVertex> vertices;
  std::vector<float> bones;
};

std::mutex g_rig_mutex;
std::unordered_map<uint32_t, ObservedMesh> g_skater_meshes;
std::chrono::steady_clock::time_point g_last_new_skater_mesh{};
std::unique_ptr<Asset> g_rigged_asset;

bool IsSkateboardDeck(const native_scene::DrawItem& item) {
  // The deck is submitted through the same character shaders as the CAC.
  // Its bind box is uniquely long, wide and almost planar at sole height;
  // material family alone therefore must never decide whether it is erased.
  const float sx = std::fabs(item.bbox_max[0] - item.bbox_min[0]);
  const float sy = std::fabs(item.bbox_max[1] - item.bbox_min[1]);
  const float sz = std::fabs(item.bbox_max[2] - item.bbox_min[2]);
  const float long_axis = std::max(sx, sz);
  const float short_axis = std::min(sx, sz);
  return long_axis >= 0.45f && short_axis >= 0.10f && sy <= 0.22f &&
         item.bbox_max[1] <= 0.45f;
}

bool IsOriginalFaceFragment(const native_scene::DrawItem& item) {
  // Eyes/teeth use character.default rather than the normal CAC material,
  // which is why the family-2 replacement left two eyeballs overhead. They
  // are compact skinned pieces authored in the upper half of the player's
  // bind pose. Restrict this to non-LivingWorld items so NPC anatomy is not
  // affected if the handheld pedestrian cull is disabled.
  if (item.lw_entity != 0 || !item.skinned || item.bones.empty() ||
      (item.char_family != 1 && item.char_family != 5)) {
    return false;
  }
  const float sx = std::fabs(item.bbox_max[0] - item.bbox_min[0]);
  const float sy = std::fabs(item.bbox_max[1] - item.bbox_min[1]);
  const float sz = std::fabs(item.bbox_max[2] - item.bbox_min[2]);
  return item.bbox_min[1] >= 0.65f && std::max({sx, sy, sz}) <= 0.65f;
}

bool ParseObjIndex(std::string_view token, ObjIndex& out) {
  const size_t a = token.find('/');
  const size_t b = a == std::string_view::npos
                       ? std::string_view::npos
                       : token.find('/', a + 1);
  try {
    out.v = std::stoi(std::string(token.substr(0, a)));
    if (a != std::string_view::npos && b != a + 1) {
      out.vt = std::stoi(std::string(token.substr(
          a + 1, b == std::string_view::npos ? b : b - a - 1)));
    }
    if (b != std::string_view::npos && b + 1 < token.size()) {
      out.vn = std::stoi(std::string(token.substr(b + 1)));
    }
  } catch (...) {
    return false;
  }
  return out.v != 0;
}

int ResolveIndex(int raw, size_t count) {
  if (raw > 0) return raw - 1;
  if (raw < 0) return int(count) + raw;
  return -1;
}

bool LoadObj(const std::filesystem::path& path, Asset& out) {
  std::ifstream file(path);
  if (!file) {
    out.error = "could not open " + path.string();
    return false;
  }

  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 2>> texcoords;
  std::vector<std::array<float, 3>> normals;
  std::unordered_map<ObjIndex, uint16_t, ObjIndexHash> remap;
  out.vertices.reserve(10000 * 14);
  out.indices.reserve(30000);
  std::fill(std::begin(out.bbox_min), std::end(out.bbox_min),
            std::numeric_limits<float>::max());
  std::fill(std::begin(out.bbox_max), std::end(out.bbox_max),
            std::numeric_limits<float>::lowest());

  auto emit_vertex = [&](ObjIndex key, uint16_t& result) -> bool {
    if (auto it = remap.find(key); it != remap.end()) {
      result = it->second;
      return true;
    }
    const int pi = ResolveIndex(key.v, positions.size());
    const int ti = ResolveIndex(key.vt, texcoords.size());
    const int ni = ResolveIndex(key.vn, normals.size());
    if (pi < 0 || pi >= int(positions.size())) return false;
    if (out.vertices.size() / 14 >= 65535) {
      out.error = "OBJ expands beyond the native renderer's 16-bit index limit";
      return false;
    }
    const auto& p = positions[size_t(pi)];
    const std::array<float, 2> uv =
        ti >= 0 && ti < int(texcoords.size())
            ? texcoords[size_t(ti)]
            : std::array<float, 2>{0.0f, 0.0f};
    const std::array<float, 3> n =
        ni >= 0 && ni < int(normals.size())
            ? normals[size_t(ni)]
            : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    for (int axis = 0; axis < 3; ++axis) {
      out.bbox_min[axis] = std::min(out.bbox_min[axis], p[axis]);
      out.bbox_max[axis] = std::max(out.bbox_max[axis], p[axis]);
    }

    // UV V is flipped because stb_image returns PNG rows top-to-bottom while
    // Blender OBJ UVs use a bottom-left origin. Every vertex is rigidly
    // attached to palette bone 0 for the first playable pass; it follows the
    // complete player transform and tricks without paying for a second rig.
    const float v = 1.0f - uv[1];
    const float weight = std::bit_cast<float>(uint32_t{0x000000FFu});
    const float bone = std::bit_cast<float>(uint32_t{0});
    const float packed[14] = {p[0], p[1], p[2], uv[0], v, uv[0], v,
                              weight, bone, n[0], n[1], n[2], uv[0], v};
    result = uint16_t(out.vertices.size() / 14);
    out.vertices.insert(out.vertices.end(), std::begin(packed), std::end(packed));
    remap.emplace(key, result);
    return true;
  };

  std::string line;
  size_t line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    std::istringstream stream(line);
    std::string tag;
    stream >> tag;
    if (tag == "v") {
      std::array<float, 3> value{};
      if (stream >> value[0] >> value[1] >> value[2]) positions.push_back(value);
    } else if (tag == "vt") {
      std::array<float, 2> value{};
      if (stream >> value[0] >> value[1]) texcoords.push_back(value);
    } else if (tag == "vn") {
      std::array<float, 3> value{};
      if (stream >> value[0] >> value[1] >> value[2]) normals.push_back(value);
    } else if (tag == "f") {
      std::vector<ObjIndex> face;
      std::string token;
      while (stream >> token) {
        ObjIndex index;
        if (!ParseObjIndex(token, index)) {
          out.error = "bad face index at OBJ line " + std::to_string(line_number);
          return false;
        }
        face.push_back(index);
      }
      for (size_t i = 1; i + 1 < face.size(); ++i) {
        for (ObjIndex key : {face[0], face[i], face[i + 1]}) {
          uint16_t index = 0;
          if (!emit_vertex(key, index)) {
            if (out.error.empty()) {
              out.error = "invalid vertex reference at OBJ line " +
                          std::to_string(line_number);
            }
            return false;
          }
          out.indices.push_back(index);
        }
      }
    }
  }
  if (out.vertices.empty() || out.indices.empty()) {
    out.error = "OBJ contained no drawable triangles";
    return false;
  }
  return true;
}

bool LoadTexture(const std::filesystem::path& path, Asset& out) {
  int width = 0, height = 0, components = 0;
  stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &components, 4);
  if (pixels == nullptr || width <= 0 || height <= 0) {
    out.error = "could not decode " + path.string();
    if (pixels != nullptr) stbi_image_free(pixels);
    return false;
  }
  out.texture_width = uint32_t(width);
  out.texture_height = uint32_t(height);
  out.rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
  stbi_image_free(pixels);
  return true;
}

Asset LoadAsset() {
  Asset result;
  std::string dir(REXCVAR_GET(skate3_penguin_mod_dir));
  if (const char* env = std::getenv("SKATE3_PENGUIN_MOD_DIR");
      env != nullptr && *env != '\0') {
    dir = env;
  }
  result.source_dir = dir;
  const std::filesystem::path root(dir);
  if (!LoadObj(root / "base.obj", result) ||
      !LoadTexture(root / "texture_diffuse.png", result)) {
    REXLOG_WARN("penguin-mod: disabled: {}", result.error);
    return result;
  }
  result.loaded = true;
  REXLOG_INFO(
      "penguin-mod: loaded {} vertices / {} triangles / {}x{} diffuse from {}",
      result.vertices.size() / 14, result.indices.size() / 3,
      result.texture_width, result.texture_height, result.source_dir);
  return result;
}

}  // namespace

const Asset& GetAsset() {
  static const Asset asset = LoadAsset();
  return asset;
}

void ObserveSkaterMesh(const native_scene::DrawItem& item, const float* vertices,
                       uint32_t vertex_count) {
  // Capture the original CAC bind data even while Original Skater is selected.
  // Decoded guest meshes are cached, so waiting until Seiyu is enabled means
  // there may be no later decode from which to build the live rig. This is a
  // bounded, one-time CPU copy and makes the menu switch genuinely hot.
  if (!item.skinned || (item.char_family != 2 && item.char_family != 4) ||
      IsSkateboardDeck(item) || vertices == nullptr || vertex_count == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_rig_mutex);
  if (g_rigged_asset != nullptr) return;
  ObservedMesh& slot = g_skater_meshes[item.mesh];
  if (!slot.vertices.empty()) return;

  slot.vertices.reserve(vertex_count);
  if (!item.bones.empty()) slot.bones = item.bones;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const float* v = vertices + size_t(i) * 14;
    ObservedVertex sample{{v[0], v[1], v[2]}, 0, 0};
    std::memcpy(&sample.weights, v + 7, sizeof(sample.weights));
    std::memcpy(&sample.indices, v + 8, sizeof(sample.indices));
    if (sample.weights != 0 && std::isfinite(sample.p[0]) &&
        std::isfinite(sample.p[1]) && std::isfinite(sample.p[2])) {
      slot.vertices.push_back(sample);
    }
  }
  if (!slot.vertices.empty()) {
    g_last_new_skater_mesh = std::chrono::steady_clock::now();
  }
}

void ObserveSkaterPalette(const native_scene::DrawItem& item) {
  if (!REXCVAR_GET(skate3_penguin_mod) || !item.skinned ||
      (item.char_family != 2 && item.char_family != 4) ||
      IsSkateboardDeck(item) || item.bones.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_rig_mutex);
  if (g_rigged_asset != nullptr) return;
  ObservedMesh& slot = g_skater_meshes[item.mesh];
  if (slot.bones.empty()) {
    slot.bones = item.bones;
    g_last_new_skater_mesh = std::chrono::steady_clock::now();
  }
}

const Asset* GetRiggedAsset() {
  const Asset& source = GetAsset();
  if (!source.loaded) return nullptr;

  std::lock_guard<std::mutex> lock(g_rig_mutex);
  if (g_rigged_asset != nullptr) return g_rigged_asset.get();

  // Let the worker decoders see the full outfit (head, torso, trousers,
  // shoes and hair) before freezing the transfer cloud. This usually lasts
  // 10-15 gameplay frames and is hidden by the normal takeover prewarm.
  size_t total_vertices = 0;
  size_t complete_meshes = 0;
  for (const auto& [mesh, observed] : g_skater_meshes) {
    if (!observed.vertices.empty() && !observed.bones.empty()) {
      total_vertices += observed.vertices.size();
      ++complete_meshes;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (complete_meshes < 3 || total_vertices < 1200 ||
      g_last_new_skater_mesh.time_since_epoch().count() == 0 ||
      now - g_last_new_skater_mesh < std::chrono::milliseconds(180)) {
    return nullptr;
  }

  // Every character piece has a compact local palette. Choose the largest
  // captured palette as canonical, then match each local bone matrix to its
  // nearest canonical matrix before combining vertices from shoes, trousers,
  // torso, face and hair. Treating the raw local byte as a global index was
  // the mangled-v1 bug (one shoe's local bone 0 became the torso root).
  const auto canonical_it = std::max_element(
      g_skater_meshes.begin(), g_skater_meshes.end(),
      [](const auto& a, const auto& b) {
        return a.second.bones.size() < b.second.bones.size();
      });
  if (canonical_it == g_skater_meshes.end() ||
      canonical_it->second.bones.size() < 12) {
    return nullptr;
  }
  const std::vector<float>& canonical_bones = canonical_it->second.bones;
  const size_t canonical_count = canonical_bones.size() / 12;

  std::vector<ObservedVertex> cloud;
  cloud.reserve(total_vertices);
  float worst_bone_match = 0.0f;
  for (const auto& [mesh, observed] : g_skater_meshes) {
    if (observed.vertices.empty() || observed.bones.empty()) continue;
    const size_t local_count = observed.bones.size() / 12;
    std::vector<uint8_t> remap(local_count, 0);
    for (size_t local = 0; local < local_count; ++local) {
      float best_error = std::numeric_limits<float>::max();
      size_t best_bone = 0;
      const float* local_rows = observed.bones.data() + local * 12;
      for (size_t candidate = 0; candidate < canonical_count; ++candidate) {
        const float* canonical_rows = canonical_bones.data() + candidate * 12;
        float error = 0.0f;
        for (int r = 0; r < 12; ++r) {
          const float delta = local_rows[r] - canonical_rows[r];
          error += delta * delta;
        }
        if (error < best_error) {
          best_error = error;
          best_bone = candidate;
        }
      }
      remap[local] = uint8_t(std::min<size_t>(best_bone, 255));
      worst_bone_match = std::max(worst_bone_match, best_error);
    }
    for (ObservedVertex vertex : observed.vertices) {
      uint32_t canonical_indices = 0;
      for (int k = 0; k < 4; ++k) {
        const uint8_t local = uint8_t(vertex.indices >> (8 * k));
        const uint8_t canonical = local < remap.size() ? remap[local] : 0;
        canonical_indices |= uint32_t(canonical) << (8 * k);
      }
      vertex.indices = canonical_indices;
      cloud.push_back(vertex);
    }
  }
  float src_min[3] = {std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
  float src_max[3] = {std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest()};
  for (const ObservedVertex& v : cloud) {
    for (int axis = 0; axis < 3; ++axis) {
      src_min[axis] = std::min(src_min[axis], v.p[axis]);
      src_max[axis] = std::max(src_max[axis], v.p[axis]);
    }
  }
  const float src_span[3] = {src_max[0] - src_min[0],
                             src_max[1] - src_min[1],
                             src_max[2] - src_min[2]};
  if (src_span[1] < 0.8f || src_span[0] < 0.25f || src_span[2] < 0.1f) {
    // An outfit piece arrived but the full body has not yet been decoded.
    return nullptr;
  }

  // Bound the one-shot nearest-neighbour work on handheld CPUs while keeping
  // dense coverage of every garment. Deterministic stride sampling retains
  // the complete head-to-shoe extent better than truncating the cloud.
  constexpr size_t kCloudLimit = 2500;
  if (cloud.size() > kCloudLimit) {
    std::vector<ObservedVertex> reduced;
    reduced.reserve(kCloudLimit);
    for (size_t i = 0; i < kCloudLimit; ++i) {
      reduced.push_back(cloud[i * (cloud.size() - 1) / (kCloudLimit - 1)]);
    }
    cloud = std::move(reduced);
  }

  auto rigged = std::make_unique<Asset>(source);
  rigged->anchor_mesh = canonical_it->first;
  const float penguin_span[3] = {
      source.bbox_max[0] - source.bbox_min[0],
      source.bbox_max[1] - source.bbox_min[1],
      source.bbox_max[2] - source.bbox_min[2]};
  const float lateral_scale =
      src_span[0] / std::max(penguin_span[0], 0.001f);
  const float map_scale[3] = {
      lateral_scale,
      src_span[1] / std::max(penguin_span[1], 0.001f),
      // Preserve the mascot's authored round cross-section. The human body
      // cloud is naturally much thinner front-to-back than it is wide; using
      // that depth directly made the penguin look paper-flat once the long
      // skateboard deck was correctly excluded from calibration.
      lateral_scale};
  const float src_center_z = 0.5f * (src_min[2] + src_max[2]);
  const float fit_min[3] = {
      src_min[0], src_min[1],
      src_center_z - 0.5f * penguin_span[2] * map_scale[2]};
  float fit_max[3] = {
      src_max[0], src_max[1],
      src_center_z + 0.5f * penguin_span[2] * map_scale[2]};

  // Resolve a compact mascot skeleton from the original outfit. The first
  // nearest-surface version copied human weights onto every penguin vertex;
  // because this model is one connected rounded shell, the belly inherited
  // thighs/arms and folded into a badly mangled human pose. Keep the torso
  // rigid instead, and animate only explicit anatomical regions.
  std::array<float, 256> body_influence{};
  std::array<float, 256> foot_influence[2]{};
  std::array<float, 256> arm_influence[2]{};
  std::array<std::vector<const ObservedVertex*>, 2> arm_samples;
  const float src_center_x = 0.5f * (src_min[0] + src_max[0]);
  const float src_half_x = 0.5f * src_span[0];
  const auto add_influence = [](std::array<float, 256>& dst,
                                const ObservedVertex& v, float scale) {
    for (int k = 0; k < 4; ++k) {
      const uint8_t weight = uint8_t(v.weights >> (8 * k));
      const uint8_t bone = uint8_t(v.indices >> (8 * k));
      dst[bone] += float(weight) * scale;
    }
  };
  for (const ObservedVertex& v : cloud) {
    const float yn = (v.p[1] - src_min[1]) / src_span[1];
    const float xn = (v.p[0] - src_center_x) / src_half_x;
    const int side = xn < 0.0f ? 0 : 1;
    if (yn < 0.16f) add_influence(foot_influence[side], v, 1.0f);
    // Human arms/hands occupy the upper outer half of the bind pose. The
    // old 0.22 lower bound admitted the wide skating stance, so the mascot
    // flippers accidentally selected leg bones (they moved for tricks but
    // ignored D-pad hand gestures). Bias the outermost samples strongly so
    // the selected end-effector is the hand/wrist rather than the sleeve.
    const float lateral = std::fabs(xn);
    if (lateral > 0.62f && yn > 0.48f && yn < 0.90f) {
      const float distal =
          std::clamp((lateral - 0.62f) / 0.38f, 0.0f, 1.0f);
      add_influence(arm_influence[side], v,
                    1.0f + 8.0f * distal * distal);
    }
    if (lateral > 0.40f && yn > 0.45f && yn < 0.90f) {
      arm_samples[side].push_back(&v);
    }
    if (std::fabs(xn) < 0.32f && yn > 0.38f && yn < 0.72f) {
      add_influence(body_influence, v, 1.0f);
    }
  }
  const auto strongest_bone = [](const std::array<float, 256>& influence) {
    return uint8_t(std::distance(
        influence.begin(), std::max_element(influence.begin(), influence.end())));
  };
  const uint8_t body_bone = strongest_bone(body_influence);
  const uint8_t foot_bone[2] = {strongest_bone(foot_influence[0]),
                                strongest_bone(foot_influence[1])};
  const uint8_t arm_bone[2] = {strongest_bone(arm_influence[0]),
                               strongest_bone(arm_influence[1])};

  // Fit the penguin to the skater's bind box for correct joint pivots. The
  // final 0.62 mascot scale is applied to the live bone output later.
  const size_t output_vertices = rigged->vertices.size() / 14;
  for (size_t i = 0; i < output_vertices; ++i) {
    float* v = rigged->vertices.data() + i * 14;
    const float model[3] = {v[0], v[1], v[2]};
    float mapped[3];
    for (int axis = 0; axis < 3; ++axis) {
      mapped[axis] = fit_min[axis] +
                     (v[axis] - source.bbox_min[axis]) * map_scale[axis];
      v[axis] = mapped[axis];
    }
    // Correct normals for the non-uniform bind-space fit.
    float nx = v[9] / map_scale[0];
    float ny = v[10] / map_scale[1];
    float nz = v[11] / map_scale[2];
    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nl > 0.0001f) {
      v[9] = nx / nl;
      v[10] = ny / nl;
      v[11] = nz / nl;
    }

    const float px = (model[0] - source.bbox_min[0]) / penguin_span[0];
    const float py = (model[1] - source.bbox_min[1]) / penguin_span[1];
    const float pz = (model[2] - source.bbox_min[2]) / penguin_span[2];
    const int side = px < 0.5f ? 0 : 1;

    // Feet are identified by their actual yellow atlas texels plus the low,
    // forward sole pixels. This deliberately excludes the yellow beak.
    const uint32_t tx = std::min(
        uint32_t(std::clamp(v[3], 0.0f, 1.0f) * source.texture_width),
        source.texture_width - 1);
    const uint32_t ty = std::min(
        uint32_t(std::clamp(v[4], 0.0f, 1.0f) * source.texture_height),
        source.texture_height - 1);
    const uint8_t* texel =
        source.rgba.data() + (size_t(ty) * source.texture_width + tx) * 4;
    const bool yellow = texel[0] > 145 && texel[1] > 95 && texel[2] < 115;
    const bool white = texel[0] > 150 && texel[1] > 150 && texel[2] > 150 &&
                       std::max({texel[0], texel[1], texel[2]}) -
                               std::min({texel[0], texel[1], texel[2]}) <
                           75;
    const bool dark_sole = texel[0] < 85 && texel[1] < 85 && texel[2] < 85;
    const bool foot = py < 0.18f && pz > 0.24f &&
                      (yellow || (dark_sole && std::fabs(px - 0.5f) > 0.07f));

    // Subtle facial relief. These tight color + bind-space masks select the
    // two eye ovals and the beak, excluding the white belly/flippers and the
    // yellow feet. Displace in the model's forward (+Z) direction after the
    // round-body fit, leaving the torso proportions untouched.
    const bool eye = white && py > 0.66f && pz > 0.62f &&
                     std::fabs(px - 0.5f) < 0.20f;
    const bool beak = yellow && py > 0.60f && py < 0.76f && pz > 0.70f &&
                      std::fabs(px - 0.5f) < 0.20f;
    if (beak) {
      v[2] += 0.045f;
    } else if (eye) {
      v[2] += 0.025f;
    }

    uint32_t packed_weights = 255u;
    uint32_t packed_bones = body_bone;
    if (foot) {
      packed_bones = foot_bone[side];
    } else if (py > 0.20f && py < 0.68f) {
      // Only the side protrusions become flippers. A narrow smooth shoulder
      // blend prevents cracking where each flipper meets the rigid torso.
      const float lateral = std::fabs(px - 0.5f) * 2.0f;
      float arm_weight = std::clamp((lateral - 0.56f) / 0.24f, 0.0f, 1.0f);
      arm_weight = arm_weight * arm_weight * (3.0f - 2.0f * arm_weight);
      const uint8_t aw = uint8_t(std::lround(arm_weight * 255.0f));
      if (aw != 0) {
        // Transfer the nearest authored human-arm skin weights locally.
        // A single strongest arm bone made the flipper rigid and missed the
        // wrist/hand motion used by D-pad emotes. Mapping shoulder->forearm->
        // hand across the protrusion preserves the complete gesture chain
        // without ever putting limb weights onto the connected belly.
        const float sign = side == 0 ? -1.0f : 1.0f;
        const float target_xn = sign * (0.58f + 0.42f * arm_weight);
        const float target_yn =
            0.56f + 0.16f * std::clamp((py - 0.20f) / 0.48f, 0.0f, 1.0f);
        const float target_zn = pz;
        const ObservedVertex* nearest = nullptr;
        float nearest_d2 = std::numeric_limits<float>::max();
        for (const ObservedVertex* sample : arm_samples[side]) {
          const float sxn = (sample->p[0] - src_center_x) / src_half_x;
          const float syn = (sample->p[1] - src_min[1]) / src_span[1];
          const float szn = (sample->p[2] - src_min[2]) / src_span[2];
          const float dx = sxn - target_xn;
          const float dy = syn - target_yn;
          const float dz = szn - target_zn;
          const float d2 = dx * dx + 1.5f * dy * dy + 0.35f * dz * dz;
          if (d2 < nearest_d2) {
            nearest_d2 = d2;
            nearest = sample;
          }
        }
        if (nearest != nullptr) {
          std::array<float, 256> blended{};
          blended[body_bone] = float(255 - aw);
          for (int k = 0; k < 4; ++k) {
            const uint8_t human_weight =
                uint8_t(nearest->weights >> (8 * k));
            const uint8_t human_bone = uint8_t(nearest->indices >> (8 * k));
            blended[human_bone] +=
                float(aw) * float(human_weight) / 255.0f;
          }
          std::array<uint8_t, 4> out_bones{};
          std::array<float, 4> out_values{};
          for (int k = 0; k < 4; ++k) {
            const auto best = std::max_element(blended.begin(), blended.end());
            out_bones[k] = uint8_t(std::distance(blended.begin(), best));
            out_values[k] = *best;
            *best = 0.0f;
          }
          const float total =
              out_values[0] + out_values[1] + out_values[2] + out_values[3];
          if (total > 0.001f) {
            uint32_t used = 0;
            packed_weights = packed_bones = 0;
            for (int k = 0; k < 4; ++k) {
              const uint32_t q =
                  k == 3
                      ? 255u - used
                      : std::min(255u - used,
                                 uint32_t(std::lround(out_values[k] * 255.0f /
                                                      total)));
              used += q;
              packed_weights |= q << (8 * k);
              packed_bones |= uint32_t(out_bones[k]) << (8 * k);
            }
          }
        } else {
          packed_weights = uint32_t(255 - aw) | (uint32_t(aw) << 8);
          packed_bones = uint32_t(body_bone) |
                         (uint32_t(arm_bone[side]) << 8);
        }
      }
    }
    std::memcpy(v + 7, &packed_weights, sizeof(packed_weights));
    std::memcpy(v + 8, &packed_bones, sizeof(packed_bones));
  }
  fit_max[2] += 0.045f;
  std::copy(std::begin(fit_min), std::end(fit_min),
            std::begin(rigged->bbox_min));
  std::copy(std::begin(fit_max), std::end(fit_max),
            std::begin(rigged->bbox_max));

  // Pick one sole sample per side at the bottom of the original skater.
  // A tiny centre bias chooses the planted inside edge rather than a stray
  // shoelace vertex, producing a stable board-height midpoint.
  const float center_x = 0.5f * (src_min[0] + src_max[0]);
  float best_score[2] = {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
  for (const ObservedVertex& v : cloud) {
    const int side = v.p[0] < center_x ? 0 : 1;
    const float score = v.p[1] + 0.02f * std::fabs(v.p[0] - center_x);
    if (score < best_score[side]) {
      best_score[side] = score;
      std::copy(std::begin(v.p), std::end(v.p),
                std::begin(rigged->sole[side].p));
      rigged->sole[side].weights = v.weights;
      rigged->sole[side].indices = v.indices;
    }
  }

  REXLOG_INFO(
      "penguin-mod: auto-rig ready from {} CAC meshes / {} source vertices; "
      "canonical={:08X}/{} bones match_err={:.5f}; bones body={} "
      "feet={}/{} arms={}/{}; bind box "
      "({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f})",
      g_skater_meshes.size(), total_vertices, rigged->anchor_mesh,
      canonical_count, worst_bone_match, body_bone, foot_bone[0],
      foot_bone[1], arm_bone[0], arm_bone[1],
      fit_min[0], fit_min[1], fit_min[2], fit_max[0], fit_max[1], fit_max[2]);
  g_rigged_asset = std::move(rigged);
  return g_rigged_asset.get();
}

void ApplyToFrame(native_scene::FrameScene& scene) {
  if (!REXCVAR_GET(skate3_penguin_mod)) return;
  for (const native_scene::DrawItem& item : scene.items) {
    ObserveSkaterPalette(item);
  }
  const Asset* rigged = GetRiggedAsset();
  if (rigged == nullptr) return;
  const Asset& asset = *rigged;

  // The CAC family is the local player's skin/face/clothes. Select the item
  // carrying the fullest palette as the pose/lighting anchor.
  const native_scene::DrawItem* anchor = nullptr;
  for (const native_scene::DrawItem& item : scene.items) {
    if (item.char_family == 2 && item.skinned && !item.bones.empty() &&
        (anchor == nullptr || item.bones.size() > anchor->bones.size())) {
      anchor = &item;
    }
    if (item.mesh == asset.anchor_mesh && item.skinned && !item.bones.empty()) {
      anchor = &item;
      break;
    }
  }
  if (anchor == nullptr) return;

  // One-shot replacement audit. Skateboard surfaces and tiny facial
  // accessories both use character materials, so family alone is not a
  // safe deletion key. Keep enough geometry/palette identity in the log to
  // classify the outfit without another guest capture.
  static bool audited_character_items = false;
  if (!audited_character_items) {
    audited_character_items = true;
    for (const native_scene::DrawItem& item : scene.items) {
      if (item.char_family == 0) continue;
      uint32_t drawn_indices = 0;
      for (const native_scene::DrawEntry& draw : item.draws) {
        drawn_indices += draw.index_count;
      }
      const float* pose = item.skinned && item.bones.size() >= 12
                              ? item.bones.data()
                              : item.world;
      REXLOG_INFO(
          "penguin-mod: audit mesh={:08X} ctx={:08X} fam={} skn={} bones={} "
          "ropa={} vb={} ib={}/{} box=({:.3f},{:.3f},{:.3f}).."
          "({:.3f},{:.3f},{:.3f}) pose=({:.2f},{:.2f},{:.2f}) tex={:08X}",
          item.mesh, item.ctx, item.char_family, item.skinned ? 1 : 0,
          item.bones.size() / 12, item.ropa ? 1 : 0, item.vb_bytes,
          drawn_indices, item.ib_count, item.bbox_min[0], item.bbox_min[1],
          item.bbox_min[2], item.bbox_max[0], item.bbox_max[1],
          item.bbox_max[2], pose[3], pose[7], pose[11], item.diffuse_tex);
    }
  }

  native_scene::DrawItem penguin = *anchor;
  // The transferred mesh occupies the original skater's bind space so every
  // limb rotates around the correct authored joint. Shrink the final skinned
  // result around the live midpoint of both soles instead: feet stay flush on
  // the board while the body, arm reach and trick motion scale together.
  const auto skin_anchor = [&](const Asset::Anchor& a, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    float total = 0.0f;
    for (int k = 0; k < 4; ++k) {
      const float weight = float(uint8_t(a.weights >> (8 * k))) / 255.0f;
      const uint32_t bone = uint8_t(a.indices >> (8 * k));
      if (weight <= 0.0f || size_t(bone) * 12 + 11 >= penguin.bones.size()) {
        continue;
      }
      const float* rows = penguin.bones.data() + size_t(bone) * 12;
      for (int axis = 0; axis < 3; ++axis) {
        const float* row = rows + axis * 4;
        out[axis] += weight *
                     (a.p[0] * row[0] + a.p[1] * row[1] +
                      a.p[2] * row[2] + row[3]);
      }
      total += weight;
    }
    if (total > 0.001f) {
      out[0] /= total;
      out[1] /= total;
      out[2] /= total;
      return true;
    }
    return false;
  };
  float sole_world[2][3] = {};
  const bool have_left = skin_anchor(asset.sole[0], sole_world[0]);
  const bool have_right = skin_anchor(asset.sole[1], sole_world[1]);
  float scale_origin[3] = {};
  if (have_left && have_right) {
    for (int axis = 0; axis < 3; ++axis) {
      scale_origin[axis] = 0.5f * (sole_world[0][axis] + sole_world[1][axis]);
    }
  } else {
    scale_origin[0] = penguin.bones[3];
    scale_origin[1] = penguin.bones[7];
    scale_origin[2] = penguin.bones[11];
  }
  const float mascot_scale = float(REXCVAR_GET(skate3_penguin_scale));
  const float board_offset = float(REXCVAR_GET(skate3_penguin_board_offset));
  for (size_t bone = 0; bone + 11 < penguin.bones.size(); bone += 12) {
    for (int axis = 0; axis < 3; ++axis) {
      float* row = penguin.bones.data() + bone + axis * 4;
      row[0] *= mascot_scale;
      row[1] *= mascot_scale;
      row[2] *= mascot_scale;
      row[3] = scale_origin[axis] +
               mascot_scale * (row[3] - scale_origin[axis]);
      if (axis == 1) row[3] += board_offset;
    }
  }
  penguin.mesh = kMeshKey;
  penguin.vb_obj = penguin.ib_obj = 0;
  penguin.vb_addr = penguin.ib_addr = 0;
  penguin.vb_bytes = uint32_t(asset.vertices.size() * sizeof(float));
  penguin.ib_count = uint32_t(asset.indices.size());
  penguin.diffuse_tex = penguin.lightmap_tex = 0;
  penguin.macro_tex = penguin.hair_alpha_tex = 0;
  penguin.decal_art = penguin.water_normal = penguin.water_normal2 = 0;
  penguin.water_env = penguin.spec_tex = penguin.detail_tex = 0;
  std::fill(std::begin(penguin.diffuse_fetch), std::end(penguin.diffuse_fetch), 0);
  std::fill(std::begin(penguin.decal_fetch), std::end(penguin.decal_fetch), 0);
  penguin.hair = penguin.decal = penguin.decal_tileable = false;
  penguin.transparent = penguin.water = penguin.water_flowing = false;
  penguin.water_ocean = 0;
  penguin.ropa = penguin.char_alpha = penguin.cloth_quads = false;
  penguin.env_family = penguin.dynobj = 0;
  penguin.pending = penguin.retained = penguin.selected = false;
  penguin.shadow_caster = true;
  penguin.fingerprint = 0x50454E4755494E32ull;  // "PENGUIN2"
  std::copy(std::begin(asset.bbox_min), std::end(asset.bbox_min),
            std::begin(penguin.bbox_min));
  std::copy(std::begin(asset.bbox_max), std::end(asset.bbox_max),
            std::begin(penguin.bbox_max));
  penguin.draws = {{4, 0, 0, uint32_t(asset.indices.size())}};

  scene.items.erase(
      std::remove_if(scene.items.begin(), scene.items.end(),
                     [](const native_scene::DrawItem& item) {
                       // Family 2 is CAC skin/face/clothes; family 4 is the
                       // CAC-only hair pass. Keeping family 4 leaves the
                       // original skater's hair floating behind the penguin.
                       // The deck can use family 2 as well, so preserve its
                       // long planar mesh explicitly. Eyes/teeth instead use
                       // family 1 and are removed by their compact high bind
                       // bounds; trucks/wheels remain below that region.
                       if ((item.char_family == 2 || item.char_family == 4) &&
                           !IsSkateboardDeck(item)) {
                         return true;
                       }
                       return IsOriginalFaceFragment(item);
                     }),
      scene.items.end());
  scene.items.push_back(std::move(penguin));

  static bool announced = false;
  if (!announced) {
    announced = true;
    REXLOG_INFO("penguin-mod: playable character replacement active");
  }
}

}  // namespace skate3::penguin_mod
