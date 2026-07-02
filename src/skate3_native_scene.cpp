#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>

#if defined(REX_HAS_D3D12) && REX_HAS_D3D12
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <d3dcompiler.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_native_render_scene, false, "Skate 3",
                    "Render the game scene natively from the hooked MeshContext stream, "
                    "replacing the emulated GPU output (requires skate3_native_render)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(skate3_native_render_scene_debug, 0, "Skate 3",
                     "Native scene debug: 0=normal, 1=clear only, 2=solid color per item, "
                     "3=limit to 20 items, 4=depth test disabled")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_scene {
namespace {

// Verified guest structure offsets.
constexpr uint32_t kCtxMatrix = 0x10;
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
std::atomic<uint8_t*> g_guest_base{nullptr};
std::atomic<uint64_t> g_frames_rendered{0};

bool SceneEnabled() { return REXCVAR_GET(skate3_native_render_scene); }

float LoadGuestF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  return std::bit_cast<float>(bits);
}

bool GuestReadableApprox(uint8_t* base, uint32_t addr) {
  // The hook layer only walks pointers the game is actively rendering from;
  // they are mapped. Reject null/small.
  (void)base;
  return addr >= 0x10000;
}

// Walk one MeshContext into a DrawItem. Returns false if any pointer in the
// chain is implausible. use_matrix: world geometry has absolute coordinates
// and its ctx+0x10 points into a transient per-frame matrix arena; reading
// it late picks up recycled garbage, so world items force identity.
bool BuildItem(uint8_t* base, uint32_t ctx, bool use_matrix, DrawItem& item) {
  const uint32_t record = REX_LOAD_U32(ctx);
  if (!GuestReadableApprox(base, record)) return false;
  const uint32_t mesh = REX_LOAD_U32(record);
  if (!GuestReadableApprox(base, mesh)) return false;

  const uint32_t vdesc = REX_LOAD_U32(mesh + kMeshVertexDescriptor);
  const uint32_t ib = REX_LOAD_U32(mesh + kMeshIndexBuffer);
  const uint32_t vb = REX_LOAD_U32(mesh + kMeshVertexBuffer);
  if (!GuestReadableApprox(base, vdesc) || !GuestReadableApprox(base, ib) ||
      !GuestReadableApprox(base, vb)) {
    return false;
  }

  // Vertex descriptor: find the stream-0 position element.
  const uint32_t num_elements = REX_LOAD_U16(vdesc + 8);
  if (num_elements == 0 || num_elements > 32) return false;
  bool have_pos = false;
  for (uint32_t i = 0; i < num_elements; ++i) {
    const uint32_t e = vdesc + 0x10 + i * 16;
    const uint32_t stream = REX_LOAD_U16(e);
    const uint32_t usage = REX_LOAD_U8(e + 9);
    if (stream == 0 && usage == 0) {
      item.pos_offset = REX_LOAD_U16(e + 2);
      item.pos_fmt = uint8_t(REX_LOAD_U32(e + 4) & 0x3F);
      have_pos = true;
      break;
    }
  }
  if (!have_pos) return false;
  item.stride = REX_LOAD_U8(vdesc + (num_elements + 1) * 16);
  if (item.stride == 0) return false;

  // Skip the backdrop/ocean/sky class (multi-km bounds). The camera sits
  // inside these volumes, their near-plane-crossing triangles rasterize as
  // full-screen cover at depth ~0, and the real game renders them with
  // dedicated passes. Mesh BBox = two Vector4s at +0x00/+0x10.
  for (int axis = 0; axis < 3; ++axis) {
    const float lo = LoadGuestF32(base, mesh + axis * 4);
    const float hi = LoadGuestF32(base, mesh + 0x10 + axis * 4);
    if (!(hi - lo < 1500.0f)) {
      return false;
    }
    item.bbox_min[axis] = lo;
    item.bbox_max[axis] = hi;
  }

  item.mesh = mesh;
  item.vb_addr = REX_LOAD_U32(vb + kBufferPhysAddr) & 0xFFFFFFFC;
  item.vb_bytes = REX_LOAD_U32(vb + kVbBytes);
  item.ib_addr = REX_LOAD_U32(ib + kBufferPhysAddr) & 0xFFFFFFFC;
  item.ib_count = REX_LOAD_U32(ib + kIbCount);
  if (item.vb_addr == 0 || item.ib_addr == 0 || item.vb_bytes == 0 ||
      item.ib_count == 0 || item.vb_bytes % item.stride != 0) {
    return false;
  }

  // Culled island draw list from the context.
  const uint32_t draw_count = REX_LOAD_U16(ctx + kCtxDrawCountU16);
  const uint32_t draw_list = REX_LOAD_U32(ctx + kCtxDrawList);
  if (draw_count == 0 || draw_count > 512 || !GuestReadableApprox(base, draw_list)) {
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
  if (item.draws.empty()) return false;

  // Payload fingerprint (FNV-1a over sampled VB/IB bytes + sizes) so the
  // renderer re-decodes when streaming replaces the data at this address.
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h = (h ^ v) * 1099511628211ull;
  };
  mix(item.vb_bytes);
  mix(item.ib_count);
  const uint32_t vb_end = item.vb_addr + item.vb_bytes - 32;
  const uint32_t ib_end = item.ib_addr + item.ib_count * 2 - 32;
  for (uint32_t off = 0; off < 32; off += 8) {
    mix(REX_LOAD_U64(item.vb_addr + off));
    mix(REX_LOAD_U64(vb_end + off));
    mix(REX_LOAD_U64(item.ib_addr + off));
    mix(REX_LOAD_U64(ib_end + off));
  }
  item.fingerprint = h;

  // Transform (identity for world geometry, instance matrix for props;
  // characters get their bone array's first matrix, which roughly places
  // them until skinning is implemented).
  const uint32_t mtx = use_matrix ? REX_LOAD_U32(ctx + kCtxMatrix) : 0;
  if (mtx != 0 && GuestReadableApprox(base, mtx)) {
    for (int i = 0; i < 16; ++i) {
      item.world[i] = LoadGuestF32(base, mtx + i * 4);
    }
  } else {
    std::memset(item.world, 0, sizeof(item.world));
    item.world[0] = item.world[5] = item.world[10] = item.world[15] = 1.0f;
  }
  return true;
}

}  // namespace

