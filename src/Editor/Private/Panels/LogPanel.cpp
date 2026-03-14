#include "Panels/LogPanel.h"
#include "EditorState.h"
#include "imgui.h"

void LogPanel::Draw(EditorState& /*state*/)
{
	ImGui::Begin(Title, &bVisible);
	ImGui::TextDisabled("(Phase 5)");
	ImGui::End();
}