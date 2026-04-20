#include "FlowManager.h"

#include "AssetRegistry.h"
#include "EntityBuilder.h"
#include "GameMode.h"
#include "FlowState.h"
#include "Logger.h"
#include "NetChannel.h"
#include "NetTypes.h"
#include "ReflectionRegistry.h"
#include "Registry.h"
#include "World.h"

#include <cstring>

#include "LogicThread.h"

FlowManager::FlowManager() = default;

FlowManager::~FlowManager()
{
	// Exit and destroy all states top-down
	for (int32_t i = static_cast<int32_t>(StateStackCount) - 1; i >= 0; --i)
	{
		if (StateStack[i]) StateStack[i]->OnExit();
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
		LOG_ENG_ERROR("[FlowManager] Max registered states exceeded");
		return;
	}
	RegisteredStates[RegisteredStateCount++] = {name, std::move(factory)};
}

void FlowManager::RegisterMode(const char* name, ModeFactory factory)
{
	if (RegisteredModeCount >= MaxRegisteredModes)
	{
		LOG_ENG_ERROR("[FlowManager] Max registered modes exceeded");
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
		LOG_ENG_ERROR("[FlowManager] LoadDefaultState called with existing states");
		return;
	}

	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ENG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();
	EnforceRequirements(nullptr, newState.get());

	StateStack[0]   = std::move(newState);
	StateStackCount = 1;
	StateStack[0]->OnEnter(*this, ActiveWorld.get());

	LOG_ENG_INFO_F("[FlowManager] Default state loaded: %s", stateName);
}

void FlowManager::TransitionTo(const char* stateName)
{
	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ENG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();

	// Get current top state for requirement comparison
	FlowState* currentState = GetActiveState();

	// Exit all states top-down
	for (int32_t i = static_cast<int32_t>(StateStackCount) - 1; i >= 0; --i)
	{
		if (StateStack[i]) StateStack[i]->OnExit();
		StateStack[i].reset();
	}
	StateStackCount = 0;

	// Enforce requirements between the old top state and new state
	EnforceRequirements(currentState, newState.get());

	// Push the new state as the sole entry
	StateStack[0]   = std::move(newState);
	StateStackCount = 1;
	StateStack[0]->OnEnter(*this, ActiveWorld.get());

	LOG_ENG_INFO_F("[FlowManager] Transitioned to: %s", stateName);
}

void FlowManager::PushState(const char* stateName)
{
	if (StateStackCount >= MaxStateStack)
	{
		LOG_ENG_ERROR("[FlowManager] State stack overflow");
		return;
	}

	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ENG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();

	// Overlays don't change World/NetSession requirements — they inherit
	// from the base state. No EnforceRequirements call here.

	StateStack[StateStackCount] = std::move(newState);
	StateStackCount++;
	StateStack[StateStackCount - 1]->OnEnter(*this, ActiveWorld.get());

	LOG_ENG_INFO_F("[FlowManager] Pushed overlay: %s", stateName);
}

void FlowManager::PopState()
{
	if (StateStackCount <= 1)
	{
		LOG_ENG_ERROR("[FlowManager] Cannot pop the base state");
		return;
	}

	auto& top = StateStack[StateStackCount - 1];
	if (top) top->OnExit();
	top.reset();
	StateStackCount--;

	LOG_ENG_INFO_F("[FlowManager] Popped overlay, active: %s",
				   GetActiveState() ? GetActiveState()->GetName() : "none");
}

// ---------------------------------------------------------------------------
// World / Level operations
// ---------------------------------------------------------------------------

