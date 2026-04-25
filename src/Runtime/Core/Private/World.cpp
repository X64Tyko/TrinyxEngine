#include "WorldBase.h"

#include "Logger.h"
#include "LogicThreadBase.h"
#include "Registry.h"
#include "JoltPhysics.h"

WorldBase::WorldBase() = default;

bool WorldBase::IsLogicRunning() const { return Logic && Logic->IsRunning(); }

WorldBase::~WorldBase()
{
	if (Logic && Logic->IsRunning())
	{
Stop();
Join();
}
}

bool WorldBase::InitBase(const EngineConfig& config, ConstructRegistry* constructRegistry,
                         int windowWidth, int windowHeight)
{
(void)windowWidth;
(void)windowHeight;

Config = config;
Constructs = constructRegistry;

// --- Registry ---
RegistryPtr = std::make_unique<Registry>(&Config);

// --- Physics ---
Physics = std::make_unique<JoltPhysics>();
if (!Physics->Initialize(&Config))
{
LOG_ENG_ERROR("[World] JoltPhysics::Initialize failed");
return false;
}
RegistryPtr->SetPhysics(Physics.get());

// --- World queue ---
WQHandle = TrinyxJobs::CreateWorldQueue();
if (WQHandle == TrinyxJobs::InvalidWorldQueue)
{
LOG_ENG_ERROR("[World] Failed to create WorldQueue");
return false;
}

return true;
}

void WorldBase::Start()
{
if (Logic) Logic->Start();
}

void WorldBase::Stop()
{
if (Logic) Logic->Stop();
}

void WorldBase::Join()
{
if (Logic) Logic->Join();
}

void WorldBase::Shutdown()
{
Stop();
Join();

// Note: ConstructRegistry is owned by FlowManager and outlives the World.
// World-lifetime and Level-lifetime Constructs are destroyed by FlowManager
// during transitions, not here.

Logic.reset();
Physics.reset();
RegistryPtr.reset();

if (WQHandle != TrinyxJobs::InvalidWorldQueue)
{
TrinyxJobs::DestroyWorldQueue(WQHandle);
WQHandle = TrinyxJobs::InvalidWorldQueue;
}

Constructs = nullptr;
LOG_ENG_INFO("[World] Shut down");
}

void WorldBase::ResetRegistry() const
{
if (RegistryPtr) RegistryPtr->ResetRegistry();
}

void WorldBase::ConfirmLocalRecycles() const
{
if (RegistryPtr) RegistryPtr->ConfirmLocalRecycles();
}

// ---------------------------------------------------------------------------
// World<TNet, TRollback, TFrame>::Initialize
//
// Lives here (not inline in World.h) so that constructing a LogicThread<>
// only happens in this TU. LogicThread<> has extern template declarations in
// LogicThread.h, so World.cpp.o emits references to the LogicThread<> vtable
// (not definitions). The definitions come from LogicThread.cpp.o.
// ---------------------------------------------------------------------------
#include "World.h"

template <typename TNet, typename TRollback, typename TFrame>
bool World<TNet, TRollback, TFrame>::Initialize(
	const EngineConfig& config, ConstructRegistry* constructRegistry,
	int windowWidth, int windowHeight)
{
	if (!InitBase(config, constructRegistry, windowWidth, windowHeight)) return false;

	auto typedLogic = std::make_unique<LogicType>();
	TypedLogic      = typedLogic.get();
	Logic           = std::move(typedLogic);

	Logic->Initialize(RegistryPtr.get(), &Config, Physics.get(),
					  &SimInput, &VizInput,
					  WQHandle, &bJobsInitialized,
					  windowWidth, windowHeight);
	Logic->SetConstructRegistry(Constructs);

	// Initialize the audio command ring (any thread → Sentinel drain).
	if (!AudioCmdRing.Initialize(64))
	{
		LOG_ENG_ERROR("[World] Failed to initialize AudioCmdRing");
		return false;
	}
	AudioCmdConsumer.emplace(AudioCmdRing.MakeConsumer());

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

template class World<SoloSim, NoRollback, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
template class World<AuthoritySim, NoRollback, GameFrame>;
template class World<OwnerSim, NoRollback, GameFrame>;
#endif
#ifdef TNX_ENABLE_ROLLBACK
template class World<SoloSim, RollbackSim, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
template class World<AuthoritySim, RollbackSim, GameFrame>;
template class World<OwnerSim, RollbackSim, GameFrame>;
#endif
#endif
