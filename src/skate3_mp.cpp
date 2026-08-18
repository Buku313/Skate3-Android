// Milestone-1 LAN multiplayer: co-op free-skate ghost.
//
// Two instances of the game on a LAN (or over the internet via direct IP)
// exchange their local skater's world pose over UDP and each renders the
// remote player by PUPPETING a LivingWorld presentation entity (a census
// pedestrian): the puppet's world matrices are overwritten with the remote
// pose every sim tick, so the other player appears as a ped standing/
// sliding at the remote skater's position and orientation.
//
// Why puppet a pedestrian (the render-side decision):
//
//  (a) A second Sk8::SkaterPresEntity is not available: Skate 3 has no
//      local split-screen, so in free skate exactly one skater entity
//      exists (the local player). NPC skaters only exist inside specific
//      missions, and inducing a real skater spawn means driving deep game
//      logic (team/AI setup) we have not mapped. Not milestone-1 viable.
//
//  (b) Drawing a placeholder through the native scene renderer
//      (src/src/skate3_native_scene.cpp) would need a mesh + skinning
//      palette + draw-item injection into an already intricate pipeline.
//      Too much machinery for a first draft.
//
//  (c) Puppeting an existing entity needs only guest-memory writes to
//      fields the game itself re-reads every frame. LivingWorld entities
//      (census pedestrians, traffic vehicles, spawned props) are always
//      present in free skate and are ticked per frame through
//      cLivingWorldPresEntity::Update (sub_827C1188), which is already
//      overridden in skate3_native_render.cpp. Simplest viable path.
//
// Guest facts relied on (all previously verified; see the cited files):
//
//  - PresentationEntity layout (src/src/native/skate3_native_entity.cpp:75-78):
//      +352 m_MatLtoW      Matrix44, row-major, translation in ROW 3
//      +416 m_MatLtoWTrans Matrix44, the transpose used for constant
//                          upload: translation in COLUMN 3 (m[3], m[7],
//                          m[11]), tail row (0,0,0,1) — layout validated by
//                          ReadWorldRowsChecked (skate3_native_entity.cpp:469).
//      +528 LivingWorld opacity Vector4, x = alpha, re-evaluated every
//                          Update as a spawn/distance fade
//                          (skate3_native_scene.cpp:3710-3718).
//
//  - sub_82782818 = Sk8::SkaterPresEntity::EndJobs: runs once per skater
//    per sim tick; on exit the entity's +416 holds the tick's final
//    locomotion ("the body pose of record",
//    skate3_native_render.cpp:512-520). Local-pose read point.
//
//  - sub_827C1188 = Sk8::cLivingWorldPresEntity::Update: runs once per LW
//    entity per sim tick with r3 = entity
//    (skate3_native_render.cpp:478-488). Remote-pose apply point: the
//    puppet's matrices are overwritten BOTH at entry (so any palette pack
//    inside Update that reads the entity world sees the remote pose when
//    the game does not rewrite the matrix itself) and at exit (so our
//    pose survives when it does). Which of the two actually reaches the
//    draw depends on where the LW pack writer (sub_827C1D38) runs relative
//    to Update — an open question, see the comment in ApplyRemotePose.
//
//  - Gameplay gate: rex::kernel::guest_presence::GameplayContextValue()
//    is 1 in gameplay, 0 in frontend/loading
//    (include/rex/kernel/guest_presence.h).
//
// Wire format (52 bytes, fixed little-endian, versioned):
//   u32 magic 'S3MP' (0x53334D50)
//   u8  version (1)
//   u8  reserved
//   u16 reserved
//   u32 tick        sender-side send counter
//   f32 pos[3]      world position
//   f32 quat[4]     world orientation (x, y, z, w)
//   f32 vel[3]      world velocity (finite difference, sender-side)
//
// Threading: one receive thread (blocking-ish: nonblocking recvfrom +
// 5 ms sleep) pushes decoded poses into a latest-wins two-sample buffer
// under a short mutex; the game thread reads it from the hooks. The send
// path runs entirely on the game thread from OnSkaterTick.

