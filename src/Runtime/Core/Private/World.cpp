#include "WorldBase.h"

#include "Logger.h"
#include "LogicThreadBase.h"
#include "Registry.h"
#include "JoltPhysics.h"

WorldBase::WorldBase() = default;

bool WorldBase::IsLogicRunning() const { return Logic && Logic->IsRunning(); }

WorldBase::~WorldBase()
{
// If not already shut down, clean up gracefully.
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
