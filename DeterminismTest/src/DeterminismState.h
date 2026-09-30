#pragma once

#include "FlowState.h"
#include "FlowManagerBase.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "SchemaReflector.h"

#include <string>

/// @brief Standalone @ref FlowState for rollback determinism testing.
///
/// On @c OnEnter, sets @c DeterminismMode and loads the configured default scene
/// so the test begins immediately without a networking handshake.
class DeterminismState : public FlowState
{
public:
	void OnEnter(FlowManagerBase& flow, WorldBase* world) override
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
		auto dot              = sceneName.rfind('.');
		if (dot != std::string::npos) sceneName = sceneName.substr(0, dot);
		Flow->LoadLevelByName(sceneName.c_str());
	}

	void OnExit() override
	{
		Flow->UnloadLevel();
	}

	void Tick(SimFloat dt) override { (void)dt; }

	StateRequirements GetRequirements() const override
	{
		return { .NeedsWorld = true, .NeedsLevel = true };
	}

	const char* GetName() const override { return "DeterminismState"; }
};

TNX_REGISTER_STATE(DeterminismState)
