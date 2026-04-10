#include "FlowManager.h"

#include "EntityBuilder.h"
#include "GameMode.h"
#include "GameState.h"
#include "Logger.h"
#include "ReflectionRegistry.h"
#include "World.h"

#include <cstring>

FlowManager::FlowManager() = default;

FlowManager::~FlowManager()
{
	// Exit and destroy all states top-down
	for (int32_t i = static_cast<int32_t>(StateStackCount) - 1; i >= 0; --i)
	{
		if (StateStack[i]) StateStack[i]->OnExit(*this);
		StateStack[i].reset();
	}
	StateStackCount = 0;

	// Mode before World — Mode may reference World state
	ActiveMode.reset();

	// Destroy all Constructs before the World so Views unbind cleanly
	ConstructReg.DestroyAll();

	if (ActiveWorld)
	{
		ActiveWorld->Shutdown();
		ActiveWorld.reset();
	}
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

void FlowManager::Initialize(TrinyxEngine* engine, const EngineConfig* config,
							  int windowWidth, int windowHeight)
{
	Engine       = engine;
	Config       = config;
	WindowWidth  = windowWidth;
	WindowHeight = windowHeight;
}

// ---------------------------------------------------------------------------
// State registration
// ---------------------------------------------------------------------------

void FlowManager::RegisterState(const char* name, StateFactory factory)
{
	if (RegisteredStateCount >= MaxRegisteredStates)
	{
		LOG_ERROR("[FlowManager] Max registered states exceeded");
		return;
	}
	RegisteredStates[RegisteredStateCount++] = {name, std::move(factory)};
}

void FlowManager::RegisterMode(const char* name, ModeFactory factory)
{
	if (RegisteredModeCount >= MaxRegisteredModes)
	{
		LOG_ERROR("[FlowManager] Max registered modes exceeded");
		return;
	}
	RegisteredModes[RegisteredModeCount++] = {name, std::move(factory)};
}

// ---------------------------------------------------------------------------
// State stack operations
// ---------------------------------------------------------------------------

void FlowManager::LoadDefaultState(const char* stateName)
{
	if (StateStackCount > 0)
	{
		LOG_ERROR("[FlowManager] LoadDefaultState called with existing states");
		return;
	}

	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();
	EnforceRequirements(nullptr, newState.get());

	StateStack[0] = std::move(newState);
	StateStackCount = 1;
	StateStack[0]->OnEnter(*this, ActiveWorld.get());

	LOG_INFO_F("[FlowManager] Default state loaded: %s", stateName);
}

void FlowManager::TransitionTo(const char* stateName)
{
	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();

	// Get current top state for requirement comparison
	GameState* currentState = GetActiveState();

	// Exit all states top-down
	for (int32_t i = static_cast<int32_t>(StateStackCount) - 1; i >= 0; --i)
	{
		if (StateStack[i]) StateStack[i]->OnExit(*this);
		StateStack[i].reset();
	}
	StateStackCount = 0;

	// Enforce requirements between the old top state and new state
	EnforceRequirements(currentState, newState.get());

	// Push the new state as the sole entry
	StateStack[0] = std::move(newState);
	StateStackCount = 1;
	StateStack[0]->OnEnter(*this, ActiveWorld.get());

	LOG_INFO_F("[FlowManager] Transitioned to: %s", stateName);
}

void FlowManager::PushState(const char* stateName)
{
	if (StateStackCount >= MaxStateStack)
	{
		LOG_ERROR("[FlowManager] State stack overflow");
		return;
	}

	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();

	// Overlays don't change World/NetSession requirements — they inherit
	// from the base state. No EnforceRequirements call here.

	StateStack[StateStackCount] = std::move(newState);
	StateStackCount++;
	StateStack[StateStackCount - 1]->OnEnter(*this, ActiveWorld.get());

	LOG_INFO_F("[FlowManager] Pushed overlay: %s", stateName);
}

void FlowManager::PopState()
{
	if (StateStackCount <= 1)
	{
		LOG_ERROR("[FlowManager] Cannot pop the base state");
		return;
	}

	auto& top = StateStack[StateStackCount - 1];
	if (top) top->OnExit(*this);
	top.reset();
	StateStackCount--;

	LOG_INFO_F("[FlowManager] Popped overlay, active: %s",
			   GetActiveState() ? GetActiveState()->GetName() : "none");
}

// ---------------------------------------------------------------------------
// World / Level operations
// ---------------------------------------------------------------------------

World* FlowManager::CreateWorld()
{
	if (ActiveWorld)
	{
		LOG_ERROR("[FlowManager] CreateWorld called with existing World — destroy first");
		return ActiveWorld.get();
	}

	ActiveWorld = std::make_unique<World>();
	if (!ActiveWorld->Initialize(*Config, &ConstructReg, WindowWidth, WindowHeight))
	{
		LOG_ERROR("[FlowManager] World::Initialize failed");
		ActiveWorld.reset();
		return nullptr;
	}

	LOG_INFO("[FlowManager] World created");
	return ActiveWorld.get();
}

void FlowManager::DestroyWorld()
{
	if (!ActiveWorld) return;

	// Destroy Mode first — it may reference World state
	ActiveMode.reset();

	// Notify surviving Constructs (Session+Persistent) that the World is going away.
	// This calls OnWorldTeardown() then Shutdown() (deregisters ticks from old LogicThread).
	ConstructReg.NotifyWorldTeardown(ConstructLifetime::Session);

	// Destroy Level-lifetime and World-lifetime Constructs.
	// Their destructors call Shutdown() which deregisters ticks.
	ConstructReg.DestroyByLifetime(ConstructLifetime::Session);

	ActiveWorld->Shutdown();
	ActiveWorld.reset();

	LOG_INFO("[FlowManager] World destroyed");
}

void FlowManager::StartWorld()
{
	if (ActiveWorld) ActiveWorld->Start();
}

void FlowManager::StopWorld()
{
	if (ActiveWorld) ActiveWorld->Stop();
}

void FlowManager::JoinWorld()
{
	if (ActiveWorld) ActiveWorld->Join();
}

void FlowManager::LoadLevel(const char* levelPath)
{
	if (!ActiveWorld)
	{
		LOG_ERROR("[FlowManager] LoadLevel called with no active World");
		return;
	}

	if (!levelPath || levelPath[0] == '\0')
	{
		LOG_ERROR("[FlowManager] LoadLevel called with empty path");
		return;
	}

	ActiveLevelPath = levelPath;

	// Spawn entities from the scene file via the World's SpawnSync handshake.
	std::string path = ActiveLevelPath;
	ActiveWorld->Spawn([path](Registry* reg)
	{
		size_t count = EntityBuilder::SpawnFromFile(reg, path.c_str());
		LOG_INFO_F("[FlowManager] LoadLevel: spawned %zu entities from %s", count, path.c_str());
	});

	LOG_INFO_F("[FlowManager] Level loaded: %s", levelPath);
}

void FlowManager::UnloadLevel()
{
	// Destroy Level-lifetime Constructs
	ConstructReg.DestroyByLifetime(ConstructLifetime::Level);

	// TODO: despawn level-placed entities — needs a "level-scoped entity" tag
	// which doesn't exist yet. For now, entities persist until World destruction.

	ActiveLevelPath.clear();
	LOG_INFO("[FlowManager] Level unloaded");
}

// ---------------------------------------------------------------------------
// GameMode
// ---------------------------------------------------------------------------

void FlowManager::SetGameMode(const char* modeName)
{
	// Clear existing mode
	if (ActiveMode)
	{
		ActiveMode->OnWorldTeardown();
		ActiveMode.reset();
	}

	if (!modeName) return;

	auto factory = FindModeFactory(modeName);
	if (!factory)
	{
		LOG_ERROR_F("[FlowManager] Unknown mode: %s", modeName);
		return;
	}

	ActiveMode = factory();
	if (ActiveWorld)
	{
		ActiveMode->Initialize(ActiveWorld.get());
	}

	LOG_INFO_F("[FlowManager] GameMode set: %s", modeName);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void FlowManager::PostNetEvent(uint8_t eventID)
{
	PendingNetEvents.fetch_or(1u << eventID, std::memory_order_release);
}

void FlowManager::Tick(float dt)
{
	// Drain net events posted by NetThread — dispatch to active state on Sentinel.
	const uint32_t events = PendingNetEvents.exchange(0, std::memory_order_acquire);
	if (events)
	{
		if (GameState* active = GetActiveState())
		{
			uint32_t bits = events;
			while (bits)
			{
				const uint32_t bit = bits & (~bits + 1); // isolate lowest set bit
				const uint8_t id   = static_cast<uint8_t>(__builtin_ctz(bit));
				active->OnNetEvent(id);
				bits &= bits - 1;
			}
		}
	}

	// Tick the active (top) state
	if (GameState* active = GetActiveState())
	{
		active->Tick(dt);
	}
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

GameState* FlowManager::GetActiveState() const
{
	if (StateStackCount == 0) return nullptr;
	return StateStack[StateStackCount - 1].get();
}

World* FlowManager::GetWorld() const
{
	return ActiveWorld.get();
}

bool FlowManager::HasWorld() const
{
	return ActiveWorld != nullptr;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

FlowManager::StateFactory FlowManager::FindStateFactory(const char* name) const
{
	// Check local overrides first (manual RegisterState calls)
	for (uint32_t i = 0; i < RegisteredStateCount; ++i)
	{
		if (strcmp(RegisteredStates[i].Name, name) == 0)
			return RegisteredStates[i].Factory;
	}

	// Fall back to ReflectionRegistry (populated by TNX_REGISTER_STATE macros)
	return ReflectionRegistry::Get().FindStateFactory(name);
}

FlowManager::ModeFactory FlowManager::FindModeFactory(const char* name) const
{
	// Check local overrides first (manual RegisterMode calls)
	for (uint32_t i = 0; i < RegisteredModeCount; ++i)
	{
		if (strcmp(RegisteredModes[i].Name, name) == 0)
			return RegisteredModes[i].Factory;
	}

	// Fall back to ReflectionRegistry (populated by TNX_REGISTER_MODE macros)
	return ReflectionRegistry::Get().FindModeFactory(name);
}

void FlowManager::EnforceRequirements(GameState* currentState, GameState* nextState)
{
	const StateRequirements current = currentState ? currentState->GetRequirements() : StateRequirements{};
	const StateRequirements next    = nextState    ? nextState->GetRequirements()    : StateRequirements{};

	// Tear down subsystems the new state doesn't need
	if (current.NeedsWorld && !next.NeedsWorld)
	{
		DestroyWorld();
	}

	// TODO: NetSession teardown when current.NeedsNetSession && !next.NeedsNetSession

	// Create subsystems the new state needs
	if (next.NeedsWorld && !ActiveWorld)
	{
		CreateWorld();
	}

	// TODO: NetSession creation when next.NeedsNetSession && !NetSession
}

