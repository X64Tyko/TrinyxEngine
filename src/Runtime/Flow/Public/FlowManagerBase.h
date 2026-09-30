#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "TrinyxJobs.h"

#include "AssetRegistry.h"
#include "ConstructRegistry.h"
#include "NetTypes.h"
#include "RegistryTypes.h"
#include "Soul.h"
#include "Types.h"

class FlowState;
class GameMode;
class StreamingManager;
#ifdef TNX_ENABLE_NETWORK
class NetChannel;
#endif
class WorldBase;
class TrinyxEngine;
struct EngineConfig;

/// @brief Non-template base for @c FlowManager<TNet,TRollback,TFrame>.
///
/// Owns all data and all externally-callable methods. This is the pointer type
/// stored by @c TrinyxEngine, @c EditorContext, @c AuthorityNet, @c OwnerNet,
/// @c FlowState, @c Soul, @c GameMode, and @c Construct.
///
/// One protected pure-virtual: @c CreateWorldImpl() — the only virtual called
/// on state transitions (Sentinel thread, not a hot path). @c LoadLevel is a
/// second override point, used by the concrete template to resolve rollback
/// configuration at compile time.
///
/// @see FlowManager.h for the concrete template derived class.
class FlowManagerBase
{
public:
	FlowManagerBase();
	virtual ~FlowManagerBase();

	FlowManagerBase(const FlowManagerBase&)            = delete;
	FlowManagerBase& operator=(const FlowManagerBase&) = delete;

	/// @brief One-time initialization called by @c TrinyxEngine after construction.
	void Initialize(TrinyxEngine* engine, const EngineConfig* config,
		int windowWidth, int windowHeight);

	// ----- State / Mode registration (call in PostInitialize) -----

	using StateFactory = std::function<std::unique_ptr<FlowState>()>;
	using ModeFactory  = std::function<std::unique_ptr<GameMode>()>;

	/// Register a named state factory. Name is used by LoadState/TransitionTo.
	void RegisterState(const char* name, StateFactory factory);

	/// Register a named mode factory. Name is used by SetGameMode.
	void RegisterMode(const char* name, ModeFactory factory);

	// ----- State stack operations -----

	/// Replace the entire state stack with a single new state.
	void TransitionTo(const char* stateName);

	/// Push an overlay state (pause menu, inventory screen).
	void PushState(const char* stateName);

	/// Pop the top overlay state.
	void PopState();

	/// Load the default state. Called once during engine bootstrap.
	void LoadDefaultState(const char* stateName);

	// ----- World / Level operations -----

	/// Create a fresh World. Non-virtual; calls CreateWorldImpl().
	WorldBase* CreateWorld();

	/// Destroy the current World and everything scoped to it.
	void DestroyWorld();

	/// Start the World's LogicThread.
	void StartWorld();

	/// Signal the World's LogicThread to stop.
	void StopWorld();

	/// Join the World's LogicThread.
	void JoinWorld();

	/// Load a level (.tnxscene) into the current World.
	/// Virtual — overridden by FlowManager<> to use if constexpr(TRollback::Enabled).
	virtual void LoadLevel(const char* levelPath, bool bBackground = false);

	/// Load a level by AssetID — resolves path via AssetRegistry.
	void LoadLevel(const AssetID& id, bool bBackground = false);

	/// Load a level by display name.
	void LoadLevelByName(const char* name, bool bBackground = false);

	/// Unload the current level.
	void UnloadLevel();

	// ----- Soul lifecycle -----

	void OnClientLoaded(uint8_t ownerID);
	void OnLocalOwnerConnected(uint8_t ownerID);
	void OnClientDisconnected(uint8_t ownerID);

	/// @brief Spawn the local player without a network round-trip.
	///
	/// Collapses the Authority's LevelReady → PlayerBegin → Confirm sequence into one
	/// synchronous call for solo/standalone mode. Equivalent outcome: Soul created,
	/// GameMode::OnPlayerBeginRequest called (which spawns the Construct and calls
	/// soul.ClaimBody), then the input accumulator opened.
	///
	/// Requires the Logic thread to be running. Call from a FlowState::OnEnter after
	/// SetGameMode, i.e. from PostStart context or later.
	void SpawnLocalPlayer(uint8_t ownerID = 0);

	/// Called by AuthorityNet when a client sends StreamReady for a non-auto-activate chunk.
	/// Override or wire GameMode logic to call AuthorityNet::SendChunkActivate when ready.
	virtual void OnStreamReady(int64_t /*assetIDRaw*/, uint16_t /*instanceIndex*/, uint8_t /*ownerID*/) {}

	Soul* GetSoul(uint8_t ownerID) const { return Souls[ownerID].get(); }

