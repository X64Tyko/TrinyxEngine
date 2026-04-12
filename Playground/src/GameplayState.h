#pragma once

#include "FlowState.h"
#include "FlowManager.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "NetTypes.h"
#include "SchemaReflector.h"

#include <string>

#include "World.h"

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
	void OnEnter(FlowManager& flow, World* world) override
	{
		FlowState::OnEnter(flow, world); // caches Flow

		const EngineConfig* cfg = Flow->GetConfig();
		if (cfg->DefaultScene[0] == '\0' || cfg->ProjectDir[0] == '\0')
		{
			LOG_INFO("[GameplayState] No DefaultScene configured — entering empty world");
			return;
		}

		// Clients wait for TravelNotify; server/standalone load immediately.
		if (cfg->Mode == EngineMode::Client)
		{
			LOG_INFO("[GameplayState] Client — deferring level load until TravelNotify");
			return;
		}

		// Activate ArenaMode on all server-role modes before any player joins.
		Flow->SetGameMode("ArenaMode");

		std::string sceneName = cfg->DefaultScene;
		auto dot = sceneName.rfind('.');
		if (dot != std::string::npos) sceneName = sceneName.substr(0, dot);
		Flow->LoadLevelByName(sceneName.c_str());

		if (cfg->Mode == EngineMode::Standalone)
		{
			// Route through FlowManager → ArenaMode::OnPlayerJoined for the local player.
			// OwnerID 0 is the standalone/server-local player's Soul.
			Flow->OnClientLoaded(0);
		}
	}

	void OnExit() override
	{
		Flow->UnloadLevel();
	}

	void OnNetEvent(uint8_t eventID) override
	{
		if (eventID == static_cast<uint8_t>(FlowEventID::TravelNotify))
		{
			const std::string& levelName = Flow->GetPendingTravelPath();
			if (!levelName.empty())
			{
				std::string name = levelName;
				auto dot = name.rfind('.');
				if (dot != std::string::npos) name = name.substr(0, dot);
				LOG_INFO_F("[GameplayState] TravelNotify — loading level '%s' (background)", levelName.c_str());
				Flow->LoadLevelByName(name.c_str(), /*bBackground=*/true);
			}
		}
	}

	void Tick(float dt) override { (void)dt; }

	StateRequirements GetRequirements() const override
	{
		return {.NeedsWorld = true, .NeedsLevel = true};
	}

	const char* GetName() const override { return "GameplayState"; }
};

TNX_REGISTER_STATE(GameplayState)
