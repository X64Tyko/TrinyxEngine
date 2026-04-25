#pragma once
#include "WorldBase.h"
#include "LogicThread.h"
#include "OwnerSim.h"

// ---------------------------------------------------------------------------
// World<TNet, TRollback, TFrame>
//
// Typed simulation container. Knows exactly which LogicThread it runs,
// eliminating all static_cast at wire-up sites and all #if NET_MODEL_*
// blocks in Initialize.
//
// In standalone/server/client builds, use WorldType (alias from Globals.h).
// In PIE, EditorContext instantiates each world with the appropriate type.
// ---------------------------------------------------------------------------
template <typename TNet, typename TRollback, typename TFrame>
class World : public WorldBase
{
public:
using LogicType = LogicThread<TNet, TRollback, TFrame>;

// Declared here; defined in World.cpp alongside explicit instantiations.
// NOT inline — ensures the vtable for LogicThread<> is only emitted by
// LogicThread.cpp, not by every TU that includes World.h.
bool Initialize(const EngineConfig& config, ConstructRegistry* constructRegistry,
                int windowWidth = 1920, int windowHeight = 1080);

LogicType* GetTypedLogicThread() const { return TypedLogic; }

void EnqueueCorrections(std::vector<EntityTransformCorrection> corrections,
						uint32_t earliestClientFrame) override
{
	if constexpr (TRollback::Enabled) TypedLogic->Rollback.EnqueueCorrections(std::move(corrections), earliestClientFrame);
}

void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections) override
{
	if constexpr (TRollback::Enabled) TypedLogic->Rollback.EnqueuePredictedCorrections(std::move(corrections));
}

void EnqueueSpawnRollback(uint32_t clientFrame) override
{
	if constexpr (TRollback::Enabled) TypedLogic->Rollback.EnqueueSpawnRollback(*TypedLogic, clientFrame);
}

void BindAuthorityNet(AuthorityNet* net, NetConnectionManager* connMgr) override
{
	if constexpr (std::is_same_v<TNet, AuthoritySim>) TypedLogic->GetNetMode().Bind(*this, connMgr);
}

private:
LogicType* TypedLogic = nullptr; // non-owning alias; WorldBase::Logic owns
};

// Explicit instantiations live in World.cpp. Suppress implicit instantiation
// in all other TUs so the World<> vtable has exactly one home. AuthoritySim/OwnerSim
// variants depend on Net/Private symbols and only exist when networking is enabled.
extern template class World<SoloSim,      NoRollback,  GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class World<AuthoritySim, NoRollback,  GameFrame>;
extern template class World<OwnerSim,     NoRollback,  GameFrame>;
#endif
#ifdef TNX_ENABLE_ROLLBACK
extern template class World<SoloSim,      RollbackSim, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class World<AuthoritySim, RollbackSim, GameFrame>;
extern template class World<OwnerSim,     RollbackSim, GameFrame>;
#endif
#endif
