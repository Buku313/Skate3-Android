#pragma once

// Presentation-entity identity store:
// draw-context -> owning entity for EVERY Sk8::PresentationEntity subclass,
// built from the game's own constant-binding walk.
//
// Write point: Sk8::PresentationEntity::BindConstants (sub_827A6658) EXIT.
// The game binds shader constants BY POINTER, once, near entity
// construction: the base bind walks LOD -> cModelInstance (stride 0x28) ->
// MeshContext (stride 0x50) and pushes {hash, pointer, count} records into
// each mesh's ShaderSystem param scope; cScope::DispatchParams dereferences
// them at draw time to fill the constant registers. Every subclass bind
// (skater / colorized / CAC / LivingWorld / props) calls the base, so one
// exit hook sees every renderable entity's full ctx set with the entity in
// r3: the identity link the draw-time constant bank cannot provide.
//
// Verified bindings (generated code, instruction level):
//  - base pushes hash 0x7DDF552B -> &entity+416 (m_MatLtoWTrans, 4 vec4s)
//    = the ROPA/rigid world block staged at c191..c194 (main pass) /
//    c188..c191 (shadow pass).
//  - SkaterPresEntity::BindConstants (sub_82783D68) additionally pushes
//    hash 0xD5DF94E7 -> &entity+496 (opacity vector, x = alpha): the same
//    hash the LivingWorld classes bind at entity+528.
//  - CACPresEntity::BindConstants (sub_82793F70) RE-pushes 0x7DDF552B ->
//    &entity+352 (m_MatLtoW, UNtransposed); CAC draws stage a transposed
//    block relative to every other class.
//
// This module is currently an OBSERVER: it builds the store and compares
// the entity's direct fields against the values the capture heuristics
// decided on, without serving anything. Serving flips come one at a time
// once the live telemetry proves each field (ident[] stats line).

#include <cstdint>

