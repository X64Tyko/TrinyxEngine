#include "Panels/EngineStatsPanel.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "LogicThread.h"
#include "Registry.h"
#include "imgui.h"

void EngineStatsPanel::Draw(EditorState& state)
{
	ImGui::Begin(Title, &bVisible);

	// --- Performance ---
	if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
	{
		float renderFps = ImGui::GetIO().Framerate;
		float renderMs  = 1000.0f / renderFps;

		float logicFps = 0.0f, logicMs = 0.0f, fixedFps = 0.0f, fixedMs = 0.0f;
		if (state.LogicPtr)
		{
			logicFps = state.LogicPtr->GetLogicFPS();
			logicMs  = state.LogicPtr->GetLogicFrameMs();
			fixedFps = state.LogicPtr->GetFixedFPS();
			fixedMs  = state.LogicPtr->GetFixedFrameMs();
		}

		ImGui::Text("Render:  %.0f FPS  (%.2f ms)", renderFps, renderMs);
		ImGui::Text("Logic:   %.0f FPS  (%.2f ms)", logicFps, logicMs);
		ImGui::Text("Fixed:   %.0f FPS  (%.2f ms)", fixedFps, fixedMs);

		uint32_t lastFrame = state.LogicPtr ? state.LogicPtr->GetLastCompletedFrame() : 0;
		ImGui::Text("Logic Frame: %u", lastFrame);
	}

	// --- Entity Stats ---
	if (ImGui::CollapsingHeader("Entities", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (state.RegistryPtr)
		{
			ImGui::Text("Total Entities: %u", state.RegistryPtr->GetTotalEntityCount());
			ImGui::Text("Total Chunks:   %u", state.RegistryPtr->GetTotalChunkCount());
		}
		else
		{
			ImGui::TextDisabled("No registry");
		}
	}

	// --- Engine Config ---
	if (state.ConfigPtr && ImGui::CollapsingHeader("Engine Config"))
	{
		const EngineConfig& cfg = *state.ConfigPtr;

		ImGui::Columns(2, "ConfigColumns", true);
		ImGui::SetColumnWidth(0, 200.0f);

		ImGui::Text("TargetFPS");
		ImGui::NextColumn();
		ImGui::Text("%d%s", cfg.TargetFPS, cfg.TargetFPS == 0 ? " (uncapped)" : "");
		ImGui::NextColumn();

		ImGui::Text("FixedUpdateHz");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.FixedUpdateHz);
		ImGui::NextColumn();

		ImGui::Text("PhysicsUpdateInterval");
		ImGui::NextColumn();
		ImGui::Text("%d (%.0f Hz)", cfg.PhysicsUpdateInterval,
					static_cast<double>(cfg.FixedUpdateHz) / cfg.PhysicsUpdateInterval);
		ImGui::NextColumn();

		ImGui::Text("NetworkUpdateHz");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.NetworkUpdateHz);
		ImGui::NextColumn();

		ImGui::Text("InputPollHz");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.InputPollHz);
		ImGui::NextColumn();

		ImGui::Text("MAX_RENDERABLE_ENTITIES");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.MAX_RENDERABLE_ENTITIES);
		ImGui::NextColumn();

		ImGui::Text("MAX_JOLT_BODIES");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.MAX_JOLT_BODIES);
		ImGui::NextColumn();

		ImGui::Text("MAX_CACHED_ENTITIES");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.MAX_CACHED_ENTITIES);
		ImGui::NextColumn();

		ImGui::Text("TemporalFrameCount");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.TemporalFrameCount);
		ImGui::NextColumn();

		ImGui::Text("JobCacheSize");
		ImGui::NextColumn();
		ImGui::Text("%d", cfg.JobCacheSize);
		ImGui::NextColumn();

		ImGui::Columns(1);
	}

	ImGui::End();
}