World* FlowManager::CreateWorld()
{
	if (ActiveWorld)
	{
		LOG_ENG_ERROR("[FlowManager] CreateWorld called with existing World — destroy first");
		return ActiveWorld.get();
	}

	ActiveWorld = std::make_unique<World>();
	if (!ActiveWorld->Initialize(*Config, &ConstructReg, WindowWidth, WindowHeight))
	{
		LOG_ENG_ERROR("[FlowManager] World::Initialize failed");
		ActiveWorld.reset();
		return nullptr;
	}

	ActiveWorld->SetFlowManager(this);
	LOG_ENG_INFO("[FlowManager] World created");
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
	ActiveWorld->SetFlowManager(nullptr);
	ActiveWorld.reset();

	LOG_ENG_INFO("[FlowManager] World destroyed");
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

void FlowManager::LoadLevel(const char* levelPath, bool bBackground)
{
	if (!ActiveWorld)
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel called with no active World");
		return;
	}

	if (!levelPath || levelPath[0] == '\0')
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel called with empty path");
		return;
	}

	ActiveLevelPath = levelPath;

	Registry* reg        = ActiveWorld->GetRegistry();
	const char* pathCStr = ActiveLevelPath.c_str();

#ifdef TNX_ENABLE_ROLLBACK
	const uint32_t spawnFrame = GetWorld()->GetLogicThread()->GetLastCompletedFrame() + 1;
	Soul* soul                = GetSoul(GetWorld()->LocalOwnerID);
	ActiveWorld->SpawnAndWait([reg, pathCStr, bBackground, spawnFrame, soul](uint32_t)
	{
		std::vector<GlobalEntityHandle> spawnedHandles;
		size_t count = EntityBuilder::SpawnFromFileTracked(reg, pathCStr, bBackground, spawnedHandles);
		LOG_NET_INFO_F(soul, "[FlowManager] LoadLevel: spawned %zu entities from %s%s at frame %u",
					   count, pathCStr, bBackground ? " (Alive-only)" : "", spawnFrame);

		// Push reInit events so resim crossing this frame can re-hydrate level entity slab slots.
		for (GlobalEntityHandle gh : spawnedHandles) reg->PushEntityReinitEvent(gh, spawnFrame);
	});
#else
	ActiveWorld->SpawnAndWait([reg, pathCStr, bBackground](uint32_t)
	{
		size_t count = EntityBuilder::SpawnFromFile(reg, pathCStr, bBackground);
		LOG_NET_INFO_F(nullptr, "[FlowManager] LoadLevel: spawned %zu entities from %s%s",
					   count, pathCStr, bBackground ? " (Alive-only)" : "");
	});
#endif

	LOG_NET_INFO_F(nullptr, "[FlowManager] Level loaded: %s", levelPath);
}

void FlowManager::LoadLevel(const AssetID& id, bool bBackground)
{
	std::string path = AssetRegistry::Get().ResolvePath(id);
	if (path.empty())
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel(AssetID): asset not found or no content root set");
		return;
	}
	LoadLevel(path.c_str(), bBackground);
}

void FlowManager::LoadLevelByName(const char* name, bool bBackground)
{
	if (!name || name[0] == '\0')
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevelByName: empty name");
		return;
	}
	std::string path = AssetRegistry::Get().ResolvePathByName(name);
	if (path.empty())
	{
		LOG_ENG_ERROR_F("[FlowManager] LoadLevelByName: '%s' not found in AssetRegistry", name);
		return;
	}
	LoadLevel(path.c_str(), bBackground);
}

void FlowManager::UnloadLevel()
{
	// Destroy Level-lifetime Constructs
	ConstructReg.DestroyByLifetime(ConstructLifetime::Level);

	// TODO: despawn level-placed entities — needs a "level-scoped entity" tag
	// which doesn't exist yet. For now, entities persist until World destruction.

	ActiveLevelPath.clear();
	LOG_ENG_INFO("[FlowManager] Level unloaded");
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
		LOG_ENG_ERROR_F("[FlowManager] Unknown mode: %s", modeName);
		return;
	}

	ActiveMode = factory();
	if (ActiveWorld)
	{
		ActiveMode->Initialize(ActiveWorld.get());
	}

	LOG_ENG_INFO_F("[FlowManager] GameMode set: %s", modeName);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void FlowManager::PostNetEvent(uint8_t eventID)
{
	PendingNetEvents.fetch_or(1u << eventID, std::memory_order_release);
}

std::string FlowManager::GetActiveLevelLocalPath() const
{
	if (ActiveLevelPath.empty()) return {};
	if (Config && Config->ProjectDir[0] != '\0')
	{
		std::string prefix = std::string(Config->ProjectDir) + "/content/";
		if (ActiveLevelPath.rfind(prefix, 0) == 0) return ActiveLevelPath.substr(prefix.size());
	}
	return ActiveLevelPath; // Fallback: return as-is (non-standard path)
}

