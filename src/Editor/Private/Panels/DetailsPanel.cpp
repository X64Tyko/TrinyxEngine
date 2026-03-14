#include "Panels/DetailsPanel.h"
#include "EditorState.h"
#include "imgui.h"

void DetailsPanel::Draw(EditorState& /*state*/)
{
	ImGui::Begin(Title, &bVisible);
	ImGui::TextDisabled("(Phase 4)");
	ImGui::End();
}