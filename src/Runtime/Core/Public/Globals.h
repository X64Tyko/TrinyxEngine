#pragma once

#include <cstdint>

/** @addtogroup core
 *  @{
 */

/// @brief Simulation units per meter. 1 unit = 0.1 mm; int32 range covers ±214 km.
/// @note Used by Fixed32, the Jolt bridge, render upload, and scene I/O.
inline constexpr int32_t FixedUnitsPerMeter = 10000;

/// @brief Simulation units per unit-range value (2^28). Used by FixedUnit for quaternion
/// components and trig output. Range ±8 gives headroom above [-1,1] for unnormalized intermediates.
inline constexpr int32_t FixedUnitsPerRadian = 268435456;

// ---------------------------------------------------------------------------
// Compile-time world type aliases resolved from three orthogonal CMake axes:
//   Net      — TNX_NET_MODEL_SERVER / TNX_NET_MODEL_CLIENT / (neither) → SoloSim
//   Rollback — TNX_ENABLE_ROLLBACK → RollbackSim, otherwise NoRollback
//   Frame    — GameFrame (sole option for now)
//
// Entity counts, tick rates, and addresses stay in EngineConfig (runtime) so
// they can be tuned without a rebuild.
//
// TNX_ENABLE_EDITOR adds PIE aliases so EditorContext.cpp can spell world types
// without repeating template arguments. World.cpp / LogicThread.cpp carry the
// matching explicit instantiations.
// ---------------------------------------------------------------------------

struct SoloSim;      ///< Offline, no networking.
struct AuthoritySim; ///< Dedicated server or Host authority side.
struct OwnerSim;     ///< Owner (local player / client) side.
struct NoRollback;   ///< Rollback history disabled; Temporal tier treated as Volatile.
struct RollbackSim;  ///< N-frame rollback history enabled.
struct GameFrame;    ///< Standard gameplay frame policy.

// --- Net axis ---------------------------------------------------------------
#if defined(TNX_NET_MODEL_SERVER)
using DefaultNet = AuthoritySim; ///< Authority sim selected by TNX_NET_MODEL_SERVER.
#elif defined(TNX_NET_MODEL_CLIENT)
using DefaultNet = OwnerSim; ///< Owner sim selected by TNX_NET_MODEL_CLIENT.
#else
using DefaultNet = SoloSim; ///< Offline sim — default when no net model is set.
#endif

// --- Rollback axis (orthogonal to net model) --------------------------------
#ifdef TNX_ENABLE_ROLLBACK
using DefaultRollback = RollbackSim; ///< Rollback enabled via TNX_ENABLE_ROLLBACK.
#else
using DefaultRollback = NoRollback; ///< Rollback disabled — default.
#endif

using DefaultFrame = GameFrame; ///< Frame policy for this build.

// --- Standalone / server / client world type --------------------------------
template <typename, typename, typename>
class World;
using WorldType = World<DefaultNet, DefaultRollback, DefaultFrame>; ///< Canonical world type for this build configuration.

template <typename, typename, typename>
class FlowManager;
using FlowManagerType = FlowManager<DefaultNet, DefaultRollback, DefaultFrame>; ///< Canonical FlowManager type for this build configuration.

// --- PIE world types (editor builds only) -----------------------------------
// PIEServerWorld always uses RollbackSim — server-side reconciliation requires it.
//   TNX_ENABLE_EDITOR forces TNX_ENABLE_ROLLBACK=ON, so RollbackSim always exists here.
// PIEClientWorld follows DefaultRollback (the build's rollback setting).
#ifdef TNX_ENABLE_EDITOR
using PIEServerNet      = AuthoritySim;    ///< PIE Authority net policy.
using PIEServerRollback = RollbackSim;     ///< PIE server always runs rollback for reconciliation.
using PIEClientNet      = OwnerSim;        ///< PIE Owner net policy.
using PIEClientRollback = DefaultRollback; ///< PIE client follows the build's rollback setting.

using PIEServerWorld = World<PIEServerNet, PIEServerRollback, DefaultFrame>;       ///< PIE Authority world type.
using PIEClientWorld = World<PIEClientNet, PIEClientRollback, DefaultFrame>;       ///< PIE Owner world type.
using PIELocalWorld  = World<SoloSim, RollbackSim, DefaultFrame>;                  ///< PIE local/solo world type.
using PIEServerFlow  = FlowManager<PIEServerNet, PIEServerRollback, DefaultFrame>; ///< PIE Authority FlowManager type.
using PIEClientFlow  = FlowManager<PIEClientNet, PIEClientRollback, DefaultFrame>; ///< PIE Owner FlowManager type.
using PIELocalFlow   = FlowManager<SoloSim, RollbackSim, DefaultFrame>;            ///< PIE local/solo FlowManager type.
#endif

/** @} */
