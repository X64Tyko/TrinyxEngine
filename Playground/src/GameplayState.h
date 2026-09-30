#pragma once

#include "FlowState.h"
#include "FlowManagerBase.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "NetTypes.h"
#include "SchemaReflector.h"

#include <string>

/// @brief Default in-game @ref FlowState for Playground.
///
/// On @c OnEnter, the Authority or standalone player loads @c EngineConfig::DefaultScene
/// and activates @c ArenaMode. Owner worlds defer level loading until the server
/// sends @c TravelNotify — guaranteeing that all clients load the same scene path.
class GameplayState : public FlowState
{
public:
	void OnEnter(FlowManagerBase& flow, WorldBase* world) override
	{
		FlowState::OnEnter(flow, world);

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
		auto dot              = sceneName.rfind('.');
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

	void Tick(SimFloat dt) override { (void)dt; }

	StateRequirements GetRequirements() const override
	{
		return { .NeedsWorld = true, .NeedsLevel = true };
	}

	const char* GetName() const override { return "GameplayState"; }
};

TNX_REGISTER_STATE(GameplayState)
