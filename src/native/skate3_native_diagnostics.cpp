// Native renderer diagnostics: offline-analysis recording, camera/bone
// signal recorders, synthetic-pan probe. Extracted verbatim from
// skate3_native_scene.cpp; no rendering logic lives here.

#include "native/skate3_native_diag.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#include <rex/logging.h>

namespace skate3::native_scene {

// ---- Offline-analysis recording ---------------------------------------------

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
std::vector<PendingInlineDump> g_pending_inline_dumps;
std::unordered_map<uint64_t, uint32_t> g_frame_dump_ids;

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
          << "\",\"diffuse_fetch\":\"" << d.diffuse_fetch[1]
          << "\",\"lightmap\":\"" << d.lightmap_tex << "\",\"macro\":\"" << d.macro_tex
          << "\",\"decal_art\":\"" << d.decal_art
          << "\",\"decal_fetch\":\"" << d.decal_fetch[1]
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

// ---- Camera-signal recorder ---------------------------------------------------

namespace {
struct CamSigEntry {
  double t;       // host time (sampler push / frame build)
  double play_t;  // kind 1 only: the smoother's playback time
  float yaw;      // heading, degrees
  uint8_t kind;   // 0 = raw sampler pose, 1 = smoothed frame, 2 = raw frame
};
std::mutex g_camsig_mutex;
std::vector<CamSigEntry> g_camsig;
}  // namespace
std::atomic<double> g_camsig_deadline{0.0};

float YawFromViewRows(const float view[16]) {
  return std::atan2(view[2], view[10]) * float(180.0 / 3.14159265358979323846);
}

void CamSigPush(double t, double play_t, float yaw, uint8_t kind) {
  std::lock_guard<std::mutex> lock(g_camsig_mutex);
  if (g_camsig.size() < 200000) {
    g_camsig.push_back({t, play_t, yaw, kind});
  }
}

void CamSigFrameTick(double rec_now, const float cam_view[16],
                     const float* smoothed_rot, double play_t) {
  const double dl = g_camsig_deadline.load(std::memory_order_relaxed);
  if (dl <= 0.0) {
    return;
  }
  if (rec_now < dl) {
    std::lock_guard<std::mutex> lock(g_camsig_mutex);
    if (g_camsig.size() < 200000) {
      g_camsig.push_back({rec_now, 0.0, YawFromViewRows(cam_view), 2});
      if (smoothed_rot) {
        g_camsig.push_back({rec_now, play_t, YawFromViewRows(smoothed_rot), 1});
      }
    }
  } else {
    std::vector<CamSigEntry> entries;
    {
      std::lock_guard<std::mutex> lock(g_camsig_mutex);
      entries.swap(g_camsig);
    }
    g_camsig_deadline.store(0.0, std::memory_order_release);
    std::thread([entries = std::move(entries)]() {
      std::error_code ec;
      std::filesystem::create_directories("logs", ec);
      char path[128];
      std::snprintf(path, sizeof(path), "logs/cam_signal_%lld.csv",
                    static_cast<long long>(std::time(nullptr)));
      std::ofstream f(path);
      f << "kind,t,play_t,yaw_deg\n";
      char line[128];
      for (const CamSigEntry& e : entries) {
        std::snprintf(line, sizeof(line), "%d,%.6f,%.6f,%.5f\n", int(e.kind), e.t,
                      e.play_t, double(e.yaw));
        f << line;
      }
      REXLOG_INFO("native-scene cam-signal: wrote {} entries -> {}", entries.size(),
                  path);
    }).detach();
  }
}

void RecordCameraSignal(double seconds) {
  {
    std::lock_guard<std::mutex> lock(g_camsig_mutex);
    g_camsig.clear();
    g_camsig.reserve(size_t(seconds * 1400.0));
  }
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  g_camsig_deadline.store(now + seconds, std::memory_order_release);
  REXLOG_INFO(
      "native-scene cam-signal: recording {} s; pan the camera with the stick at a "
      "steady rate NOW",
      seconds);
}

// ---- Bone-signal recorder -----------------------------------------------------

namespace {
std::vector<uint8_t> g_bonesig;
std::atomic<double> g_bonesig_deadline{0.0};
// Armed from the UI thread (F12 button), consumed on the guest thread; the
// blob itself is guest-thread-only.
std::atomic<double> g_bonesig_request{0.0};
}  // namespace

void BoneSigAppend(uint8_t kind, uint64_t key, double t, double play, const float* v,
                   uint32_t n) {
  if (g_bonesig.size() > (200u << 20)) {
    return;  // runaway cap
  }
  const size_t off = g_bonesig.size();
  g_bonesig.resize(off + 29 + size_t(n) * 4);
  uint8_t* p = g_bonesig.data() + off;
  *p = kind;
  std::memcpy(p + 1, &key, 8);
  std::memcpy(p + 9, &t, 8);
  std::memcpy(p + 17, &play, 8);
  std::memcpy(p + 25, &n, 4);
  std::memcpy(p + 29, v, size_t(n) * 4);
}

bool BoneSigTick(double now) {
  const double req = g_bonesig_request.exchange(0.0, std::memory_order_acq_rel);
  if (req > 0.0) {
    g_bonesig.clear();
    g_bonesig.reserve(16u << 20);
    g_bonesig_deadline.store(now + req, std::memory_order_relaxed);
  }
  const double dl = g_bonesig_deadline.load(std::memory_order_relaxed);
  if (dl <= 0.0) {
    return false;
  }
  if (now < dl) {
    return true;
  }
  std::vector<uint8_t> blob;
  blob.swap(g_bonesig);
  g_bonesig_deadline.store(0.0, std::memory_order_release);
  std::thread([blob = std::move(blob)]() {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    char path[128];
    std::snprintf(path, sizeof(path), "logs/bone_signal_%lld.bin",
                  static_cast<long long>(std::time(nullptr)));
    std::ofstream f(path, std::ios::binary);
    f.write("BSIG1\n", 6);
    f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    REXLOG_INFO("native-scene bone-signal: wrote {} bytes -> {}", blob.size() + 6,
                path);
  }).detach();
  return false;
}

void RecordBoneSignal(double seconds) {
  g_bonesig_request.store(seconds, std::memory_order_release);
  REXLOG_INFO(
      "native-scene bone-signal: recording {} s; skate past the camera at a steady "
      "speed NOW",
      seconds);
}

// ---- Synthetic camera pan -----------------------------------------------------

std::atomic<int> g_synpan_active{0};  // engaged mode (0 = off)
std::mutex g_synpan_mutex;
float g_synpan_view0[16];  // raw guest view at engage
float g_synpan_proj0[16];
float g_synpan_c0[3];   // camera world position at engage (held fixed)
double g_synpan_t0 = 0.0;
double g_synpan_step_phase = 0.0;  // mode 2: accumulated phase (degrees)
double g_synpan_ema_dt = 0.0;      // mode 2: slow EMA of the publish dt
uint64_t g_synpan_frames = 0;
double g_synpan_last_build = 0.0;
double g_synpan_dt_sum = 0.0, g_synpan_dt_sum2 = 0.0;
double g_synpan_dt_min = 0.0, g_synpan_dt_max = 0.0;
double g_synpan_err_sum2 = 0.0, g_synpan_err_max = 0.0;
uint64_t g_synpan_err_n = 0;
std::unordered_map<uint64_t, DrawItem> g_synpan_union;

uint64_t SynPanItemKey(const DrawItem& it) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(it.mesh);
  mix(it.vb_obj);
  mix(it.ib_obj);
  mix(std::bit_cast<uint32_t>(it.world[12]));
  mix(std::bit_cast<uint32_t>(it.world[13]));
  mix(std::bit_cast<uint32_t>(it.world[14]));
  return h;
}