	Soul* EnsureEchoSoul(uint8_t ownerID)
	{
		if (!Souls[ownerID])
		{
			Souls[ownerID]          = std::make_unique<Soul>(ownerID);
			Souls[ownerID]->FlowMgr = this;
			Souls[ownerID]->SetRole(SoulRole::Echo);
		}
		return Souls[ownerID].get();
	}

	// ----- GameMode -----

	void SetGameMode(const char* modeName);
	GameMode* GetGameMode() const { return ActiveMode.get(); }

	/// True once the pending GameMode's OnPreload() has finished and AreUploadsReady()
	/// returns true. ReplicationSystem gates ServerReady on this.
	/// Returns true immediately when no mode is pending (solo/headless worlds aren't blocked).
	bool IsGameModeReady() const { return PendingMode == nullptr; }

#ifdef TNX_ENABLE_NETWORK
	std::optional<PlayerBeginResult> HandlePlayerBeginRequest(Soul* soul, const PlayerBeginRequestPayload& req);
	void SendPlayerBeginRequest(NetChannel channel, uint32_t frameNumber, PredictionLedger& ledger);
#endif

	// ----- RPC dispatch -----

	void PostNetEvent(uint8_t eventID);
	void PostTravelNotify(const char* levelPath);
	void PostPlayerBeginConfirm(const PlayerBeginConfirmPayload& payload);
	PlayerBeginConfirmPayload GetPendingPlayerBeginConfirm() const { return PendingPlayerBeginConfirm; }
	const std::string& GetPendingTravelPath() const { return PendingTravelPath; }

	// ----- Streaming / net helpers (used by FlowManager<TNet> template) -----

	/// Returns the engine's StreamingManager, or nullptr if not initialised.
	StreamingManager* GetStreamingManager() const;

	/// Returns the active net thread as a void* to avoid including TrinyxEngine.h in the template header.
	/// FlowManager<TNet> static_casts this to the concrete NetThreadType.
	void* GetRawNetThread() const;

	// ----- Tick -----

	void Tick(SimFloat dt);

	// ----- Accessors -----

	FlowState* GetActiveState() const;
	WorldBase* GetWorld() const;
	bool HasWorld() const;
	const EngineConfig* GetConfig() const { return Config; }
	void RewireConfig(const EngineConfig* newConfig) { Config = newConfig; }
	ConstructRegistry* GetConstructRegistry() { return &ConstructReg; }
	const std::string& GetActiveLevelPath() const { return ActiveLevelPath; }
	std::string GetActiveLevelLocalPath() const;

protected:
	static constexpr uint32_t MaxStateStack       = 8;
	static constexpr uint32_t MaxRegisteredStates = 32;
	static constexpr uint32_t MaxRegisteredModes  = 16;

	// State stack (index 0 = bottom, StateStackCount-1 = top/active)
	std::unique_ptr<FlowState> StateStack[MaxStateStack];
	uint32_t StateStackCount = 0;

	struct NamedStateFactory
	{
		const char* Name = nullptr;
		StateFactory Factory;
	};

	struct NamedModeFactory
	{
		const char* Name = nullptr;
		ModeFactory Factory;
	};

	NamedStateFactory RegisteredStates[MaxRegisteredStates];
	uint32_t RegisteredStateCount = 0;

	NamedModeFactory RegisteredModes[MaxRegisteredModes];
	uint32_t RegisteredModeCount = 0;

	ConstructRegistry ConstructReg;
	std::unique_ptr<Soul> Souls[MaxOwnerIDs];

	std::unique_ptr<WorldBase> ActiveWorld;
	std::unique_ptr<GameMode> ActiveMode;
	std::unique_ptr<GameMode> PendingMode;
	TrinyxJobs::JobCounter PendingPreloadCounter;
	std::string ActiveLevelPath;
	std::string PendingTravelPath;
	PlayerBeginConfirmPayload PendingPlayerBeginConfirm{};

	TrinyxEngine* Engine       = nullptr;
	const EngineConfig* Config = nullptr;
	int WindowWidth            = 1920;
	int WindowHeight           = 1080;

	std::atomic<uint32_t> PendingNetEvents{ 0 };

	// Bitmask of ownerIDs that called OnClientLoaded while a mode was preloading.
	// CheckPendingMode() drains this after Initialize() completes.
	uint32_t PendingPlayerJoins = 0;

	// Internal helpers
	void CheckPendingMode();
	StateFactory FindStateFactory(const char* name) const;
	ModeFactory FindModeFactory(const char* name) const;
	void EnforceRequirements(FlowState* currentState, FlowState* nextState);

	/// Scan a level JSON file and demand-load all named asset references via TriggerLoad().
	/// Called by FlowManager::LoadLevel before SpawnFromFile so assets are ready when entities spawn.
	static void PreloadLevelAssets(const char* path);

	/// Implemented by FlowManager<TNet,TRollback,TFrame> — creates the typed World.
	virtual WorldBase* CreateWorldImpl() = 0;
};