bool Enabled() { return SceneEnabled(); }

void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count) {
  if (!SceneEnabled() || count == 0) {
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
  for (size_t i = 0; i < count; ++i) {
    const SubmitRecord& r = records[i];
    // Dynamic entities (kind 0) are skinned characters/props rendered in
    // bind pose without bone matrices; the skater's limbs sprawl across the
    // camera. Skip until skinning is implemented; render world geometry only.
    if (r.kind == 0) {
      continue;
    }
    // Primary opaque list of the chosen view only; other lists (shadow
    // culling, transparents, z-prepass) duplicate the same geometry through
    // different MeshContext objects and z-fight.
    if (r.c != view || r.b != 20160) {
      continue;
    }
    if (!seen.insert(r.a).second) {
      continue;
    }
    DrawItem item;
    if (BuildItem(base, r.a, /*use_matrix=*/r.kind == 0, item)) {
      scene.items.push_back(std::move(item));
    }
  }
  if (scene.items.empty()) {
    return;
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

  std::lock_guard<std::mutex> lock(g_scene_mutex);
  scene.generation = ++g_generation;
  g_scene = std::move(scene);
}

}  // namespace skate3::native_scene

#if defined(REX_HAS_D3D12) && REX_HAS_D3D12

namespace skate3::native_scene {
namespace {

using rex::graphics::NativeGuestOutputBackend;
using rex::graphics::NativeGuestOutputRenderContext;

struct MeshBuffers {
  ID3D12Resource* vb = nullptr;
  ID3D12Resource* ib = nullptr;
  D3D12_VERTEX_BUFFER_VIEW vb_view{};
  D3D12_INDEX_BUFFER_VIEW ib_view{};
  uint64_t fingerprint = 0;
};

struct RendererState {
  ID3D12Device* device = nullptr;
  ID3D12RootSignature* root_signature = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12PipelineState* pso_nodepth = nullptr;
  DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  ID3D12DescriptorHeap* dsv_heap = nullptr;
  ID3D12Resource* depth = nullptr;
  uint32_t depth_width = 0;
  uint32_t depth_height = 0;
  ID3D12Resource* rtv_resource = nullptr;
  std::unordered_map<uint32_t, MeshBuffers> meshes;
  // Buffers replaced by re-decode, kept alive until the GPU has finished the
  // submission that last referenced them.
  std::vector<std::pair<ID3D12Resource*, uint64_t>> retired;
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
};
struct VSOut { float4 pos : SV_Position; float3 rpos : TEXCOORD0; };
VSOut vs_main(float3 p : POSITION) {
  VSOut o;
  o.pos = mul(float4(p, 1.0), mvp);
  o.rpos = mul(float4(p, 1.0), world).xyz - cam_pos.xyz;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  if (tint.a > 0.0) {
    return tint;
  }
  float3 n = normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  float l = abs(dot(n, normalize(float3(0.4, 0.8, 0.3)))) * 0.6 + 0.35;
  return float4(l, l, l, 1.0);
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

// Decode guest vertices into float3 positions.
bool DecodeMesh(ID3D12Device* device, uint8_t* base, const DrawItem& item,
                MeshBuffers& out) {
  const uint32_t num_verts = item.vb_bytes / item.stride;
  if (num_verts == 0) return false;
  ID3D12Resource* vb = CreateUploadBuffer(device, size_t(num_verts) * 12);
  ID3D12Resource* ib = CreateUploadBuffer(device, size_t(item.ib_count) * 2);
  if (!vb || !ib) {
    if (vb) vb->Release();
    if (ib) ib->Release();
    return false;
  }

  const uint8_t* src_vb = base + item.vb_addr;
  float* dst = nullptr;
  uint32_t garbage = 0;
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
    dst[v * 3 + 0] = x;
    dst[v * 3 + 1] = y;
    dst[v * 3 + 2] = z;
  }
  vb->Unmap(0, nullptr);
  if (garbage != 0) {
    REXLOG_WARN(
        "native-scene: mesh {:08X} decoded {} of {} verts outside bbox "
        "({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) fmt {} stride {} vb {:08X}",
        item.mesh, garbage, num_verts, item.bbox_min[0], item.bbox_min[1], item.bbox_min[2],
        item.bbox_max[0], item.bbox_max[1], item.bbox_max[2], item.pos_fmt, item.stride,
        item.vb_addr);
  }

  const uint16_t* src_ib = reinterpret_cast<const uint16_t*>(base + item.ib_addr);
  uint16_t* dst_ib = nullptr;
  ib->Map(0, nullptr, reinterpret_cast<void**>(&dst_ib));
  for (uint32_t i = 0; i < item.ib_count; ++i) {
    dst_ib[i] = SwapU16(src_ib[i]);
  }
  ib->Unmap(0, nullptr);

  out.vb = vb;
  out.ib = ib;
  out.vb_view = {vb->GetGPUVirtualAddress(), num_verts * 12u, 12u};
  out.ib_view = {ib->GetGPUVirtualAddress(), item.ib_count * 2u, DXGI_FORMAT_R16_UINT};
  return true;
}

bool EnsurePipeline(const NativeGuestOutputRenderContext& context) {
  if (g_r.failed) return false;
  ID3D12Device* device = context.d3d12.device;
  g_r.device = device;

  if (!g_r.root_signature) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.Num32BitValues = 40;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!context.d3d12.create_root_signature(context.d3d12.command_processor_user_data,
                                             &desc, &g_r.root_signature)) {
      REXLOG_ERROR("native-scene: root signature creation failed");
      g_r.failed = true;
      return false;
    }
  }

  if (!g_r.pso || g_r.rtv_format != context.d3d12.guest_output_format) {
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
    D3D12_INPUT_ELEMENT_DESC input{
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
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
    pso.InputLayout = {&input, 1};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = context.d3d12.guest_output_format;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    const HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso));
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    const HRESULT hr2 =
        device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_r.pso_nodepth));
    vs->Release();
    ps->Release();
    if (errors) errors->Release();
    if (FAILED(hr) || FAILED(hr2)) {
      REXLOG_ERROR("native-scene: PSO creation failed {:08X}/{:08X}", uint32_t(hr),
                   uint32_t(hr2));
      g_r.failed = true;
      return false;
    }
    g_r.rtv_format = context.d3d12.guest_output_format;
  }

  if (!g_r.rtv_heap) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_r.rtv_heap)))) {
      g_r.failed = true;
      return false;
    }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_r.dsv_heap)))) {
      g_r.failed = true;
      return false;
    }
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
    desc.SampleDesc.Count = 1;
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

  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        context.d3d12.guest_output_resource,
                                        context.d3d12.guest_output_initial_state,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  const int32_t debug_mode_early = REXCVAR_GET(skate3_native_render_scene_debug);
  const bool use_depth = debug_mode_early != 4;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_r.dsv_heap->GetCPUDescriptorHandleForHeapStart();
  const FLOAT clear_color[4] = {0.25f, 0.35f, 0.55f, 1.0f};
  list.D3DClearRenderTargetView(rtv, clear_color, 0, nullptr);
  if (use_depth) {
    list.D3DClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list.D3DOMSetRenderTargets(1, &rtv, FALSE, &dsv);
  } else {
    list.D3DOMSetRenderTargets(1, &rtv, FALSE, nullptr);
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

  const int32_t debug_mode = REXCVAR_GET(skate3_native_render_scene_debug);
  uint32_t drawn = 0;
  uint32_t item_index = 0;
  for (const DrawItem& item : scene.items) {
    const uint32_t index = item_index++;
    if (debug_mode == 1) {
      break;
    }
    if (debug_mode == 3 && index >= 20) {
      break;
    }
    auto it = g_r.meshes.find(item.mesh);
    if (it != g_r.meshes.end() && it->second.fingerprint != item.fingerprint) {
      const uint64_t submission = command_processor->GetCurrentSubmission();
      g_r.retired.emplace_back(it->second.vb, submission);
      g_r.retired.emplace_back(it->second.ib, submission);
      g_r.meshes.erase(it);
      it = g_r.meshes.end();
    }
    if (it == g_r.meshes.end()) {
      MeshBuffers buffers;
      if (!DecodeMesh(g_r.device, base, item, buffers)) {
        continue;
      }
      buffers.fingerprint = item.fingerprint;
      it = g_r.meshes.emplace(item.mesh, buffers).first;
    }
    const MeshBuffers& buffers = it->second;

    // constants = world + mvp (world * view_proj, row-vector) + tint + cam.
    float constants[40];
    std::memcpy(constants, item.world, sizeof(item.world));
    float* mvp = constants + 16;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += item.world[r * 4 + k] * scene.view_proj[k * 4 + c];
        }
        mvp[r * 4 + c] = sum;
      }
    }
    if (debug_mode >= 2) {
      constants[32] = float((index * 37u) % 255u) / 255.0f;
      constants[33] = float((index * 73u) % 255u) / 255.0f;
      constants[34] = float((index * 151u) % 255u) / 255.0f;
      constants[35] = 1.0f;
    } else {
      constants[32] = constants[33] = constants[34] = constants[35] = 0.0f;
    }
    constants[36] = scene.cam_pos[0];
    constants[37] = scene.cam_pos[1];
    constants[38] = scene.cam_pos[2];
    constants[39] = 0.0f;
    list.D3DSetGraphicsRoot32BitConstants(0, 40, constants, 0);
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
  }

  context.d3d12.push_transition_barrier(context.d3d12.command_processor_user_data,
                                        context.d3d12.guest_output_resource,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        context.d3d12.guest_output_initial_state);
  context.d3d12.submit_barriers(context.d3d12.command_processor_user_data);

  const uint64_t frames = g_frames_rendered.fetch_add(1) + 1;
  if (frames % 600 == 0) {
    REXLOG_INFO("native-scene: frame {} items={} draws={} cached_meshes={}", frames,
                scene.items.size(), drawn, g_r.meshes.size());
  }
  return true;
}

}  // namespace

void Install() {
  if (!SceneEnabled()) {
    return;
  }
  rex::graphics::SetNativeGuestOutputRenderer(&RenderScene, nullptr);
  REXLOG_INFO("native-scene: guest output renderer registered");
}

}  // namespace skate3::native_scene

#else  // !REX_HAS_D3D12

namespace skate3::native_scene {
void Install() {}
}  // namespace skate3::native_scene

#endif  // REX_HAS_D3D12