#include "skate3_mp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_mp_enabled, false, "Skate 3",
                    "Enable milestone-1 LAN multiplayer: exchange the local "
                    "skater pose over UDP and render the remote player as a "
                    "puppeted pedestrian (co-op free-skate ghost).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_mp_mode, "host", "Skate 3",
                      "Multiplayer role: host | join. Milestone 1 is "
                      "symmetric (both peers send and receive); the value "
                      "only labels log lines.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(skate3_mp_addr, "127.0.0.1", "Skate 3",
                      "IPv4 address of the other instance (numeric dotted "
                      "quad; its skate3_mp_peer_port / skate3_mp_port).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_mp_port, 26960, "Skate 3",
                     "UDP port this instance binds for multiplayer packets.")
    .range(1, 65535)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_mp_peer_port, 0, "Skate 3",
                     "UDP port packets are sent to on the peer. 0 = same as "
                     "skate3_mp_port. Set distinct port/peer_port pairs to "
                     "run two instances on one machine.")
    .range(0, 65535)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(skate3_mp_send_hz, 20, "Skate 3",
                     "Local-pose send rate in packets per second.")
    .range(5, 60)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(
    skate3_mp_puppet_entity, "0", "Skate 3",
    "Guest address (hex) of the LivingWorld entity to puppet as the remote "
    "player. 0 = auto: the first LivingWorld entity ticked in gameplay.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_mp_debug, false, "Skate 3",
                    "Log multiplayer diagnostics: socket state, puppet "
                    "selection, packet send/receive stats.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::mp {
namespace {

// ---- guest memory helpers (big-endian guest; the idiom from
// skate3_draw_distance.cpp:93-124).

inline uint32_t LoadGuestU32(uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

inline float LoadGuestF32(uint8_t* base, uint32_t addr) {
  const uint32_t raw = LoadGuestU32(base, addr);
  float value;
  std::memcpy(&value, &raw, 4);
  return value;
}

inline void StoreGuestF32(uint8_t* base, uint32_t addr, float value) {
  uint32_t raw;
  std::memcpy(&raw, &value, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(base + addr, &raw, 4);
}

inline bool PlausibleGuestAddr(uint32_t addr) { return addr >= 0x10000; }

// PresentationEntity offsets (skate3_native_entity.cpp:75-78).
constexpr uint32_t kEntL2W = 352;       // m_MatLtoW (Matrix44)
constexpr uint32_t kEntL2WTrans = 416;  // m_MatLtoWTrans (Matrix44)
constexpr uint32_t kEntOpacityLw = 528;  // LivingWorld opacity, x = alpha

// ---- pose math. Matrices use the m_MatLtoWTrans (+416) storage layout:
// m[r*4+c] with the translation in column 3 and a (0,0,0,1) tail row.

struct Pose {
  float pos[3] = {0, 0, 0};
  float quat[4] = {0, 0, 0, 1};  // x, y, z, w
  float vel[3] = {0, 0, 0};
};

void QuatFromMatrix(const float m[16], float out_q[4]) {
  const float t = m[0] + m[5] + m[10];
  float x, y, z, w;
  if (t > 0.0f) {
    const float s = std::sqrt(t + 1.0f) * 2.0f;
    w = 0.25f * s;
    x = (m[9] - m[6]) / s;
    y = (m[2] - m[8]) / s;
    z = (m[4] - m[1]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const float s = std::sqrt(1.0f + m[0] - m[5] - m[10]) * 2.0f;
    w = (m[9] - m[6]) / s;
    x = 0.25f * s;
    y = (m[1] + m[4]) / s;
    z = (m[2] + m[8]) / s;
  } else if (m[5] > m[10]) {
    const float s = std::sqrt(1.0f + m[5] - m[0] - m[10]) * 2.0f;
    w = (m[2] - m[8]) / s;
    x = (m[1] + m[4]) / s;
    y = 0.25f * s;
    z = (m[6] + m[9]) / s;
  } else {
    const float s = std::sqrt(1.0f + m[10] - m[0] - m[5]) * 2.0f;
    w = (m[4] - m[1]) / s;
    x = (m[2] + m[8]) / s;
    y = (m[6] + m[9]) / s;
    z = 0.25f * s;
  }
  const float n = std::sqrt(x * x + y * y + z * z + w * w);
  const float inv = n > 1e-8f ? 1.0f / n : 0.0f;
  out_q[0] = x * inv;
  out_q[1] = y * inv;
  out_q[2] = z * inv;
  out_q[3] = n > 1e-8f ? w * inv : 1.0f;
}

void MatrixFromQuat(const float q[4], float m[16]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  m[0] = 1.0f - 2.0f * (yy + zz);
  m[1] = 2.0f * (xy - wz);
  m[2] = 2.0f * (xz + wy);
  m[4] = 2.0f * (xy + wz);
  m[5] = 1.0f - 2.0f * (xx + zz);
  m[6] = 2.0f * (yz - wx);
  m[8] = 2.0f * (xz - wy);
  m[9] = 2.0f * (yz + wx);
  m[10] = 1.0f - 2.0f * (xx + yy);
  for (int i = 0; i < 4; ++i) {
    m[i * 4 + 3] = 0.0f;
    m[12 + i] = 0.0f;
  }
  m[15] = 1.0f;
}

// Reads the entity's m_MatLtoWTrans (+416): 16 big-endian floats, tail row
// (0,0,0,1), translation in column 3. Same structural checks as
// ReadWorldRowsChecked (skate3_native_entity.cpp:469), so a torn or
// reused entity slot is rejected instead of puppeted.
bool ReadEntityPose(uint8_t* base, uint32_t entity, Pose* out) {
  if (!PlausibleGuestAddr(entity)) {
    return false;
  }
  float m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = LoadGuestF32(base, entity + kEntL2WTrans + uint32_t(i) * 4);
  }
  if (std::fabs(m[12]) > 1e-4f || std::fabs(m[13]) > 1e-4f ||
      std::fabs(m[14]) > 1e-4f || std::fabs(m[15] - 1.0f) > 1e-4f) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    float n = 0.0f;
    for (int c = 0; c < 3; ++c) {
      const float f = m[r * 4 + c];
      if (f < -1e7f || f > 1e7f) {
        return false;
      }
      n += f * f;
    }
    if (n < 0.0025f || n > 400.0f) {
      return false;
    }
  }
  out->pos[0] = m[3];
  out->pos[1] = m[7];
  out->pos[2] = m[11];
  QuatFromMatrix(m, out->quat);
  return true;
}

// Quick structural check that a puppet candidate still holds a Matrix44 at
// +416 (guards against writing into a despawned/reused entity slot).
bool EntityLooksAlive(uint8_t* base, uint32_t entity) {
  if (!PlausibleGuestAddr(entity)) {
    return false;
  }
  const float m12 = LoadGuestF32(base, entity + kEntL2WTrans + 12 * 4);
  const float m15 = LoadGuestF32(base, entity + kEntL2WTrans + 15 * 4);
  return std::fabs(m12) <= 1e-4f && std::fabs(m15 - 1.0f) <= 1e-4f;
}

// Overwrites both world matrices with the pose. +416 gets the upload
// layout (translation column 3); +352 its transpose (translation row 3).
// Also pins the LivingWorld opacity (+528 x) to 1 so the spawn/distance
// fade cannot sink the ghost when the two players are far apart.
void WriteEntityPose(uint8_t* base, uint32_t entity, const Pose& pose,
                     bool pin_opacity) {
  float m[16];
  MatrixFromQuat(pose.quat, m);
  m[3] = pose.pos[0];
  m[7] = pose.pos[1];
  m[11] = pose.pos[2];
  for (int i = 0; i < 16; ++i) {
    StoreGuestF32(base, entity + kEntL2WTrans + uint32_t(i) * 4, m[i]);
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      StoreGuestF32(base, entity + kEntL2W + uint32_t(r * 4 + c) * 4,
                    m[c * 4 + r]);
    }
  }
  if (pin_opacity) {
    StoreGuestF32(base, entity + kEntOpacityLw, 1.0f);
  }
}

// ---- wire format.

constexpr uint32_t kPacketMagic = 0x53334D50;  // 'S3MP'
constexpr uint8_t kPacketVersion = 1;
constexpr size_t kPacketSize = 52;

void PutU32LE(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

uint32_t GetU32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

void PutF32LE(uint8_t* p, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  PutU32LE(p, bits);
}

float GetF32LE(const uint8_t* p) {
  const uint32_t bits = GetU32LE(p);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void EncodePacket(uint8_t out[kPacketSize], uint32_t tick, const Pose& pose) {
  PutU32LE(out + 0, kPacketMagic);
  out[4] = kPacketVersion;
  out[5] = 0;
  out[6] = 0;
  out[7] = 0;
  PutU32LE(out + 8, tick);
  for (int i = 0; i < 3; ++i) {
    PutF32LE(out + 12 + i * 4, pose.pos[i]);
  }
  for (int i = 0; i < 4; ++i) {
    PutF32LE(out + 24 + i * 4, pose.quat[i]);
  }
  for (int i = 0; i < 3; ++i) {
    PutF32LE(out + 40 + i * 4, pose.vel[i]);
  }
}

bool DecodePacket(const uint8_t* p, size_t len, uint32_t* tick, Pose* pose) {
  if (len != kPacketSize || GetU32LE(p) != kPacketMagic ||
      p[4] != kPacketVersion) {
    return false;
  }
  *tick = GetU32LE(p + 8);
  for (int i = 0; i < 3; ++i) {
    pose->pos[i] = GetF32LE(p + 12 + i * 4);
  }
  for (int i = 0; i < 4; ++i) {
    pose->quat[i] = GetF32LE(p + 24 + i * 4);
  }
  for (int i = 0; i < 3; ++i) {
    pose->vel[i] = GetF32LE(p + 40 + i * 4);
  }
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(pose->pos[i]) || !std::isfinite(pose->vel[i]) ||
        std::fabs(pose->pos[i]) > 1e6f) {
      return false;
    }
  }
  float qn = 0.0f;
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(pose->quat[i])) {
      return false;
    }
    qn += pose->quat[i] * pose->quat[i];
  }
  if (qn < 0.5f || qn > 2.0f) {
    return false;
  }
  return true;
}