namespace skate3::native_entity {

// Which BindConstants override reached the base walk, most-derived wins
// (the derived binds run after the base within one virtual call).
enum class EntClass : uint8_t {
  kUnknown = 0,   // base bind only (world props, misc presentation)
  kLivingWorld,   // sub_827C1720 (already served by the LW entity store)
  kSkater,        // sub_82783D68 (Sk8::SkaterPresEntity)
  kColorized,     // sub_82785260 (ColorizedSkaterPresEntity)
  kCac,           // sub_82793F70 (CACPresEntity, untransposed world rebind)
  kSkaterAux,     // sub_82785528 (unnamed class with the skater layout)
};

// sub_827A6658 EXIT (guest thread): register every MeshContext the walk
// just bound against the owning entity. Idempotent; re-registration on
// LOD/model changes refreshes the mapping.
void OnBindConstants(uint8_t* base, uint32_t entity);

// Derived-bind EXIT: tag the entity's class (most-derived override fires
// last, so plain assignment converges on the correct class).
void OnBindClass(uint32_t entity, EntClass cls);

// RenderPresentation::AddEntityToRenderViews / RmvEntityFrmRenderViews
// (sub_827A6C50 / sub_827A6CE8, r4 = entity): per-entity view refcount,
// the direct scene-membership/lifetime signal.
void OnEntityViewAdd(uint32_t entity);
void OnEntityViewRemove(uint32_t entity);

// SkaterPresEntity::DoubleBuffer EXIT (sub_82783038, r3 = skater,
// r4 = garment index): runs exactly once per garment per completed cloth
// sim tick: the authoritative "the sim wrote this VB this tick" signal.
// Stamps both double-buffer halves fresh. The garment-table fields
// (m_pModelClothTarget etc.) PERSIST after the game stops running the
// cloth job for an entity, so field checks alone misclassify a sleeping
// garment as CPU-simulated and draw its last (stale, mid-pose) deformed
// VB rigidly, the floating/perpendicular shirt.
void OnRopaDoubleBuffer(uint8_t* base, uint32_t skater, uint32_t index);

struct CtxInfo {
  uint32_t entity = 0;
  uint32_t instance = 0;  // owning cModelInstance (m_matrices at +0x14)
  EntClass cls = EntClass::kUnknown;
  bool view_live = false;  // Add/Rmv refcount > 0
};
bool LookupCtx(uint32_t ctx, CtxInfo* out);

// Observer probes (render thread, called from the scene builder).
//
// Accepted ROPA rigid world vs the entity's own matrix fields: rows =
// the 3 accepted bank rows (c-register layout, translation in component
// 3), compared against entity+416 and entity+352.
void ObserveRopaWorld(uint8_t* base, uint32_t ctx, const float rows[12]);

// Per published character item: identity coverage (hit/miss/LW overlap)
// and, for skater-family entities, entity+496 opacity vs the alpha the
// renderer decided to use.
void ObserveCharItem(uint8_t* base, uint32_t ctx, uint32_t family,
                     bool lw_mapped, float used_alpha);

// Serve flip 1: the owner entity's world matrix for a ROPA garment whose
// draw-time bank read REFUSED (foreign bank, post-shadow tail clobber,
// torn state - the pending-forever/invisible classes). The observer run
// proved the accepted bank block is a bit-exact dereference of
// entity+416 (m_MatLtoWTrans), so the entity field serves the same
// matrix through every bank state.
//
// MODE AUTHORITY: serving a rigid world implicitly asserts the garment is
// CPU-deformed, and on a refused bank the flag row proves nothing, so
// the serve additionally requires the owning entity's own garment table
// to claim this VB as a live cloth target (m_bSkinningEnabled &&
// m_pModelClothTarget[i] != 0 && vb_obj is one of that garment's
// double-buffer halves). Without that proof the caller keeps its refusal
// path (pending + post-draw fixup, which re-reads the authentic flag
// from the mesh's own draw and can resolve the item SKINNED).
//
// Fills rows in the staged c-register layout (3 rows, translation in
// component 3) after structural checks (homogeneous (0,0,0,1) tail row +
// row-norm sanity).
bool ServeRopaWorld(uint8_t* base, uint32_t ctx, uint32_t vb_obj,
                    float out_rows[12]);

// Fade serve (skater-family classes only; LivingWorld entities are
// served by the LW store): the entity's live opacity at +496, the exact
// value BindConstants points the shader's alpha parameter at. Returns
// false for unmapped ctxs, non-skater classes, or an out-of-range read
// (teardown), leaving the caller's captured-row fade untouched.
bool ReadSkaterFade(uint8_t* base, uint32_t ctx, float* out_alpha);

// True when the ctx maps to an entity whose garment table no longer
// claims vb_obj as a live cloth target; the game DROPPED the garment
// (m_numClothModels back to 0, VB objects freed, memory recycled). The
// entity itself stays alive and its ctxs keep being submitted (drawn
// skinned by the game's own sim-inactive path), so any rigid ropa state
// our caches still hold for the vb is a ghost: a retained mid-sim drape
// paired with a live world tracks the character as a floating garment.
// Unmapped ctx / unreadable fields return false (never suppress on
// uncertainty).
bool RopaGarmentDropped(uint8_t* base, uint32_t ctx, uint32_t vb_obj);

// The mapped instance's live packed palette (m_matrices, 12 floats/row),
// for resolving a dropped-garment's skinned draw when bank capture fails
// chronically. Returns the row count copied (0 = unmapped/unreadable).
uint32_t ServeInstancePalette(uint8_t* base, uint32_t ctx, float* out,
                              uint32_t max_rows);

// Quiet variant for per-render-frame checks (no counters, no logs, no
// garment-table gate): the mapped owner entity's live world block in the
// staged c-register layout, with the same structural sanity checks.
// Used by the post-interpolation identity anchor; the ring lerp can only
// be validated against the entity, since both the published scene and the
// guest draws already carried the correct matrix.
bool ReadEntityWorldRows(uint8_t* base, uint32_t ctx, float out_rows[12]);

// Tripwire for the primary world serve: on ACCEPTED bank captures the
// entity rows are proven bit-exact equal to the bank block; count any
// divergence (a nonzero count means the mapping or read point drifted,
// and shows up in the ident[] line as wprim=<serves>/<divergences>).
void NoteAcceptedWorldCompare(const float bank_rows[12],
                              const float ent_rows[12]);

// Rate-limited ident[] stats line; call once per built frame.
void EmitStats();

}  // namespace skate3::native_entity
