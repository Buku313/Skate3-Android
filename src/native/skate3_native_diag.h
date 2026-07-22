#pragma once

// Internal diagnostics interface shared between skate3_native_scene.cpp and
// skate3_native_diagnostics.cpp: the offline-analysis recording
// (StartRecording/WriteRecording), the camera/bone signal recorders and the
// synthetic-pan probe. Pure diagnosis machinery; nothing here participates
// in rendering; capture call sites live in the scene TU because they read
// per-draw locals, but all state and file writing lives in the diagnostics
// TU.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "skate3_native_scene.h"

namespace skate3::native_scene {

// ---- Frame-end capture diagnostics (skate3_native_diagnostics.cpp) --------
// Hotkey/cvar/pad-armed guest-memory snapshots (.gsnap + metadata) and the
// per-frame record collection windows. Called from the frame-boundary hook
// under its record lock; may move the frame's records into an open capture
// window. SnapshotWritten() reports whether a one-shot snapshot completed.
void OnCaptureFrameEnd(uint8_t* base, uint64_t frame_index,
                       std::vector<SubmitRecord>& current_frame);
bool SnapshotWritten();


// ---- Offline-analysis recording (see StartRecording/WriteRecording) --------
// The full per-draw constant bank stream is the ground truth for what the
// game's shaders saw; the per-frame item lists are what our pipeline made of
// it.
struct RecordedDraw {
  uint32_t frame;
  uint32_t func;  // 0 = DrawIndexedVertices, 1 = DrawVertices, 2 = BeginVertices
  uint32_t ib;
  uint32_t vb;
  uint32_t vb_offset;      // stream-0 OffsetInBytes at bind time
  uint32_t vb_stride;      // stream-0 stride at bind time
  uint32_t streams[4][3];  // all streams: {vb, offset, stride}
  uint32_t vfetch[12];     // fetch-constant shadow groups 0-1 (device+0x480)
  uint32_t args[4];
  float bank[1024];  // VS c0..c255
  float ps[256];     // PS c0..c63 (material colors, e.g. CAS hair tint)
  // 2D recon fields (SK3DRAW7):
  uint32_t flags2d;      // bit per active 2D bracket (see On2dPhase)
  uint32_t ps_obj;       // current guest pixel/vertex shader objects
  uint32_t vs_obj;
  uint32_t viewport[6];  // last SetViewport payload (raw guest dwords)
  uint32_t scissor[4];   // last SetScissorRect payload
  uint32_t rstates[256];    // render-state shadow snapshot (blend/depth)
  uint32_t vfetch_all[192];  // full 32-slot texture fetch shadow
  uint32_t vb_dump;  // index into buffers.bin for this draw's payloads
  uint32_t ib_dump;  // (~0u = none)
};
struct RecordedFrame {
  uint64_t generation;
  float view_proj[16];
  float cam_pos[3];
  std::vector<DrawItem> dynitems;
  std::vector<DrawItem> items;
};
// Dynamic meshes live in streaming arenas that are recycled DURING a long
// recording; the end-of-window .gsnap holds garbage for them. Dump each
// captured buffer payload once per content fingerprint so offline tools
// decode exactly what the game used.
struct RecordedBuffer {
  uint32_t vb_addr;
  uint32_t ib_addr;
  uint64_t fingerprint;
  std::vector<uint8_t> vb;
  std::vector<uint8_t> ib;
};
// BeginVertices (inline ring) payloads are written by the CPU AFTER the
// call returns; dump them at frame end, when the writes are complete but
// the ring has not yet wrapped. draw_index links the dump back to its
// RecordedDraw.
struct PendingInlineDump {
  uint32_t addr;
  uint32_t bytes;
  size_t draw_index;
};

extern std::mutex g_record_mutex;
extern std::atomic<bool> g_recording;
extern uint32_t g_record_frame;   // index of the NEXT recorded frame
extern uint32_t g_record_stride;  // record every Nth frame
extern uint32_t g_frames_seen;    // frames completed since StartRecording
extern std::vector<std::unique_ptr<RecordedDraw>> g_recorded_draws;
extern std::vector<RecordedFrame> g_recorded_frames;
extern std::vector<RecordedBuffer> g_recorded_buffers;
extern std::unordered_set<uint64_t> g_recorded_buffer_keys;
extern size_t g_recorded_buffer_bytes;
extern std::vector<PendingInlineDump> g_pending_inline_dumps;
// One payload dump per (guest address, frame): 2D dynamic buffers hold many
// draws' vertices; the repeated full-buffer dump would be pure duplication.
extern std::unordered_map<uint64_t, uint32_t> g_frame_dump_ids;

// ---- Camera-signal recorder (judder diagnosis) ------------------------------
// Records the guest camera SIGNAL during a real stick pan: every distinct
// pose the ~1 kHz sampler accepts (kind 0, precise host timestamps), the raw
// per-frame pose at scene build (kind 2) and the smoother's output (kind 1,
// with its playback time). Written as logs/cam_signal_<ts>.csv when the
// window closes. Entry storage is private to the diagnostics TU; call sites
// gate on the deadline before computing anything.
extern std::atomic<double> g_camsig_deadline;  // record until this host time

// Heading from a row-vector view matrix: camera forward in world space is
// the view rotation's third column (v[2], v[6], v[10]).
float YawFromViewRows(const float view[16]);

// Append one entry while the window is open (bounded; thread-safe).
void CamSigPush(double t, double play_t, float yaw, uint8_t kind);

// Per-frame tick (guest render thread, scene build): appends the raw frame
// pose (kind 2) and, when smoothed_rot is non-null, the smoother's output
// (kind 1 at play_t); writes + resets once the window closes. Call only when
// g_camsig_deadline > 0.
void CamSigFrameTick(double rec_now, const float cam_view[16],
                     const float* smoothed_rot, double play_t);

// ---- Bone-signal recorder (wheel-guard diagnosis) ---------------------------
// Records the raw per-entity pose stream (every ring push: bone palettes /
// rigid worlds with timestamps, kind 0) plus what the interpolation actually
// output each frame (kind 1, with the playback time) to
// logs/bone_signal_<ts>.bin. Binary records after a "BSIG1\n" header:
// u8 kind, u64 key, f64 t, f64 play, u32 n, float[n]. Guest render thread
// only (written off-thread at close).
//
// Per-frame arm/flush tick (guest render thread): consumes a pending
// RecordBoneSignal request, returns true while the window is open, and
// spawns the file write once it closes.
bool BoneSigTick(double now);
void BoneSigAppend(uint8_t kind, uint64_t key, double t, double play, const float* v,
                   uint32_t n);

// ---- Synthetic camera pan (judder isolation probe) --------------------------
// See the skate3_native_render_scene_synthetic_pan cvar comment for the mode
// semantics. Engage state is captured from the RAW guest camera on the guest
// render thread; the sampler thread reads it for mode 3 under g_synpan_mutex.
extern std::atomic<int> g_synpan_active;  // engaged mode (0 = off)
extern std::mutex g_synpan_mutex;
extern float g_synpan_view0[16];  // raw guest view at engage
extern float g_synpan_proj0[16];
extern float g_synpan_c0[3];   // camera world position at engage (held fixed)
extern double g_synpan_t0;
extern double g_synpan_step_phase;  // mode 2: accumulated phase (degrees)
extern double g_synpan_ema_dt;      // mode 2: slow EMA of the publish dt
// Telemetry (guest render thread only).
extern uint64_t g_synpan_frames;
extern double g_synpan_last_build;
extern double g_synpan_dt_sum, g_synpan_dt_sum2;
extern double g_synpan_dt_min, g_synpan_dt_max;
extern double g_synpan_err_sum2, g_synpan_err_max;
extern uint64_t g_synpan_err_n;

// Full-surround world union for the probe: the game only submits what ITS
// frustum sees each frame, so a synthetic pan away from the real heading
// would show an empty world (sky dome only). While the probe is engaged,
// every static item published is accumulated by identity and appended to
// each frame's scene; sweep the REAL camera around once with the stick
// after engaging and the union fills in the whole surround, making a full
// 360-degree spin (amplitude 0) usable. Guest render thread only.
extern std::unordered_map<uint64_t, DrawItem> g_synpan_union;

uint64_t SynPanItemKey(const DrawItem& it);

// Pan phase (degrees of accumulated rotation) -> heading angle in degrees.
// Triangle wave in [-amp, +amp] starting at 0 heading upward; amp <= 0 =
// unbounded continuous rotation.
double SynPanAngleDeg(double phase_deg, double amp);

// View matrix for the engage pose yawed by `angle_deg` about world up (+Y),
// camera position held at the engage position. Row-vector convention
// throughout (p_view = p * V): the 3x3 becomes Ry * R0 (world rotates first,
// then the engage view, a level pan; direction sign is irrelevant for the
// probe), translation row rebuilt as -c0 * R'.
void SynPanView(double angle_deg, float view_out[16]);

}  // namespace skate3::native_scene
