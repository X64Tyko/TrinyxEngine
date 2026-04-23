#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "AssetRegistry.h"
#include "ConstructRegistry.h"
#include "NetTypes.h"
#include "RegistryTypes.h"
#include "Soul.h"
#include "Types.h"

class FlowState;
class GameMode;
#ifdef TNX_ENABLE_NETWORK
class NetChannel;
#endif
class WorldBase;
class TrinyxEngine;
struct EngineConfig;

// ---------------------------------------------------------------------------
// FlowManagerBase — Non-template base for FlowManager<TNet,TRollback,TFrame>.
//
// All data members and ALL externally-called methods live here. This is what
// TrinyxEngine, EditorContext, AuthorityNet, OwnerNet, FlowState, Soul,
// GameMode, and Construct hold as a pointer.
//
// One protected pure-virtual: CreateWorldImpl() — the ONLY virtual called on
// state transitions (Sentinel thread, not hot path).
//
// One virtual override point: LoadLevel(const char*, bool) — overridden by
// FlowManager<> to replace #ifdef TNX_ENABLE_ROLLBACK with if constexpr.
//
// See FlowManager.h for the concrete template derived class.
// ---------------------------------------------------------------------------
class FlowManagerBase
{
public:
	FlowManagerBase();
	virtual ~FlowManagerBase();

	FlowManagerBase(const FlowManagerBase&)            = delete;
	FlowManagerBase& operator=(const FlowManagerBase&) = delete;

	/// Called once by TrinyxEngine after construction.
	void Initialize(TrinyxEngine* engine, const EngineConfig* config,
					int windowWidth, int windowHeight);

	// ----- State registration (code-driven, call in PostInitialize) -----

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

	// ----- Tick -----

	void Tick(float dt);

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
	std::string ActiveLevelPath;
	std::string PendingTravelPath;
	PlayerBeginConfirmPayload PendingPlayerBeginConfirm{};

	TrinyxEngine* Engine       = nullptr;
	const EngineConfig* Config = nullptr;
	int WindowWidth            = 1920;
	int WindowHeight           = 1080;

	std::atomic<uint32_t> PendingNetEvents{0};

	// Internal helpers
	StateFactory FindStateFactory(const char* name) const;
	ModeFactory FindModeFactory(const char* name) const;
	void EnforceRequirements(FlowState* currentState, FlowState* nextState);

	/// Implemented by FlowManager<TNet,TRollback,TFrame> — creates the typed World.
	virtual WorldBase* CreateWorldImpl() = 0;
};
