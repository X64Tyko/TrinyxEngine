#pragma once

#include "FlowState.h"
#include "FlowManagerBase.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "NetTypes.h"
#include "SchemaReflector.h"

#include <string>

// ---------------------------------------------------------------------------
// GameplayState — The default in-game state for Playground.
//
// Requires a World (NeedsWorld = true). On entry, loads the scene specified
// by EngineConfig::DefaultScene — but only if this is the server or standalone.
// Clients wait for TravelNotify from the server, then load the level via
// OnNetEvent so the path is always the authoritative server path.
// ---------------------------------------------------------------------------
class GameplayState : public FlowState
{
public:
	void OnEnter(FlowManagerBase& flow, WorldBase* world) override
	{
		FlowState::OnEnter(flow, world); // caches Flow

		const EngineConfig* cfg = Flow->GetConfig();
		if (cfg->DefaultScene[0] == '\0' || cfg->ProjectDir[0] == '\0')
		{
			LOG_INFO("[GameplayState] No DefaultScene configured — entering empty world");
			return;
		}

		// Owner worlds wait for TravelNotify before loading — the server controls level timing.
		// In PIE, client worlds have LocalOwnerID != 0 (assigned before LoadDefaultState fires).
		if (world && world->GetLocalOwnerID() != 0)
		{
			LOG_INFO("[GameplayState] Client world — deferring level load until TravelNotify");
			return;
		}

		// Activate ArenaMode on all server-role modes before any player joins.
		Flow->SetGameMode("ArenaMode");

		std::string sceneName = cfg->DefaultScene;
		auto dot = sceneName.rfind('.');
		if (dot != std::string::npos) sceneName = sceneName.substr(0, dot);
		Flow->LoadLevelByName(sceneName.c_str());

#if !defined(TNX_NET_MODEL_CLIENT)
		// Route through FlowManager → ArenaMode::OnPlayerJoined for the local player.
		// OwnerID 0 is the standalone/listen-server-local player's Soul.
		// In PIE, client worlds have LocalOwnerID != 0 (set before LoadDefaultState) —
		// they receive the spawn via ConstructSpawn replication, not this direct call.
		if (!world || world->GetLocalOwnerID() == 0) Flow->OnClientLoaded(0);
#endif
	}

	void OnExit() override
	{
		Flow->UnloadLevel();
	}

	void OnNetEvent([[maybe_unused]] uint8_t eventID) override
	{
	}

	void Tick(float dt) override { (void)dt; }

	StateRequirements GetRequirements() const override
	{
		return {.NeedsWorld = true, .NeedsLevel = true, .SweepsAliveFlagsOnServerReady = true};
	}

	const char* GetName() const override { return "GameplayState"; }
};

TNX_REGISTER_STATE(GameplayState)
