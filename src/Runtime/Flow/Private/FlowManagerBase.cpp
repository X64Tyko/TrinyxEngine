#include "FlowManagerBase.h"

#include "AssetRegistry.h"
#include "GameMode.h"
#include "FlowState.h"
#include "Json.h"
#include "TrinyxJobs.h"
#include "Logger.h"
#include "NetChannel.h"
#include "NetTypes.h"
#include "ReflectionRegistry.h"
#include "StreamingManager.h"
#include "TrinyxEngine.h"
#include "WorldBase.h"

#include <cstring>
#include <fstream>

#include "LogicThreadBase.h"

FlowManagerBase::FlowManagerBase() = default;

FlowManagerBase::~FlowManagerBase()
{
	// If a preload job is still in-flight, wait for it before destroying PendingMode
	// and PendingPreloadCounter. The job holds a raw pointer to PendingMode and
	// decrements &PendingPreloadCounter — both are members of this object.
	if (PendingPreloadCounter.Value.load(std::memory_order_acquire) != 0)
		TrinyxJobs::WaitForCounter(&PendingPreloadCounter, TrinyxJobs::Queue::General);
	PendingMode.reset();

	// Destroy all states top-down.
	for (int32_t i = static_cast<int32_t>(StateStackCount) - 1; i >= 0; --i)
	{
		if (StateStack[i]) StateStack[i]->OnExit();
		StateStack[i].reset();
	}
	StateStackCount = 0;

	// Mode before World — Mode may reference World state
	ActiveMode.reset();
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

void FlowManagerBase::Initialize(TrinyxEngine* engine, const EngineConfig* config,
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

void FlowManagerBase::RegisterState(const char* name, StateFactory factory)
{
	if (RegisteredStateCount >= MaxRegisteredStates)
	{
		LOG_ENG_ERROR("[FlowManager] Max registered states exceeded");
		return;
	}
	RegisteredStates[RegisteredStateCount++] = { name, std::move(factory) };
}

void FlowManagerBase::RegisterMode(const char* name, ModeFactory factory)
{
	if (RegisteredModeCount >= MaxRegisteredModes)
	{
		LOG_ENG_ERROR("[FlowManager] Max registered modes exceeded");
		return;
	}
	RegisteredModes[RegisteredModeCount++] = { name, std::move(factory) };
}

// ---------------------------------------------------------------------------
// State stack operations
// ---------------------------------------------------------------------------

void FlowManagerBase::LoadDefaultState(const char* stateName)
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

void FlowManagerBase::TransitionTo(const char* stateName)
{
	auto factory = FindStateFactory(stateName);
	if (!factory)
	{
		LOG_ENG_ERROR_F("[FlowManager] Unknown state: %s", stateName);
		return;
	}

	auto newState = factory();

	FlowState* currentState = GetActiveState();

	for (int32_t i = static_cast<int32_t>(StateStackCount) - 1; i >= 0; --i)
	{
		if (StateStack[i]) StateStack[i]->OnExit();
		StateStack[i].reset();
	}
	StateStackCount = 0;

	EnforceRequirements(currentState, newState.get());

	StateStack[0]   = std::move(newState);
	StateStackCount = 1;
	StateStack[0]->OnEnter(*this, ActiveWorld.get());

	LOG_ENG_INFO_F("[FlowManager] Transitioned to: %s", stateName);
}

void FlowManagerBase::PushState(const char* stateName)
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

	StateStack[StateStackCount] = std::move(newState);
	StateStackCount++;
	StateStack[StateStackCount - 1]->OnEnter(*this, ActiveWorld.get());

	LOG_ENG_INFO_F("[FlowManager] Pushed overlay: %s", stateName);
}

void FlowManagerBase::PopState()
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

WorldBase* FlowManagerBase::CreateWorld()
{
	if (ActiveWorld)
	{
		LOG_ENG_ERROR("[FlowManager] CreateWorld called with existing World — destroy first");
		return ActiveWorld.get();
	}
	return CreateWorldImpl();
}

void FlowManagerBase::DestroyWorld()
{
	if (!ActiveWorld) return;

	ActiveMode.reset();

	ConstructReg.NotifyWorldTeardown(ConstructLifetime::Session);
	ConstructReg.DestroyByLifetime(ConstructLifetime::Session);

	ActiveWorld->Shutdown();
	ActiveWorld->SetFlowManager(nullptr);
	ActiveWorld.reset();

	LOG_ENG_INFO("[FlowManager] World destroyed");
}

void FlowManagerBase::StartWorld()
{
	if (ActiveWorld) ActiveWorld->Start();
}

void FlowManagerBase::StopWorld()
{
	if (ActiveWorld) ActiveWorld->Stop();
}

void FlowManagerBase::JoinWorld()
{
	if (ActiveWorld) ActiveWorld->Join();
}

void FlowManagerBase::LoadLevel(const char* /*levelPath*/, bool /*bBackground*/)
{
	// Default body — overridden by FlowManager<TNet,TRollback,TFrame>.
	// Reachable only if a FlowManagerBase subclass doesn't override LoadLevel.
	LOG_ENG_ERROR("[FlowManager] LoadLevel(const char*) base called — no typed override");
}

void FlowManagerBase::LoadLevel(const AssetID& id, bool bBackground)
{
	std::string path = AssetRegistry::Get().ResolvePath(id);
	if (path.empty())
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel(AssetID): asset not found or no content root set");
		return;
	}
	LoadLevel(path.c_str(), bBackground);
}

