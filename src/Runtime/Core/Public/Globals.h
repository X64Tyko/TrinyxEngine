#pragma once

// ---------------------------------------------------------------------------
// Globals.h — compile-time world type aliases.
//
// Entity counts, tick rates, and network addresses remain runtime fields on
// EngineConfig so projects can tune them without a rebuild.
//
// What lives here: the WorldType alias that wires together the three policy
// axes at standalone build time. PIE (EditorContext) always instantiates each
// world with an explicit type and does not use WorldType.
//
// Axes:
//   DefaultNet      — driven by TNX_NET_MODEL_* CMake define
//   DefaultRollback — NoRollback by default; RollbackSim when TNX_ENABLE_ROLLBACK=ON and Owner role
//   DefaultFrame    — GameFrame for all standalone/server/client builds
//
// Include order note: forward-declare policies only. Full types come from
// the policy headers included by LogicThread.h and World.h.
// ---------------------------------------------------------------------------

// Forward-declared so callers can name the alias without pulling in full
// policy headers. Use WorldType for instantiation sites only.
struct SoloSim;
struct AuthoritySim;
struct OwnerSim;
struct NoRollback;
struct RollbackSim;
struct GameFrame;

#if defined(TNX_NET_MODEL_SERVER)
    using DefaultNet      = AuthoritySim;
    using DefaultRollback = NoRollback;
#elif defined(TNX_NET_MODEL_CLIENT)
    using DefaultNet      = OwnerSim;
    #ifdef TNX_ENABLE_ROLLBACK
        using DefaultRollback = RollbackSim;
    #else
        using DefaultRollback = NoRollback;
    #endif
#else
    // Solo (no TNX_ENABLE_NETWORK) or PIE host process — world type selection
    // is explicit in EditorContext; this alias is used only by TrinyxEngine.
    using DefaultNet      = SoloSim;
    using DefaultRollback = NoRollback;
#endif

using DefaultFrame = GameFrame;

// WorldType — the concrete World specialization for this build target.
// Defined after World.h is included (World.h includes Globals.h for the aliases).
// Usage:  std::make_unique<WorldType>()
// Callers that need only a base pointer hold WorldBase*.
template <typename, typename, typename> class World;
using WorldType = World<DefaultNet, DefaultRollback, DefaultFrame>;
