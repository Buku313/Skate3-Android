#pragma once

// Milestone-1 LAN multiplayer: two instances of the game each render the
// other player's skater as a puppeted LivingWorld pedestrian (a co-op
// free-skate "ghost"). See skate3_mp.cpp for the design notes and the
// guest facts every offset below rests on.

#include <cstdint>

namespace skate3::mp {

// App lifecycle (Skate3BaseApp::OnPostSetup / OnShutdown): spawn/join the
// UDP receive thread. The socket itself opens lazily while
// skate3_mp_enabled is set, so the cvar can be toggled without a restart.
void Start();
void Stop();

// sub_82782818 (Sk8::SkaterPresEntity::EndJobs) EXIT, per skater per sim
// tick: the entity's m_MatLtoWTrans (+416) has just been packed with the
// tick's final locomotion, so this is the local-pose read point and the
// network pump heartbeat (send at skate3_mp_send_hz).
void OnSkaterTick(uint8_t* base, uint32_t entity);

// sub_827C1188 (Sk8::cLivingWorldPresEntity::Update) ENTRY and EXIT, per
// LivingWorld entity per sim tick: the remote-pose apply point. The chosen
// puppet entity's world matrices are overwritten with the remote pose on
// both sides of the game's own update (see skate3_mp.cpp for why both).
void OnLwEntityUpdatePre(uint8_t* base, uint32_t entity);
void OnLwEntityUpdatePost(uint8_t* base, uint32_t entity);

}  // namespace skate3::mp