void FlowManagerBase::LoadLevelByName(const char* name, bool bBackground)
{
	if (!name || name[0] == '\0')
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevelByName: empty name");
		return;
	}
	std::string path = AssetRegistry::Get().ResolvePathByTNameAndType(TnxName(name), AssetType::Level);
	if (path.empty())
	{
		LOG_ENG_ERROR_F("[FlowManager] LoadLevelByName: '%s' not found in AssetRegistry", name);
		return;
	}
	LoadLevel(path.c_str(), bBackground);
}

void FlowManagerBase::UnloadLevel()
{
	ConstructReg.DestroyByLifetime(ConstructLifetime::Level);
	ActiveLevelPath.clear();
	LOG_ENG_INFO("[FlowManager] Level unloaded");
}

// ---------------------------------------------------------------------------
// GameMode
// ---------------------------------------------------------------------------

void FlowManagerBase::SetGameMode(const char* modeName)
{
	if (ActiveMode)
	{
		ActiveMode->OnWorldTeardown();
		ActiveMode.reset();
	}

	// If a previous preload job is still running, wait for it before destroying the mode.
	// The job lambda captures a raw pointer to PendingMode — destroying the mode while
	// the job runs is UB. This only blocks if SetGameMode is called twice in quick succession.
	if (PendingMode && PendingPreloadCounter.Value.load(std::memory_order_acquire) != 0)
		TrinyxJobs::WaitForCounter(&PendingPreloadCounter, TrinyxJobs::Queue::General);

	PendingMode.reset();
	PendingPlayerJoins = 0;

	if (!modeName) return;

	auto factory = FindModeFactory(modeName);
	if (!factory)
	{
		LOG_ENG_ERROR_F("[FlowManager] Unknown mode: %s", modeName);
		return;
	}

	PendingMode = factory();

	// Dispatch OnPreload to a General worker — Sentinel stays responsive to
	// client connections while assets load from disk.
	// Initialize() is called from Tick() once preload and uploads are complete.
	GameMode* mode = PendingMode.get();
	TrinyxJobs::Dispatch([mode](uint32_t)
	{
		mode->OnPreload();
	}, &PendingPreloadCounter, TrinyxJobs::Queue::General);

	LOG_ENG_INFO_F("[FlowManager] GameMode preloading: %s", modeName);
}

// ---------------------------------------------------------------------------
// Tick / net events
// ---------------------------------------------------------------------------

void FlowManagerBase::PostNetEvent(uint8_t eventID)
{
	PendingNetEvents.fetch_or(1u << eventID, std::memory_order_release);
}

std::string FlowManagerBase::GetActiveLevelLocalPath() const
{
	if (ActiveLevelPath.empty()) return {};
	if (Config && Config->ProjectDir[0] != '\0')
	{
		std::string prefix = std::string(Config->ProjectDir) + "/content/";
		if (ActiveLevelPath.rfind(prefix, 0) == 0) return ActiveLevelPath.substr(prefix.size());
	}
	return ActiveLevelPath;
}

StreamingManager* FlowManagerBase::GetStreamingManager() const
{
	return Engine ? Engine->GetStreamingManager() : nullptr;
}

void* FlowManagerBase::GetRawNetThread() const
{
#ifdef TNX_ENABLE_NETWORK
	return Engine ? static_cast<void*>(Engine->GetNetThread()) : nullptr;
#else
	return nullptr;
#endif
}

void FlowManagerBase::PostTravelNotify(const char* levelPath)
{
	PendingTravelPath = levelPath ? levelPath : "";
	PostNetEvent(static_cast<uint8_t>(FlowEventID::TravelNotify));
}

