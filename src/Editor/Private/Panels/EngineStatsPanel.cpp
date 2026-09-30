#include "Panels/EngineStatsPanel.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "LogicThreadBase.h"
#include "Registry.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

void EngineStatsPanel::Draw(EditorState& state)
{
	if (!BeginPadded())
	{
		ImGui::End();
		return;
	}

	TnxWidgets::PanelHeader(nullptr, "Engine Stats");

	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);

	// --- Thread budget bars ---
	float renderFps = ImGui::GetIO().Framerate;
	float renderMs  = renderFps > 0.f ? (1000.0f / renderFps) : 0.f;

	float logicFps = 0.0f, logicMs = 0.0f, fixedFps = 0.0f, fixedMs = 0.0f;
	if (state.LogicPtr)
	{
		logicFps = state.LogicPtr->GetLogicFPS();
		logicMs  = state.LogicPtr->GetLogicFrameMs();
		fixedFps = state.LogicPtr->GetFixedFPS();
		fixedMs  = state.LogicPtr->GetFixedFrameMs();
	}

	const float fixedBudget   = (fixedFps > 0.f) ? (1000.f / fixedFps) : 1.953f; // 512 Hz default
	const float encoderBudget = (renderFps > 0.f) ? (1000.f / renderFps) : 16.67f;

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 10));

	char fixedHz[32];
	snprintf(fixedHz, sizeof(fixedHz), "%.0f Hz", fixedFps);
	TnxWidgets::FrameBudgetBar("Brain", fixedHz, fixedMs, fixedBudget, TnxStyle::Color::ThBrain, "Logic thread — fixed-step simulation");
	ImGui::Spacing();

	char renderHz[32];
	snprintf(renderHz, sizeof(renderHz), "%.0f FPS", renderFps);
	TnxWidgets::FrameBudgetBar("Encoder", renderHz, renderMs, encoderBudget, TnxStyle::Color::ThEncoder, "Render thread — GPU upload + draw");

	ImGui::PopStyleVar();
	ImGui::Separator();

	// --- Entity stats ---
	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
	ImGui::TextUnformatted("ENTITIES");
	ImGui::PopStyleColor();
	ImGui::Spacing();

	if (state.RegistryPtr)
	{
		ImGui::Text("Live:    %u", state.RegistryPtr->GetTotalEntityCount());
		ImGui::Text("Chunks:  %u", state.RegistryPtr->GetTotalChunkCount());
	}
	else
	{
		ImGui::TextDisabled("No registry");
	}

	uint32_t lastFrame = state.LogicPtr ? state.LogicPtr->GetLastCompletedFrame() : 0;
	ImGui::Text("Frame:   %u", lastFrame);

	// --- Engine config (collapsed by default) ---
	ImGui::Separator();
	if (state.ConfigPtr && ImGui::CollapsingHeader("Config"))
	{
		const EngineConfig& cfg = *state.ConfigPtr;

		if (ImGui::BeginTable("cfg", 2, ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 210.f);
			ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

			auto row = [&](const char* key, const char* fmt, auto... args)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
				ImGui::TextUnformatted(key);
				ImGui::PopStyleColor();
				ImGui::TableSetColumnIndex(1);
				char buf[64];
				snprintf(buf, sizeof(buf), fmt, args...);
				ImGui::PushFont(TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont());
				ImGui::TextUnformatted(buf);
				ImGui::PopFont();
			};

			row("TargetFPS", "%d%s", cfg.TargetFPS, cfg.TargetFPS == 0 ? " (uncapped)" : "");
			row("FixedUpdateHz", "%d", cfg.FixedUpdateHz);
			row("PhysicsUpdateInterval", "%d  (%.0f Hz)", cfg.PhysicsUpdateInterval,
				(double)cfg.FixedUpdateHz / cfg.PhysicsUpdateInterval);
			row("NetworkUpdateHz", "%d", cfg.NetworkUpdateHz);
			row("InputPollHz", "%d", cfg.InputPollHz);
			row("MAX_RENDERABLE_ENTITIES", "%d", cfg.MAX_RENDERABLE_ENTITIES);
			row("MAX_JOLT_BODIES", "%d", cfg.MAX_JOLT_BODIES);
			row("MAX_CACHED_ENTITIES", "%d", cfg.MAX_CACHED_ENTITIES);
			row("TemporalFrameCount", "%d", cfg.TemporalFrameCount);
			row("JobCacheSize", "%d", cfg.JobCacheSize);

			ImGui::EndTable();
		}
	}

	ImGui::End();
}
