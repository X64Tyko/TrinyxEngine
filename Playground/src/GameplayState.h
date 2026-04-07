#pragma once

#include "GameState.h"
#include "FlowManager.h"
#include "EngineConfig.h"
#include "TrinyxEngine.h"
#include "Logger.h"
#include "SchemaReflector.h"
#include "PlayerConstruct.h"

#include <string>

#include "World.h"

class PlayerConstruct;
// ---------------------------------------------------------------------------
// GameplayState — The default in-game state for Playground.
//
// Requires a World (NeedsWorld = true). On entry, loads the scene specified
// by EngineConfig::DefaultScene and optionally activates a GameMode.
// ---------------------------------------------------------------------------
class GameplayState : public GameState
{
public:
	void OnEnter(FlowManager& flow, World* world) override
	{
		(void)world;

		// Load the default scene if configured
		const EngineConfig* cfg = TrinyxEngine::Get().GetConfig();
		if (cfg->DefaultScene[0] != '\0' && cfg->ProjectDir[0] != '\0')
		{
			std::string scenePath = std::string(cfg->ProjectDir) + "/content/" + cfg->DefaultScene;
			flow.LoadLevel(scenePath.c_str());

			// In standalone we can just spawn our character.
			if (cfg->Mode == EngineMode::Standalone)
			{
				world->Spawn([world](Registry*)
				{
					world->GetConstructRegistry()->Create<PlayerConstruct>(world);
				});
			}
		}
		else
		{
			LOG_INFO("[GameplayState] No DefaultScene configured — entering empty world");
		}
	}

	void OnExit(FlowManager& flow) override
	{
		flow.UnloadLevel();
	}

	void Tick(float dt) override
	{
		(void)dt;
	}

	StateRequirements GetRequirements() const override
	{
		return {.NeedsWorld = true, .NeedsLevel = true};
	}

	const char* GetName() const override { return "GameplayState"; }
};

TNX_REGISTER_STATE(GameplayState)