void FlowManagerBase::PostPlayerBeginConfirm(const PlayerBeginConfirmPayload& payload)
{
	PendingPlayerBeginConfirm = payload;
	PostNetEvent(static_cast<uint8_t>(FlowEventID::PlayerBeginConfirm));
}

void FlowManagerBase::CheckPendingMode()
{
	if (!PendingMode) return;

	// OnPreload() job not yet done.
	if (PendingPreloadCounter.Value.load(std::memory_order_acquire) != 0) return;

	// GPU uploads dispatched during OnPreload() not yet complete.
	if (!PendingMode->AreUploadsReady()) return;

	// Both CPU and GPU work are done — promote and initialize.
	ActiveMode = std::move(PendingMode);
	if (ActiveWorld)
		ActiveMode->Initialize(ActiveWorld.get());

	// Drain any player joins that arrived while the mode was preloading.
	uint32_t pending   = PendingPlayerJoins;
	PendingPlayerJoins = 0;
	while (pending)
	{
		const uint32_t bit = pending & (~pending + 1);
		const uint8_t oid  = static_cast<uint8_t>(TNX_CTZ32(bit));
		if (Souls[oid]) ActiveMode->OnPlayerJoined(*Souls[oid]);
		pending &= pending - 1;
	}

	LOG_ENG_INFO_F("[FlowManager] GameMode ready: %s", ActiveMode->GetModeName());
}

