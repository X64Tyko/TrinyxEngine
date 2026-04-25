#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// World units. 1 unit = 0.1mm. 10,000 units = 1m. int32 range = ±214km.
// Used by Fixed32 (deterministic sim) and any code converting between sim
// space and meters (Jolt bridge, render upload, scene I/O).
// ---------------------------------------------------------------------------
inline constexpr int32_t FixedUnitsPerMeter = 10000;

// ---------------------------------------------------------------------------
// Globals.h — compile-time world type aliases.
//
// Entity counts, tick rates, and network addresses remain runtime fields on
// EngineConfig so projects can tune them without a rebuild.
//
// Three axes, fully independent:
//   Net      — Solo / Authority / Owner  (TNX_NET_MODEL_* CMake define)
//   Rollback — NoRollback / RollbackSim  (TNX_ENABLE_ROLLBACK CMake define)
//   Frame    — GameFrame                 (always, for now)
//
// When TNX_ENABLE_EDITOR is defined, additional PIE aliases are declared so
// EditorContext.cpp can name its world types without spelling out template
// arguments. The instantiation lists in World.cpp and LogicThread.cpp add the
// matching explicit instantiations.
// ---------------------------------------------------------------------------

struct SoloSim;
struct AuthoritySim;
struct OwnerSim;
struct NoRollback;
struct RollbackSim;
struct GameFrame;

// --- Net axis ---------------------------------------------------------------
#if defined(TNX_NET_MODEL_SERVER)
using DefaultNet = AuthoritySim;
#elif defined(TNX_NET_MODEL_CLIENT)
using DefaultNet = OwnerSim;
#else
using DefaultNet = SoloSim;
#endif

// --- Rollback axis (orthogonal to net model) --------------------------------
#ifdef TNX_ENABLE_ROLLBACK
using DefaultRollback = RollbackSim;
#else
using DefaultRollback = NoRollback;
#endif

using DefaultFrame = GameFrame;

// --- Standalone / server / client world type --------------------------------
template <typename, typename, typename>
class World;
using WorldType = World<DefaultNet, DefaultRollback, DefaultFrame>;

template <typename, typename, typename>
class FlowManager;
using FlowManagerType = FlowManager<DefaultNet, DefaultRollback, DefaultFrame>;

// --- PIE world types (editor builds only) -----------------------------------
// PIEServerWorld: Authority sim + rollback (server-side reconciliation requires it).
//   TNX_ENABLE_EDITOR forces TNX_ENABLE_ROLLBACK=ON so RollbackSim always exists here.
// PIEClientWorld: Owner sim, follows the build's rollback setting (DefaultRollback).
#ifdef TNX_ENABLE_EDITOR
using PIEServerNet      = AuthoritySim;
using PIEServerRollback = RollbackSim;
using PIEClientNet      = OwnerSim;
using PIEClientRollback = DefaultRollback;

using PIEServerWorld = World<PIEServerNet, PIEServerRollback, DefaultFrame>;
using PIEClientWorld = World<PIEClientNet, PIEClientRollback, DefaultFrame>;
using PIEServerFlow  = FlowManager<PIEServerNet, PIEServerRollback, DefaultFrame>;
using PIEClientFlow  = FlowManager<PIEClientNet, PIEClientRollback, DefaultFrame>;
#endif
