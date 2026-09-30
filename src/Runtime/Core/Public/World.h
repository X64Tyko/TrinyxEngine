#pragma once
#include "WorldBase.h"
#include "LogicThread.h"
#include "OwnerSim.h"

/** @addtogroup core
 *  @{
 */

/**
 * @brief Typed simulation container that binds a concrete LogicThread policy triple.
 *
 * @tparam TNet      Net policy — SoloSim, AuthoritySim, or OwnerSim.
 * @tparam TRollback Rollback policy — NoRollback or RollbackSim.
 * @tparam TFrame    Frame policy — GameFrame.
 *
 * Knowing the exact LogicThread type at compile time eliminates static_cast at
 * wire-up sites and all `#if NET_MODEL_*` guards inside Initialize.
 *
 * In standalone/Authority/Owner builds use @ref WorldType from Globals.h.
 * In PIE, EditorContext constructs @ref PIEServerWorld and @ref PIEClientWorld directly.
 *
 * @note Explicit instantiations live in World.cpp — do not define template members inline
 *       here. Keeping them out-of-line ensures each LogicThread<> vtable has exactly one
 *       home in LogicThread.cpp.
 */
template <typename TNet, typename TRollback, typename TFrame>
class World : public WorldBase
{
public:
	using LogicType = LogicThread<TNet, TRollback, TFrame>; ///< Fully-typed logic thread for this world.

	// Defined in World.cpp alongside explicit instantiations — NOT inline.
	bool Initialize(const EngineConfig& config, ConstructRegistry* constructRegistry,
		int windowWidth = 1920, int windowHeight = 1080);

	/// @brief Returns the typed logic thread pointer (non-owning; WorldBase::Logic owns it).
	LogicType* GetTypedLogicThread() const { return TypedLogic; }

	/// @brief Forwards transform corrections to the rollback subsystem; no-op when TRollback::Enabled is false.
	void EnqueueCorrections(std::vector<EntityTransformCorrection> corrections,
		uint32_t earliestClientFrame) override
	{
		if constexpr (TRollback::Enabled) TypedLogic->Rollback.EnqueueCorrections(std::move(corrections), earliestClientFrame);
	}

	/// @brief Forwards predicted corrections to the rollback subsystem; no-op when TRollback::Enabled is false.
	void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections) override
	{
		if constexpr (TRollback::Enabled) TypedLogic->Rollback.EnqueuePredictedCorrections(std::move(corrections));
	}

	/// @brief Enqueues a spawn-triggered rollback to the given client frame; no-op when rollback is disabled.
	void EnqueueSpawnRollback(uint32_t clientFrame) override
	{
		if constexpr (TRollback::Enabled) TypedLogic->Rollback.EnqueueSpawnRollback(*TypedLogic, clientFrame);
	}

	/// @brief Binds the AuthorityNet handler; no-op on non-Authority builds.
	void BindAuthorityNet(AuthorityNet* net, NetConnectionManager* connMgr) override
	{
		if constexpr (std::is_same_v<TNet, AuthoritySim>) TypedLogic->GetNetMode().Bind(*this, connMgr);
	}

private:
	LogicType* TypedLogic = nullptr; ///< Non-owning alias into WorldBase::Logic.
};

// Explicit instantiations live in World.cpp. Suppress implicit instantiation in all
// other TUs so the World<> vtable has exactly one home. AuthoritySim/OwnerSim variants
// depend on Net/Private symbols and only exist when networking is enabled.
extern template class World<SoloSim, NoRollback, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class World<AuthoritySim, NoRollback, GameFrame>;
extern template class World<OwnerSim, NoRollback, GameFrame>;
#endif
#ifdef TNX_ENABLE_ROLLBACK
extern template class World<SoloSim, RollbackSim, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class World<AuthoritySim, RollbackSim, GameFrame>;
extern template class World<OwnerSim, RollbackSim, GameFrame>;
#endif
#endif

/** @} */
