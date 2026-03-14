#include "Panels/WorldOutlinerPanel.h"
#include "EditorState.h"
#include "imgui.h"

void WorldOutlinerPanel::Draw(EditorState& /*state*/)
{
	ImGui::Begin(Title, &bVisible);
	ImGui::TextDisabled("(Phase 3)");
	ImGui::End();
}