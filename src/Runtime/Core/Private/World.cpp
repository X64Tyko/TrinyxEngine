#include "World.h"

#include "Logger.h"
#include "LogicThread.h"
#include "Registry.h"
#include "JoltPhysics.h"

World::World() = default;

World::~World()
{
	// If not already shut down, clean up gracefully.
	if (Logic&& Logic
	->
	IsRunning()
	)
	{
		Stop();
		Join();
	}
}

bool World::Initialize(const EngineConfig& config, ConstructRegistry* constructRegistry,
					   int windowWidth, int windowHeight)
{
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

	// --- Logic thread (created but not started) ---
	Logic = std::make_unique<LogicThread>();
	Logic->Initialize(RegistryPtr.get(), &Config, Physics.get(),
					  &SimInput, &VizInput, &Spawner, &bJobsInitialized,
					  windowWidth, windowHeight);
	Logic->SetConstructRegistry(Constructs);

	LOG_ENG_INFO("[World] Initialized");
	return true;
}

void World::Start()
{
	if (Logic) Logic->Start();
}

void World::Stop()
{
	if (Logic) Logic->Stop();
}

void World::Join()
{
	if (Logic) Logic->Join();
}

void World::Shutdown()
{
	Stop();
	Join();

	// Note: ConstructRegistry is owned by FlowManager and outlives the World.
	// World-lifetime and Level-lifetime Constructs are destroyed by FlowManager
	// during transitions, not here.

	Logic.reset();
	Physics.reset();
	RegistryPtr.reset();

	Constructs = nullptr;
	LOG_ENG_INFO("[World] Shut down");
}

void World::ResetRegistry() const
{
	if (RegistryPtr) RegistryPtr->ResetRegistry();
}

void World::ConfirmLocalRecycles() const
{
	if (RegistryPtr) RegistryPtr->ConfirmLocalRecycles();
}