void FlowManagerBase::Tick(SimFloat dt)
{
	CheckPendingMode();

	const uint32_t events = PendingNetEvents.exchange(0, std::memory_order_acquire);
	if (events)
	{
		if (FlowState* active = GetActiveState())
		{
			uint32_t bits = events;
			while (bits)
			{
				const uint32_t bit = bits & (~bits + 1);
				const uint8_t id   = static_cast<uint8_t>(TNX_CTZ32(bit));

				if (id == static_cast<uint8_t>(FlowEventID::TravelNotify)
					&& active->GetRequirements().NeedsLevel
					&& !PendingTravelPath.empty())
				{
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

	if (FlowState* active = GetActiveState())
	{
		active->Tick(dt);
	}
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

FlowState* FlowManagerBase::GetActiveState() const
{
	if (StateStackCount == 0) return nullptr;
	return StateStack[StateStackCount - 1].get();
}

WorldBase* FlowManagerBase::GetWorld() const
{
	return ActiveWorld.get();
}

bool FlowManagerBase::HasWorld() const
{
	return ActiveWorld != nullptr;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

FlowManagerBase::StateFactory FlowManagerBase::FindStateFactory(const char* name) const
{
	for (uint32_t i = 0; i < RegisteredStateCount; ++i)
	{
		if (strcmp(RegisteredStates[i].Name, name) == 0) return RegisteredStates[i].Factory;
	}
	return ReflectionRegistry::Get().FindStateFactory(name);
}

FlowManagerBase::ModeFactory FlowManagerBase::FindModeFactory(const char* name) const
{
	for (uint32_t i = 0; i < RegisteredModeCount; ++i)
	{
		if (strcmp(RegisteredModes[i].Name, name) == 0) return RegisteredModes[i].Factory;
	}
	return ReflectionRegistry::Get().FindModeFactory(name);
}

void FlowManagerBase::EnforceRequirements(FlowState* currentState, FlowState* nextState)
{
	const StateRequirements current = currentState ? currentState->GetRequirements() : StateRequirements{};
	const StateRequirements next    = nextState ? nextState->GetRequirements() : StateRequirements{};

	if (current.NeedsWorld && !next.NeedsWorld)
	{
		DestroyWorld();
	}

	if (next.NeedsWorld && !ActiveWorld)
	{
		CreateWorld();
	}
}

// ---------------------------------------------------------------------------
// Soul lifecycle
// ---------------------------------------------------------------------------

void FlowManagerBase::OnClientLoaded(uint8_t ownerID)
{
	if (Souls[ownerID]) return;

	Souls[ownerID]          = std::make_unique<Soul>(ownerID);
	Souls[ownerID]->FlowMgr = this;
	Souls[ownerID]->SetRole(SoulRole::Authority);
	if (ownerID == 0) Souls[ownerID]->AddRole(SoulRole::Owner);
	Souls[ownerID]->OnJoined();

	LOG_NET_INFO_F(Souls[ownerID].get(), "[FlowMgr] OnClientLoaded: ownerID=%u joined", ownerID);

	if (ActiveMode)
	{
		ActiveMode->OnPlayerJoined(*Souls[ownerID]);
	}
	else if (PendingMode)
	{
		// Mode is preloading — defer OnPlayerJoined until CheckPendingMode promotes it.
		PendingPlayerJoins |= (1u << ownerID);
		LOG_ENG_INFO_F("[FlowMgr] Deferred OnPlayerJoined for ownerID=%u (mode preloading)", ownerID);
	}
}

void FlowManagerBase::OnLocalOwnerConnected(uint8_t ownerID)
{
	if (Souls[ownerID]) return;

	Souls[ownerID]          = std::make_unique<Soul>(ownerID);
	Souls[ownerID]->FlowMgr = this;
	Souls[ownerID]->SetRole(SoulRole::Owner);
	Souls[ownerID]->OnJoined();
}

void FlowManagerBase::OnClientDisconnected(uint8_t ownerID)
{
	if (!Souls[ownerID]) return;

	LOG_NET_INFO_F(Souls[ownerID].get(), "[FlowMgr] OnClientDisconnected: ownerID=%u left", ownerID);

	if (ActiveMode) ActiveMode->OnPlayerLeft(*Souls[ownerID]);

	Souls[ownerID]->OnLeft();
	Souls[ownerID].reset();
}

#ifdef TNX_ENABLE_NETWORK
std::optional<PlayerBeginResult> FlowManagerBase::HandlePlayerBeginRequest(Soul* soul, const PlayerBeginRequestPayload& req)
{
	if (!soul)
	{
		LOG_NET_WARN(nullptr, "[FlowMgr] HandlePlayerBeginRequest: null Soul");
		return std::nullopt;
	}

	PlayerBeginResult result;
	if (ActiveMode)
		result = ActiveMode->OnPlayerBeginRequest(*soul, req);
	else
	{
		result.Accepted = true;
		result.PosX     = req.PosX;
		result.PosY     = req.PosY;
		result.PosZ     = req.PosZ;
		soul->ClaimBody({});
	}

	if (!result.Accepted) return std::nullopt;
	return result;
}

void FlowManagerBase::SendPlayerBeginRequest(NetChannel channel, uint32_t frameNumber, PredictionLedger& ledger)
{
	const uint8_t ownerID           = channel.OwnerID();
	constexpr uint32_t PredictionID = 1;

	if (!Souls[ownerID])
	{
		Souls[ownerID]          = std::make_unique<Soul>(ownerID);
		Souls[ownerID]->FlowMgr = this;
		Souls[ownerID]->SetRole(SoulRole::Owner);
		Souls[ownerID]->OnJoined();
	}
	Souls[ownerID]->Channel = channel;

	PlayerBeginRequestPayload req{};
	req.PrefabID     = 0;
	req.PredictionID = PredictionID;
	req.PosX         = 0.0f;
	req.PosY         = 5.0f;
	req.PosZ         = 0.0f;

	LOG_NET_INFO_F(Souls[ownerID].get(), "[FlowMgr] SendPlayerBeginRequest: ownerID=%u frame=%u", ownerID, frameNumber);

	ledger.Set(PredictionID, frameNumber, {}, req.PrefabID);

	if (!Souls[ownerID]->PlayerBegin(req)) [[unlikely]]
		LOG_NET_WARN_F(Souls[ownerID].get(), "[FlowMgr] PlayerBeginRequest send failed (GNS rejected) — ownerID=%u", ownerID);
}
#endif // TNX_ENABLE_NETWORK

// ---------------------------------------------------------------------------
// PreloadLevelAssets — scan scene JSON and demand-load all named asset refs
// ---------------------------------------------------------------------------

void FlowManagerBase::PreloadLevelAssets(const char* path)
{
	if (!path || path[0] == '\0') return;

	std::ifstream file(path);
	if (!file.is_open()) return;

	std::string contents((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	JsonValue root = JsonParse(contents);
	if (!root.IsObject()) return;

	const JsonValue* entities = root.Find("entities");
	if (!entities || !entities->IsArray()) return;

	AssetRegistry& reg = AssetRegistry::Get();

	for (const JsonValue& entity : entities->AsArray())
	{
		if (!entity.IsObject()) continue;
		const JsonValue* components = entity.Find("components");
		if (!components || !components->IsObject()) continue;

		for (const auto& [compName, compData] : components->AsObject())
		{
			if (!compData.IsObject()) continue;
			for (const auto& [fieldName, fieldValue] : compData.AsObject())
			{
				if (!fieldValue.IsString()) continue;
				const std::string& ref = fieldValue.AsString();
				if (ref.empty()) continue;

				for (const AssetEntry* entry : reg.GetAssetsByName(TnxName(ref.c_str())))
					reg.TriggerLoad(entry->ID);
			}
		}
	}
}