// ---- remote pose buffer (receive thread -> game thread handoff).

struct RemoteSample {
  Pose pose;
  uint32_t tick = 0;
  std::chrono::steady_clock::time_point received;
};

std::mutex g_remote_mutex;
// Two newest samples, [0] = newest; interpolated on the game thread at
// (now - kRenderDelayMs) so 20 Hz packets render smoothly.
RemoteSample g_remote_samples[2];
uint32_t g_remote_count = 0;

void PushRemotePose(uint32_t tick, const Pose& pose) {
  std::lock_guard<std::mutex> lock(g_remote_mutex);
  g_remote_samples[1] = g_remote_samples[0];
  g_remote_samples[0].pose = pose;
  g_remote_samples[0].tick = tick;
  g_remote_samples[0].received = std::chrono::steady_clock::now();
  if (g_remote_count < 2) {
    ++g_remote_count;
  }
}

constexpr double kRenderDelayMs = 100.0;
constexpr double kRemoteStaleMs = 1000.0;

// Latest-wins with a one-packet interpolation delay. Returns false when no
// fresh remote pose is known.
bool SampleRemotePose(Pose* out) {
  std::lock_guard<std::mutex> lock(g_remote_mutex);
  if (g_remote_count == 0) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  const double newest_age_ms =
      std::chrono::duration<double, std::milli>(now - g_remote_samples[0].received)
          .count();
  if (newest_age_ms > kRemoteStaleMs) {
    return false;
  }
  const RemoteSample& a = g_remote_samples[0];
  if (g_remote_count < 2) {
    *out = a.pose;
    return true;
  }
  const RemoteSample& b = g_remote_samples[1];
  const double span_ms =
      std::chrono::duration<double, std::milli>(a.received - b.received).count();
  if (span_ms <= 1.0) {
    *out = a.pose;
    return true;
  }
  double t = (newest_age_ms - kRenderDelayMs) / span_ms + 1.0;
  t = std::clamp(t, 0.0, 1.0);
  for (int i = 0; i < 3; ++i) {
    out->pos[i] = b.pose.pos[i] + float(t) * (a.pose.pos[i] - b.pose.pos[i]);
    out->vel[i] = a.pose.vel[i];
  }
  // Nlerp with hemisphere fix: packet-rate quats are close enough that
  // slerp's extra cost buys nothing.
  float dot = 0.0f;
  for (int i = 0; i < 4; ++i) {
    dot += a.pose.quat[i] * b.pose.quat[i];
  }
  const float s = dot < 0.0f ? -1.0f : 1.0f;
  float n = 0.0f;
  for (int i = 0; i < 4; ++i) {
    out->quat[i] = b.pose.quat[i] + float(t) * (s * a.pose.quat[i] - b.pose.quat[i]);
    n += out->quat[i] * out->quat[i];
  }
  const float inv = n > 1e-8f ? 1.0f / std::sqrt(n) : 0.0f;
  for (int i = 0; i < 4; ++i) {
    out->quat[i] *= inv;
  }
  if (inv == 0.0f) {
    out->quat[3] = 1.0f;
  }
  return true;
}

