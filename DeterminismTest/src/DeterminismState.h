#pragma once

#include "FlowState.h"
#include "FlowManager.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "SchemaReflector.h"

#include <string>

// ---------------------------------------------------------------------------
// DeterminismState — standalone flow state for rollback determinism testing.
//
// On entry: loads the floor scene and immediately spawns the local player
// via DeterminismMode so the test begins without any networking handshake.
// ---------------------------------------------------------------------------
class DeterminismState : public FlowState
{
public:
	void OnEnter(FlowManager& flow, WorldBase* world) override
	{
		FlowState::OnEnter(flow, world);

		const EngineConfig* cfg = Flow->GetConfig();
		if (cfg->DefaultScene[0] == '\0' || cfg->ProjectDir[0] == '\0')
		{
			LOG_INFO("[DeterminismState] No DefaultScene configured — entering empty world");
			return;
		}

		Flow->SetGameMode("DeterminismMode");

		std::string sceneName = cfg->DefaultScene;
		auto dot = sceneName.rfind('.');
		if (dot != std::string::npos) sceneName = sceneName.substr(0, dot);
		Flow->LoadLevelByName(sceneName.c_str());
	}

	void OnExit() override
	{
		Flow->UnloadLevel();
	}

	void Tick(float dt) override { (void)dt; }

	StateRequirements GetRequirements() const override
	{
		return {.NeedsWorld = true, .NeedsLevel = true, .SweepsAliveFlagsOnServerReady = true};
	}

	const char* GetName() const override { return "DeterminismState"; }
};

TNX_REGISTER_STATE(DeterminismState)
