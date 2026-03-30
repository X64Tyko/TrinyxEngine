#include "World.h"

#include "Logger.h"
#include "LogicThread.h"
#include "Registry.h"
#include "JoltPhysics.h"

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

bool World::Initialize(const EngineConfig& config, int windowWidth, int windowHeight)
{
	Config = config;

	// --- Registry ---
	RegistryPtr = std::make_unique<Registry>(&Config);

	// --- Physics ---
	Physics = std::make_unique<JoltPhysics>();
	if (!Physics->Initialize(&Config))
	{
		LOG_ERROR("[World] JoltPhysics::Initialize failed");
		return false;
	}
	RegistryPtr->SetPhysics(Physics.get());

	// --- Logic thread (created but not started) ---
	Logic = std::make_unique<LogicThread>();
	Logic->Initialize(RegistryPtr.get(), &Config, Physics.get(),
					  &SimInput, &VizInput, &Spawner, &bJobsInitialized,
					  windowWidth, windowHeight);

	LOG_INFO("[World] Initialized");
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

	// Destroy in reverse order of creation.
	Logic.reset();
	Physics.reset();
	RegistryPtr.reset();

	LOG_INFO("[World] Shut down");
}

void World::ResetRegistry() const
{
	if (RegistryPtr) RegistryPtr->ResetRegistry();
}

void World::ConfirmLocalRecycles() const
{
	if (RegistryPtr) RegistryPtr->ConfirmLocalRecycles();
}