// ---- UDP socket (raw BSD / Winsock, nonblocking).

#if defined(_WIN32)
using SocketFd = SOCKET;
constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
#else
using SocketFd = int;
constexpr SocketFd kInvalidSocket = -1;
#endif

std::atomic<bool> g_run{false};
std::thread g_recv_thread;
std::mutex g_socket_mutex;  // guards g_socket + g_bound_port
SocketFd g_socket = kInvalidSocket;
int g_bound_port = 0;

void CloseSocketLocked() {
  if (g_socket != kInvalidSocket) {
#if defined(_WIN32)
    closesocket(g_socket);
#else
    close(g_socket);
#endif
    g_socket = kInvalidSocket;
    g_bound_port = 0;
  }
}

// Opens and binds the receive socket if enabled and not already bound to
// the current skate3_mp_port. Called from both threads under the mutex.
void EnsureSocketLocked() {
  if (!REXCVAR_GET(skate3_mp_enabled)) {
    CloseSocketLocked();
    return;
  }
  const int port = REXCVAR_GET(skate3_mp_port);
  if (g_socket != kInvalidSocket && g_bound_port == port) {
    return;
  }
  CloseSocketLocked();
#if defined(_WIN32)
  static std::once_flag s_wsa;
  std::call_once(s_wsa, [] {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  });
#endif
  const SocketFd fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == kInvalidSocket) {
    return;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
             sizeof(one));
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(uint16_t(port));
  if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    if (REXCVAR_GET(skate3_mp_debug)) {
      REXLOG_WARN("mp: bind to UDP port {} failed", port);
    }
    return;
  }