double SynPanAngleDeg(double phase_deg, double amp) {
  if (amp <= 0.0) {
    return phase_deg;
  }
  const double period = 4.0 * amp;
  double m = std::fmod(phase_deg, period);
  if (m < 0.0) {
    m += period;
  }
  return m < amp ? m : (m < 3.0 * amp ? 2.0 * amp - m : m - period);
}

void SynPanView(double angle_deg, float view_out[16]) {
  const double a = angle_deg * (3.14159265358979323846 / 180.0);
  const float c = float(std::cos(a)), s = float(std::sin(a));
  const float ry[3][3] = {{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}};
  std::memset(view_out, 0, 16 * sizeof(float));
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      float sum = 0.0f;
      for (int k = 0; k < 3; ++k) {
        sum += ry[i][k] * g_synpan_view0[k * 4 + j];
      }
      view_out[i * 4 + j] = sum;
    }
  }
  for (int k = 0; k < 3; ++k) {
    view_out[12 + k] = -(g_synpan_c0[0] * view_out[0 * 4 + k] +
                         g_synpan_c0[1] * view_out[1 * 4 + k] +
                         g_synpan_c0[2] * view_out[2 * 4 + k]);
  }
  view_out[15] = 1.0f;
}

}  // namespace skate3::native_scene
