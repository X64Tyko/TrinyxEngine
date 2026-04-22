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

bool Initialize(const EngineConfig& config, ConstructRegistry* constructRegistry,
                int windowWidth = 1920, int windowHeight = 1080);

LogicType* GetTypedLogicThread() const { return TypedLogic; }

private:
LogicType* TypedLogic = nullptr; // non-owning alias; WorldBase::Logic owns
};

// ---------------------------------------------------------------------------
// Template method bodies
// ---------------------------------------------------------------------------

template <typename TNet, typename TRollback, typename TFrame>
bool World<TNet, TRollback, TFrame>::Initialize(
    const EngineConfig& config, ConstructRegistry* constructRegistry,
    int windowWidth, int windowHeight)
{
if (!InitBase(config, constructRegistry, windowWidth, windowHeight))
return false;

auto typedLogic = std::make_unique<LogicType>();
TypedLogic = typedLogic.get();
Logic      = std::move(typedLogic);

Logic->Initialize(RegistryPtr.get(), &Config, Physics.get(),
                  &SimInput, &VizInput,
                  WQHandle, &bJobsInitialized,
                  windowWidth, windowHeight);
Logic->SetConstructRegistry(Constructs);

if constexpr (std::is_same_v<TNet, OwnerSim>)
{
if (!InputAccumRing.Initialize(256))
{
LOG_ENG_ERROR("[World] Failed to initialize InputAccumRing");
return false;
}
InputAccumConsumer.emplace(InputAccumRing.MakeConsumer());
TypedLogic->GetNetMode().Initialize(&InputAccumRing, &bInputAccumEnabled, &SimInput);
}

LOG_ENG_INFO("[World] Initialized");
return true;
}
