#include "World.h"

#include "Logger.h"
#include "LogicThread.h"
#include "Registry.h"
#include "JoltPhysics.h"

World::World() = default;

bool World::IsLogicRunning() const { return Logic && Logic->IsRunning(); }

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
	Logic    = std::make_unique<LogicThread>();
	WQHandle = TrinyxJobs::CreateWorldQueue();
	if (WQHandle == TrinyxJobs::InvalidWorldQueue)
	{
		LOG_ENG_ERROR("[World] Failed to create WorldQueue");
		return false;
	}
	Logic->Initialize(RegistryPtr.get(), &Config, Physics.get(),
					  &SimInput, &VizInput,
					  (Config.Mode == EngineMode::Client) ? &InputAccumRing : nullptr,
					  (Config.Mode == EngineMode::Client) ? &bInputAccumEnabled : nullptr,
					  WQHandle, &bJobsInitialized,
					  windowWidth, windowHeight);
	Logic->SetConstructRegistry(Constructs);

	// Client-side input accumulator: ring sized to comfortably exceed the worst-case
	// unacked window (~100 frames at 512Hz + 200ms RTT). 256 slots = ample headroom.
	// Only initialized (and drained by the net thread) when running as a Client.
	if (Config.Mode == EngineMode::Client)
	{
		if (!InputAccumRing.Initialize(256))
		{
			LOG_ENG_ERROR("[World] Failed to initialize InputAccumRing");
			return false;
		}
		InputAccumConsumer.emplace(InputAccumRing.MakeConsumer());
	}

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

	if (WQHandle != TrinyxJobs::InvalidWorldQueue)
	{
		TrinyxJobs::DestroyWorldQueue(WQHandle);
		WQHandle = TrinyxJobs::InvalidWorldQueue;
	}

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