#if defined(_WIN32)
  u_long nonblock = 1;
  ioctlsocket(fd, FIONBIO, &nonblock);
#else
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
  g_socket = fd;
  g_bound_port = port;
  REXLOG_INFO("mp: listening on UDP port {} (mode={}, peer={}:{})", port,
              REXCVAR_GET(skate3_mp_mode), REXCVAR_GET(skate3_mp_addr),
              REXCVAR_GET(skate3_mp_peer_port) != 0
                  ? REXCVAR_GET(skate3_mp_peer_port)
                  : port);
}

// Sends one encoded packet to the configured peer. Game thread.
void SendPacket(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lock(g_socket_mutex);
  EnsureSocketLocked();
  if (g_socket == kInvalidSocket) {
    return;
  }
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  const std::string addr = REXCVAR_GET(skate3_mp_addr);
  if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    return;
  }
  const int peer_port = REXCVAR_GET(skate3_mp_peer_port);
  sa.sin_port =
      htons(uint16_t(peer_port != 0 ? peer_port : REXCVAR_GET(skate3_mp_port)));
  sendto(g_socket, reinterpret_cast<const char*>(data), int(len), 0,
         reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
}

void RecvThreadMain() {
  uint8_t buf[512];
  uint32_t packet_count = 0;
  while (g_run.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lock(g_socket_mutex);
      EnsureSocketLocked();
      if (g_socket != kInvalidSocket) {
        for (;;) {
          const int n = int(recvfrom(g_socket, reinterpret_cast<char*>(buf),
                                     sizeof(buf), 0, nullptr, nullptr));
          if (n <= 0) {
            break;
          }
          uint32_t tick = 0;
          Pose pose;
          if (DecodePacket(buf, size_t(n), &tick, &pose)) {
            PushRemotePose(tick, pose);
            if (REXCVAR_GET(skate3_mp_debug) &&
                (packet_count++ & 127u) == 0) {
              REXLOG_INFO("mp: received {} packets, latest tick {}", packet_count,
                          tick);
            }
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::lock_guard<std::mutex> lock(g_socket_mutex);
  CloseSocketLocked();
}

// ---- game-thread state.

// Sticky local-skater identity: the first SkaterPresEntity ticked in
// gameplay is the player (NPC skaters only exist inside missions, and the
// player entity is created first). Reset when gameplay is left.
uint32_t g_local_skater = 0;
uint32_t g_send_tick = 0;
Pose g_last_sent_pose;
bool g_last_sent_valid = false;
std::chrono::steady_clock::time_point g_last_send{};

// Sticky puppet identity: cvar override or the first LivingWorld entity
// ticked in gameplay. A puppet that stops ticking (despawned) is dropped
// after kPuppetLostMs and the next ticked entity takes over.
uint32_t g_puppet = 0;
std::chrono::steady_clock::time_point g_puppet_last_tick{};
constexpr double kPuppetLostMs = 2000.0;

uint32_t PuppetOverride() {
  const std::string s = REXCVAR_GET(skate3_mp_puppet_entity);
  if (s.empty() || s == "0") {
    return 0;
  }
  return uint32_t(std::stoul(s, nullptr, 0));
}

bool InGameplay() {
  return rex::kernel::guest_presence::GameplayContextValue() == 1;
}

void ResetSessionState() {
  g_local_skater = 0;
  g_puppet = 0;
  g_last_sent_valid = false;
}

void ApplyRemotePose(uint8_t* base, uint32_t entity, bool pin_opacity) {
  Pose pose;
  if (!SampleRemotePose(&pose)) {
    return;
  }
  if (!EntityLooksAlive(base, entity)) {
    return;
  }
  WriteEntityPose(base, entity, pose, pin_opacity);
}

}  // namespace

void Start() {
  if (!REXCVAR_GET(skate3_mp_enabled)) {
    return;
  }
  bool expected = false;
  if (!g_run.compare_exchange_strong(expected, true)) {
    return;
  }
  g_recv_thread = std::thread(RecvThreadMain);
}

void Stop() {
  g_run.store(false, std::memory_order_release);
  if (g_recv_thread.joinable()) {
    g_recv_thread.join();
  }
}

void OnSkaterTick(uint8_t* base, uint32_t entity) {
  if (!REXCVAR_GET(skate3_mp_enabled)) {
    return;
  }
  if (!InGameplay()) {
    if (g_local_skater != 0 || g_puppet != 0) {
      ResetSessionState();
    }
    return;
  }
  if (g_local_skater == 0) {
    g_local_skater = entity;
    if (REXCVAR_GET(skate3_mp_debug)) {
      REXLOG_INFO("mp: local skater entity = 0x{:08X}", entity);
    }
  }
  if (entity != g_local_skater) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const int hz = std::max(1, REXCVAR_GET(skate3_mp_send_hz));
  const double since_ms =
      std::chrono::duration<double, std::milli>(now - g_last_send).count();
  if (since_ms < 1000.0 / double(hz)) {
    return;
  }

  Pose pose;
  if (!ReadEntityPose(base, entity, &pose)) {
    return;
  }
  // Sender-side finite-difference velocity over the send interval.
  if (g_last_sent_valid && since_ms > 1.0) {
    const float dt = float(since_ms / 1000.0);
    for (int i = 0; i < 3; ++i) {
      pose.vel[i] = (pose.pos[i] - g_last_sent_pose.pos[i]) / dt;
    }
  }
  g_last_send = now;
  g_last_sent_pose = pose;
  g_last_sent_valid = true;

  uint8_t packet[kPacketSize];
  EncodePacket(packet, g_send_tick++, pose);
  SendPacket(packet, sizeof(packet));

  // Extra puppet write from the skater tick as well: the LW Update order
  // relative to the skater jobs within a frame is not mapped, and the last
  // writer before the pack wins. Cheap (one sample + two matrix stores).
  if (g_puppet != 0) {
    ApplyRemotePose(base, g_puppet, /*pin_opacity=*/false);
  }
}

void OnLwEntityUpdatePre(uint8_t* base, uint32_t entity) {
  if (!REXCVAR_GET(skate3_mp_enabled) || !InGameplay()) {
    return;
  }
  if (g_puppet == 0) {
    const uint32_t override_addr = PuppetOverride();
    g_puppet = override_addr != 0 ? override_addr : entity;
    g_puppet_last_tick = std::chrono::steady_clock::now();
    if (REXCVAR_GET(skate3_mp_debug)) {
      REXLOG_INFO("mp: puppet entity = 0x{:08X}{}", g_puppet,
                  override_addr != 0 ? " (cvar)" : " (auto)");
    }
  }
  if (entity != g_puppet) {
    return;
  }
  g_puppet_last_tick = std::chrono::steady_clock::now();
  // Pre-write: if the entity's own Update leaves a stationary ped's matrix
  // untouched, any palette pack inside Update reads the remote pose.
  ApplyRemotePose(base, entity, /*pin_opacity=*/false);
}

void OnLwEntityUpdatePost(uint8_t* base, uint32_t entity) {
  if (!REXCVAR_GET(skate3_mp_enabled) || !InGameplay()) {
    return;
  }
  if (g_puppet == 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (entity != g_puppet) {
    // Drop a puppet that has stopped ticking (despawned / arena reuse) so
    // the next ticked entity takes over.
    if (std::chrono::duration<double, std::milli>(now - g_puppet_last_tick)
            .count() > kPuppetLostMs) {
      if (REXCVAR_GET(skate3_mp_debug)) {
        REXLOG_INFO("mp: puppet 0x{:08X} lost, reselecting", g_puppet);
      }
      g_puppet = 0;
    }
    return;
  }
  // Post-write: wins when Update itself rewrote the matrix (walking peds).
  // Pin opacity after the game's own fade evaluation so the ghost stays
  // visible at distance.
  ApplyRemotePose(base, entity, /*pin_opacity=*/true);
}

}  // namespace skate3::mp
