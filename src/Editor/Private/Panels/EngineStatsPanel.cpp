#include "Panels/EngineStatsPanel.h"
#include "EditorState.h"
#include "imgui.h"

void EngineStatsPanel::Draw(EditorState& /*state*/)
{
	ImGui::Begin(Title, &bVisible);
	ImGui::TextDisabled("(Phase 2)");
	ImGui::End();
}