void FlowManager::PostTravelNotify(const char* levelPath)
{
	// Store the path (written from NetThread, read from Sentinel in OnNetEvent).
	// This is a benign data race under the sequenced-happens-before guarantee:
	// PostNetEvent's release fence ensures the path write is visible before the
	// event bit, and Sentinel reads after the acquire in Tick().
	PendingTravelPath = levelPath ? levelPath : "";
	PostNetEvent(static_cast<uint8_t>(FlowEventID::TravelNotify));
}

void FlowManager::PostPlayerBeginConfirm(const PlayerBeginConfirmPayload& payload)
{
	// Same sequenced-happens-before contract as PostTravelNotify.
	// Write before the event bit so Sentinel sees the payload atomically.
	PendingPlayerBeginConfirm = payload;
	PostNetEvent(static_cast<uint8_t>(FlowEventID::PlayerBeginConfirm));
}

void FlowManager::Tick(float dt)
{
	// Drain net events posted by NetThread — dispatch to active state on Sentinel.
	const uint32_t events = PendingNetEvents.exchange(0, std::memory_order_acquire);
	if (events)
	{
		if (FlowState* active = GetActiveState())
		{
			uint32_t bits = events;
			while (bits)
			{
				const uint32_t bit = bits & (~bits + 1); // isolate lowest set bit
				const uint8_t id   = static_cast<uint8_t>(TNX_CTZ32(bit));

				// Auto-handle TravelNotify for states that declare NeedsLevel.
				// The FlowState receives OnNetEvent afterward in case it needs to
				// do additional work (e.g., overlay logic, HUD reset).
				if (id == static_cast<uint8_t>(FlowEventID::TravelNotify)
					&& active->GetRequirements().NeedsLevel
					&& !PendingTravelPath.empty())
				{
					// PendingTravelPath is a content-root-relative local path sent by the server.
					// Resolve to absolute before passing to LoadLevel.
					const std::string& root = AssetRegistry::Get().GetContentRoot();
					std::string absPath     = root.empty()
												  ? PendingTravelPath
												  : root + "/" + PendingTravelPath;
					LoadLevel(absPath.c_str(), /*bBackground=*/true);
				}

				active->OnNetEvent(id);
				bits &= bits - 1;
			}
		}
	}

	// Tick the active (top) state
	if (FlowState* active = GetActiveState())
	{
		active->Tick(dt);
	}
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

FlowState* FlowManager::GetActiveState() const
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
		if (strcmp(RegisteredStates[i].Name, name) == 0) return RegisteredStates[i].Factory;
	}

	// Fall back to ReflectionRegistry (populated by TNX_REGISTER_STATE macros)
	return ReflectionRegistry::Get().FindStateFactory(name);
}

FlowManager::ModeFactory FlowManager::FindModeFactory(const char* name) const
{
	// Check local overrides first (manual RegisterMode calls)
	for (uint32_t i = 0; i < RegisteredModeCount; ++i)
	{
		if (strcmp(RegisteredModes[i].Name, name) == 0) return RegisteredModes[i].Factory;
	}

	// Fall back to ReflectionRegistry (populated by TNX_REGISTER_MODE macros)
	return ReflectionRegistry::Get().FindModeFactory(name);
}

void FlowManager::EnforceRequirements(FlowState* currentState, FlowState* nextState)
{
	const StateRequirements current = currentState ? currentState->GetRequirements() : StateRequirements{};
	const StateRequirements next    = nextState ? nextState->GetRequirements() : StateRequirements{};

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

// ---------------------------------------------------------------------------
// Soul lifecycle
// ---------------------------------------------------------------------------

void FlowManager::OnClientLoaded(uint8_t ownerID)
{
	if (Souls[ownerID]) return; // Already present — guard against double-create

	Souls[ownerID]          = std::make_unique<Soul>(ownerID);
	Souls[ownerID]->FlowMgr = this;
	// ownerID=0 is the listen-server's own local player — drives via keyboard AND
	// is the simulation authority. ownerID>0 are remote clients — authority only.
	Souls[ownerID]->SetRole(SoulRole::Authority);
	if (ownerID == 0) Souls[ownerID]->AddRole(SoulRole::Owner);
	Souls[ownerID]->OnJoined();

	LOG_NET_INFO_F(Souls[ownerID].get(), "[FlowMgr] OnClientLoaded: ownerID=%u joined", ownerID);

	if (ActiveMode) ActiveMode->OnPlayerJoined(*Souls[ownerID]);
}

void FlowManager::OnLocalOwnerConnected(uint8_t ownerID)
{
	if (Souls[ownerID]) return; // Idempotent

	Souls[ownerID]          = std::make_unique<Soul>(ownerID);
	Souls[ownerID]->FlowMgr = this;
	Souls[ownerID]->SetRole(SoulRole::Owner);
	Souls[ownerID]->OnJoined();
}

void FlowManager::OnClientDisconnected(uint8_t ownerID)
{
	if (!Souls[ownerID]) return;

	LOG_NET_INFO_F(Souls[ownerID].get(), "[FlowMgr] OnClientDisconnected: ownerID=%u left", ownerID);

	if (ActiveMode) ActiveMode->OnPlayerLeft(*Souls[ownerID]);

	Souls[ownerID]->OnLeft();
	Souls[ownerID].reset();
}

#ifdef TNX_ENABLE_NETWORK
std::optional<PlayerBeginResult> FlowManager::HandlePlayerBeginRequest(Soul* soul, const PlayerBeginRequestPayload& req)
{
	if (!soul)
	{
		LOG_NET_WARN(nullptr, "[FlowMgr] HandlePlayerBeginRequest: null Soul");
		return std::nullopt;
	}

	// Delegate entirely to GameMode — all game-layer spawn logic lives there.
	// GameMode returns a PlayerBeginResult; no Soul fields used as a data relay.
	PlayerBeginResult result;
	if (ActiveMode) result = ActiveMode->OnPlayerBeginRequest(*soul, req);
	else
	{
		// No GameMode: accept unconditionally, echo client hint.
		result.Accepted = true;
		result.PosX     = req.PosX;
		result.PosY     = req.PosY;
		result.PosZ     = req.PosZ;
		soul->ClaimBody({});
	}

	if (!result.Accepted) return std::nullopt;
	return result;
}

void FlowManager::SendPlayerBeginRequest(NetChannel channel, uint32_t frameNumber, PredictionLedger& ledger)
{
	const uint8_t ownerID           = channel.OwnerID();
	constexpr uint32_t PredictionID = 1; // Single in-flight entry today

	// Create the client-side Soul on first call.
	if (!Souls[ownerID])
	{
		Souls[ownerID]          = std::make_unique<Soul>(ownerID);
		Souls[ownerID]->FlowMgr = this;
		Souls[ownerID]->SetRole(SoulRole::Owner); // owning client: predicts locally
		Souls[ownerID]->OnJoined();
	}
	Souls[ownerID]->Channel = channel;

	PlayerBeginRequestPayload req{};
	req.PrefabID     = 0; // 0 = server picks via GameMode::GetCharacterPrefab
	req.PredictionID = PredictionID;
	req.PosX         = 0.0f;
	req.PosY         = 5.0f;
	req.PosZ         = 0.0f;

	LOG_NET_INFO_F(Souls[ownerID].get(), "[FlowMgr] SendPlayerBeginRequest: ownerID=%u frame=%u", ownerID, frameNumber);

	ledger.Set(PredictionID, frameNumber, {}, req.PrefabID);

	// Fire the PlayerBegin SoulRPC — thunk packs RPCHeader + payload and sends
	// as NetMessageType::SoulRPC, replacing the old direct PlayerBeginRequest send.
	if (!Souls[ownerID]->PlayerBegin(req)) [[unlikely]]
	LOG_NET_WARN_F(Souls[ownerID].get(), "[FlowMgr] PlayerBeginRequest send failed (GNS rejected) — ownerID=%u", ownerID);
}
#endif // TNX_ENABLE_NETWORK
